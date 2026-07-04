#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <novaworld/connection/registry.h>  // PeerAddr / PeerAddrHash
#include <novaworld/lobby_session.h>
#include <npwire/protocol_message.h>
#include <npruntime/napi_np_protocol.h>     // np::handle_server_datagram / frame_in_match_s2c (P8)
#include <npruntime/server_session.h>       // np::set_connection_mode / create_session (host bring-up)

namespace opennova {
class ConnectionManager;
class UnknownTracker;
namespace db { class Database; }
}

namespace opennova::server {

// Per-connection state owned by NwUdpListener (in addition to the wire-level
// info ConnectionRegistry tracks). Keyed by ConnectionRegistry id.
struct LobbyConnState {
	LobbyState lobby;
	uint32_t next_outbound_seq = 1;
	uint32_t last_inbound_seq = 0;
	// Client's ClientAuth.ck — this is retail's "local_key" per the
	// session_id validation in NapiNPProtocol_HandleSessionPacket
	// (libs/npwire/include/npwire/protocol_message.h notes). We
	// must echo it as the session_id field in S→C SESSION packets so
	// retail's validator accepts them.
	uint32_t client_ck = 0;
	// Per-connection server SK we generated and advertised in
	// ServerAuth.SK. Retail echoes this as session_id on inbound SESSION
	// packets. Was a hardcoded constant before G.2 (2026-04-28).
	uint32_t server_sk = 0;
	// Inbound fragment reassembly state. Multi-packet messages
	// (e.g. ClientRequestVerifyResult ~3.4 KB per
	// notes/retail_capture_findings.md) carry FRAG_CONT (0x04) on
	// every chunk except the final. We accumulate into `reassembly`
	// and only dispatch the assembled payload when FRAG_CONT clears.
	ProtocolReassemblyState reassembly;
};

struct ServerConfig;

// Port-64206 UDP listener for the NAPI NovaWorld session protocol
// (NWU-encrypted, NAPI-CRC-enveloped, opcode-dispatched).
//
// Handles the four session-layer opcodes:
//   0x41 ClientHello   -> 0x81 ServerHello
//   0x42 ClientAuth    -> 0x82 ServerAuth      ("ClientJoin"/"ServerJoin" in onnet)
//   0x43 SESSION       -> 0x83 SESSION (heartbeat ack only for now)
//   0x46 ClientGoodBye -> 0x86 ServerGoodBye + drop the connection
//
// PN dispatch happens at HELLO time. "NOVAWORLDUDP" peers use the lobby
// container path; "JointOperations"/"JOINTOPERATIONS" peers use a World-less
// npruntime session-responder ctx on the same retail UDP session port.
class NwUdpListener {
public:
	explicit NwUdpListener(ConnectionManager &manager);
	~NwUdpListener();

	// Optional DB handle. When set, the lobby session persists host state
	// to active_hosts / host_players (Phase I.2/I.3) and erase_lobby_state
	// removes the corresponding rows.
	void set_database(opennova::db::Database *db) { db_ = db; lobby_session_.set_database(db); }

	// Forward the reflect-endpoint override (the client's NovaWorld session
	// IP:port we advertise to joiners) to the lobby session.
	void set_reflect_endpoint(std::string ip, uint16_t port) {
		lobby_session_.set_reflect_endpoint(std::move(ip), port);
	}

	// Optional unknown-message tracker. When set, unhandled opcodes,
	// unsupported PN strings, unhandled protocol-message types, and "unknown:"
	// lobby containers are recorded (deduped) for /api/unknowns + the
	// unknown_messages table. Null is safe (every hook is guarded).
	void set_unknown_tracker(opennova::UnknownTracker *tracker) { tracker_ = tracker; }

	NwUdpListener(const NwUdpListener &) = delete;
	NwUdpListener &operator=(const NwUdpListener &) = delete;

	bool start(const ServerConfig &config);
	void stop();

	bool running() const { return running_.load(); }

	// Snapshot of every connection that has issued a ClientHostRequest
	// (and therefore appears in the lobby browser). Thread-safe.
	struct HostedSnapshot {
		uint32_t connection_id;
		LobbyState lobby;
	};
	std::vector<HostedSnapshot> snapshot_hosted() const;

	// Drop the per-connection lobby state for the connection at `peer`.
	// Called from main()'s ConnectionManager::on_lost handler so
	// heartbeat-timeouts free the lobby (otherwise hosts would persist
	// in /api/hosts forever after a retail process disappears without a
	// GOODBYE). Logs a "[lobby] stopped" line if the dropped entry was
	// hosting. Idempotent.
	//
	// Was keyed by ClientHello.ci before 2026-04-28 (G.7), but two
	// concurrent retail processes both send ci=0x00000001, so the second
	// AUTH overwrote the first's per-connection state. PeerAddr is the
	// only reliable per-process key for loopback testing.
	void erase_lobby_state(const PeerAddr &peer, const char *reason);

private:
	void run_loop();

	ConnectionManager &manager_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	uint16_t bound_port_ = 0;

	// Layer-4 lobby state. The dispatcher is stateless; per-connection
	// state lives in the map (keyed by PeerAddr — see erase_lobby_state
	// comment above for why CI was the wrong key).
	LobbySession lobby_session_;
	// JointOperations in-match join: a World-less npruntime session-responder ctx (the same legs the
	// in-engine listen server drives via np::handle_server_datagram). It owns the JO peers'
	// handshake/SCRK state + the reactive §5.1 reply config; the lobby (NOVAWORLDUDP) container path
	// below is independent. `jo_peers_` tracks which peers classified as JointOperations at HELLO so
	// 0x42/0x43/0x46 route to the ctx without re-parsing PN each datagram. With no World wired this is a
	// session responder, not a live-sim host — it answers the handshake but never drives a spawn
	// (PeerSpawned does not surface; the real in-match spawn happens on the host's listen server). (P8)
	np::NapiNPServerCtx jo_ctx_;
	std::unordered_set<PeerAddr, PeerAddrHash> jo_peers_;
	// Per-JO-peer dcb (the joiner's NapiNPConnection.unk_18) and pool-0 wire
	// slot, assigned at PeerSpawned and stamped into the S2C 0x0C organic-spawn
	// `entity_flags`/`slot_id`. dcb is taken from the lobby ClientPlayerEnterRequest
	// the joiner sent (correlated by its reported game PortNumber); a join-order
	// counter is the fallback when no lobby entry exists.
	struct JoPeerSpawn { uint32_t dcb = 0; uint16_t slot = 0; bool announced = false; };
	std::unordered_map<PeerAddr, JoPeerSpawn, PeerAddrHash> jo_spawns_;
	uint32_t next_jo_dcb_ = 1;   // host/server reserves 0 (witnessed: dedicatedserver=0)
	uint16_t next_jo_slot_ = 1;  // pool-0 slot counter for joiners
	// game PortNumber -> ConnectionId(dcb), populated from the lobby
	// ClientPlayerEnterRequest (the joiner reports its own unk_18 + game port).
	std::unordered_map<uint16_t, uint32_t> dcb_by_game_port_;
	mutable std::mutex lobby_states_mu_;
	std::unordered_map<PeerAddr, LobbyConnState, PeerAddrHash> lobby_states_;
	opennova::db::Database *db_ = nullptr;
	opennova::UnknownTracker *tracker_ = nullptr;
};

} // namespace opennova::server
