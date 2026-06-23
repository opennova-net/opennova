#pragma once

#include <novaworld/connection/registry.h>  // PeerAddr / PeerAddrHash
#include <novaworld/game_server_runtime.h>
#include <novaworld/protocol_message.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova {

// Host-side accept of a JointOperations in-match join — socket-free and
// Godot-agnostic. This is the consolidated reimpl of the opcode switch that
// used to live ONLY inside apps/novaworld_server's run_loop: both the
// standalone server and the in-engine listen-server (NovaSimulation) drive a
// joiner through the witnessed handshake here, never reinventing the wire.
//   0x41 ClientHello -> 0x81 ServerHello
//   0x42 ClientAuth  -> 0x82 ServerAuth   (per-session SCRK established)
//   0x43 SESSION     -> 0x83 SESSION      (drives GameSession to Spawned)
//   0x46 ClientGoodbye
// On reaching Spawned the owner spawns the joiner's entity + binds it to a
// connection; thereafter NetSystem owns the per-frame S2C 0x0A while the
// joiner's in-match C2S 0x0C is surfaced for the live sim to apply.
//
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00; NapiNPConnection_ParseMessages
//  @0x625BC0; accept loop apps/novaworld_server/nw_udp_listener.cpp run_loop]

// Joiner pose recovered when a peer reaches Spawned. Position is i32 16.16
// world: the joiner's own decoded C2S 0x0C uplink when one has arrived, else
// the host-advertised spawn from the session config (pos_valid distinguishes).
struct HostJoinerPose {
	bool     pos_valid = false;     // false ⇒ from config spawn, not a joiner uplink
	uint16_t entity_handle = 0;     // joiner's claimed pool<<12|slot (informational)
	uint16_t item_type_id = 0x14B9; // player infantry template type
	int32_t  pos_x = 0, pos_y = 0, pos_z = 0;
	int16_t  heading = 0;
	int16_t  pitch = 0;
	uint8_t  team = 1;
};

// One event surfaced by handle_datagram. Owner reacts: PeerSpawned -> admit_peer
// + bind a connection; PeerC2SInMatch -> route each 0x0C into the live sim;
// PeerGoodbye -> drop the connection.
struct HostAcceptEvent {
	enum class Kind { PeerHandshakeAdvanced, PeerSpawned, PeerC2SInMatch, PeerGoodbye };
	Kind kind = Kind::PeerHandshakeAdvanced;
	PeerAddr peer;
	HostJoinerPose pose;                       // valid when kind == PeerSpawned
	std::vector<ProtocolMessage> in_match_c2s; // valid when kind == PeerC2SInMatch
};

class HostSessionAccept {
public:
	explicit HostSessionAccept(GameServerRuntimeConfig config = {});

	void start();   // game_runtime_.start()
	void stop();    // game_runtime_.stop()
	void configure(GameServerRuntimeConfig config); // re-seat mission/spawn; clears peers
	bool running() const { return game_runtime_.running(); }
	const GameServerRuntimeConfig &config() const { return game_runtime_.config(); }

	struct HandleResult {
		std::vector<std::vector<uint8_t>> outbound;  // fully-framed datagrams to send to `peer`
		std::vector<HostAcceptEvent>      events;
	};

	// Decode + dispatch one raw inbound datagram from `peer` (the bytes off the
	// socket, envelope+NWU still on). `now_tick` feeds the game-session tag
	// clock. Returns the outbound datagrams to ship back + events to react to.
	HandleResult handle_datagram(const PeerAddr &peer, const uint8_t *raw, size_t len,
	                             uint32_t now_tick = 0);

	// Drive the periodic emitter for every peer NOT yet Spawned (so
	// entity_batch_count climbs and the spawn gate opens) and frame each
	// session's replies. Owner sends each TickOut.outbound to TickOut.peer.
	// Spawned peers are skipped — NetSystem owns their per-frame 0x0A.
	struct TickOut { PeerAddr peer; std::vector<std::vector<uint8_t>> outbound; };
	std::vector<TickOut> tick_handshakes(int elapsed_ms, uint32_t now_tick);

	// Wrap one in-match S2C inner message (e.g. NetSystem's 0x0A body) into a
	// fully-framed 0x83 SESSION datagram for `peer`, using that peer's live
	// SCRK + sequence state (advances the peer's outbound seq). Returns false
	// if the peer is unknown / pre-auth.
	bool frame_in_match_s2c(const PeerAddr &peer, uint8_t inner_tag,
	                        const std::vector<uint8_t> &inner_body,
	                        std::vector<uint8_t> &out_datagram);

	// True once `peer` has completed handshake + spawn (its connection is live).
	bool peer_spawned(const PeerAddr &peer) const;

	std::size_t peer_count() const { return peers_.size(); }

private:
	struct PeerState {
		std::string pn;             // ClientHello.pn — drives classify_session_protocol
		std::string client_scrk;    // ClientAuth.scrk — decrypts inbound 0x43
		std::string server_scrk;    // our SCRK — encrypts outbound 0x83, echoed in ServerAuth
		uint32_t client_ck = 0;     // ClientAuth.ck → session_id on our S2C
		uint32_t server_sk = 0;     // our ServerAuth.SK
		uint32_t next_outbound_seq = 1;
		uint32_t last_inbound_seq = 0;
		std::string session_id;     // key into GameServerRuntime sessions_ (the peer label)
		bool spawned_announced = false; // edge-latch so PeerSpawned fires once
	};

	static std::string peer_session_id(const PeerAddr &peer); // stable "a.b.c.d:port"
	HostJoinerPose pose_from_session(const GameSessionState &gss) const;
	std::vector<uint8_t> frame_session_replies(PeerState &state,
	                                           const std::vector<ProtocolMessage> &replies);

	GameServerRuntime game_runtime_;
	std::unordered_map<PeerAddr, PeerState, PeerAddrHash> peers_;
};

} // namespace opennova
