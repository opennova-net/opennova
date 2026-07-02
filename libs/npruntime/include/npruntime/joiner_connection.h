#pragma once

#include "npruntime/napi_np_connection.h"

#include <novaworld/client_session.h>   // ClientSession::Config (shared JO identity)
#include <novaworld/ingame_decode.h>     // OrganicSpawnBatch / PlayerExtendedUplink / EntityPacketSubHeader
#include <novaworld/protocol_message.h>  // ProtocolMessage

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// P2 — the CLIENT mirror of the np server legs. JoinerConnection is the bytes-in / bytes-out state
// machine that drives a JointOperations in-match JOIN from the joiner side, promoted verbatim from
// novaworld::JoinerSession onto a client-side NapiNPConnection (type 2) that holds its SCRK/seq/ack.
// It is the symmetric inverse of the np server legs (this class crafts what handle_server_datagram
// parses, and parses what frame_session_replies frames), so the two are unit-testable against each
// other in-process with no sockets.
//
// It does NOT compose ClientSession: ClientSession's post-0x82 path is the matchmaking lobby-verify
// flow, the WRONG channel for the in-match game connection. The game connection goes straight from
// ServerAuth into the witnessed in-match C2S burst that trips the host's spawn gate. Only
// ClientSession::Config (the JointOperations identity preset) is reused.
//
//   start()                         -> ClientHello (0x41)   [Idle -> Hello]
//   <- ServerHello (0x81)             : learn host key hk; send ClientAuth (0x42)
//   <- ServerAuth  (0x82)             : learn server key sk + server scrk (cr==1)  [-> Driving]
//   pump() x4                         : 0x37 -> 0x09 -> 0x22 -> (0x2F,0x2F,0x0B)
//   <- S2C 0x0C organic-spawn (0x83)  : find the record whose entity_name == our player name ->
//                                       learn self wire handle H, cache spawn pose  [-> InMatch]
//   frame_c2s_uplink(H, ...)          : per-frame C2S 0x0C player uplink
//
// SELF-IDENTIFICATION = NAME-MATCH (D.0, docs/net §5.23): the host streams the joiner's admitted
// pool-0 player entity (type 0x14B9) as a named S2C 0x0C organic-spawn record whose entity_name is
// the joiner's player name. The joiner matches it against its own name and adopts record.slot_id as
// its wire handle H (the value it stamps in its C2S 0x0C sub-header so the host's
// apply_player_intent resolves the right peer). The joiner sends NO C2S 0x0C before it knows H.
//
// [orig: NapiNPClientMsg_0x00C @0x42E730 (self name-match), NapiNPClientMsg_0x00F @0x42E200
//  (world-state-load), the joiner C2S in-match burst from the host_and_join_lan capture]. No socket
// I/O lives here — the owner pumps bytes.
namespace opennova::np {

class JoinerConnection {
public:
	enum class Phase {
		Idle,     // nothing sent yet
		Hello,    // ClientHello sent, awaiting ServerHello
		Auth,     // ClientAuth sent, awaiting ServerAuth
		Driving,  // ServerAuth accepted; pump() drives the in-match spawn-gate burst
		InMatch,  // name-matched our organic-spawn record -> self handle H learned
		Error,    // protocol/envelope error or server rejection
	};

	// The joiner's own spawn, learned from the named S2C 0x0C organic-spawn record.
	struct SelfSpawn {
		int32_t  pos_x = 0, pos_y = 0, pos_z = 0; // i32 16.16 world
		int32_t  orientation = 0;                 // 32-bit BAM
		uint8_t  team = 0;
		uint16_t item_type_id = 0x14B9;
		uint16_t net_id = 0;                      // SSN (distinct from the wire handle H)
	};

	struct PollResult {
		std::vector<std::vector<uint8_t>> outbound;   // datagrams to send back to the host
		std::vector<std::vector<uint8_t>> inbound_0a; // raw S2C 0x0A bodies (for NetClientView)
		// Raw S2C world-stream spawn/static bodies the host sends during load, as (tag, body):
		// 0x0C organics, 0x0D pool-1, 0x10 statics, 0x20 markers.
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> inbound_world;
		bool reached_in_match = false;                // true on the datagram that learns H
	};

	// `player_name` is BOTH the on-wire ClientHello.co (the host echoes it into the organic-spawn
	// entity_name) AND the local key the joiner name-matches against.
	JoinerConnection(ClientSession::Config config, std::string player_name);

	// Begin the handshake. Returns the ClientHello datagram to send (Idle -> Hello).
	std::vector<uint8_t> start();

	// Feed one inbound datagram (off the socket, CRC envelope intact). Returns the reply datagrams +
	// any surfaced 0x0A bodies + whether this datagram learned H.
	PollResult handle_datagram(const uint8_t *raw, std::size_t len);

	// Emit the NEXT stage of the witnessed in-match spawn-gate burst (one datagram per call; empty
	// once all stages are sent or before Driving). `now_tick` is reserved for future frame-pacing.
	std::vector<std::vector<uint8_t>> pump(uint32_t now_tick);

	// Build a C2S 0x0C player uplink datagram: 5-byte sub-header (handle = H, item_type_id = type,
	// sub_op = 0x0A extended) + the 43-byte extended body, wrapped as one 0x43 SESSION packet.
	std::vector<uint8_t> frame_c2s_uplink(uint16_t handle_H, uint16_t type,
	                                      const PlayerExtendedUplink &body);

	// Wrap one inner {tag,body} as a single 0x43 SESSION datagram over this connection's live SCRK +
	// seq (advances the outbound seq, stamps the ack). The framing path the per-frame housekeeping
	// rides (0x34 keepalive / 0x4C net-quality / 0x2C RTT, P6 §5.44) so those messages share the SAME
	// 0x43/SCRK envelope and seq stream as the 0x0C uplink — the witnessed PumpClientProtocolSend flush
	// bundles them into 0x43s the same way. [orig: CNapiNetwork_QueueReliableMessage @0x4c4fa0 ->
	// CNapiNPConnection_QueueMessage @0x628640]
	std::vector<uint8_t> frame_inner(uint8_t tag, std::vector<uint8_t> body) {
		return frame_session({make_protocol_message(tag, std::move(body))});
	}

	// Deterministic golden replay: force the in-match connection state so frame_c2s_uplink
	// reproduces a CAPTURED C2S 0x0C datagram byte-for-byte (the ROADMAP "Determinism" seed-inject).
	// session_id = the captured 0x43 header session_id (= ServerAuth.sk); client_scrk = the captured
	// client SCRK (its ClientAuth.scrk); next_seq/last_ack = the captured datagram's seq/ack;
	// self_handle/self_type = its 0x0C sub-header. server_scrk lets the same runtime also decode the
	// capture's S2C 0x83 stream. Skips the live handshake — for replay/parity only.
	void seed_in_match(uint32_t session_id, std::string client_scrk, std::string server_scrk,
	                   uint32_t next_seq, uint32_t last_ack, uint16_t self_handle, uint16_t self_type);

	Phase phase() const { return phase_; }
	bool in_match() const { return phase_ == Phase::InMatch; }
	bool has_self_handle() const { return has_self_handle_; }
	uint16_t self_handle() const { return self_handle_; } // the wire handle H
	const SelfSpawn &spawn_pose() const { return spawn_; }
	const std::string &player_name() const { return player_name_; }
	uint32_t server_key() const { return conn_.server_sk; }
	const std::string &client_scrk() const { return conn_.client_scrk; }
	const std::string &server_scrk() const { return conn_.server_scrk; }
	const std::string &last_error() const { return last_error_; }

	// The client-side connection node carrying this joiner's SCRK/seq/ack (type 2).
	const NapiNPConnection &connection() const { return conn_; }

private:
	std::vector<uint8_t> build_client_hello();
	std::vector<uint8_t> build_client_auth();
	std::vector<uint8_t> frame_session(const std::vector<ProtocolMessage> &messages);

	void on_server_hello(const std::vector<uint8_t> &body, PollResult &out);
	void on_server_auth(const std::vector<uint8_t> &body, PollResult &out);
	void on_server_session(const std::vector<uint8_t> &body, PollResult &out);
	void fail(std::string reason);

	ClientSession::Config cfg_;
	std::string player_name_;
	Phase phase_ = Phase::Idle;

	// SCRK / seq / ack live on the client-side connection node (folded, as on the server side).
	NapiNPConnection conn_;

	uint32_t server_hk_ = 0;    // ServerHello.hk — echoed in ClientAuth.hk (transient)
	int pump_stage_ = 0;        // cursor into the in-match spawn-gate burst stages

	bool has_self_handle_ = false;
	uint16_t self_handle_ = 0;  // wire handle H, learned via the name-match (pool<<12|slot)
	SelfSpawn spawn_;

	std::string last_error_;
};

} // namespace opennova::np
