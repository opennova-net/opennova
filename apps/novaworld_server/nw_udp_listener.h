#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <novaworld/connection/registry.h>  // PeerAddr / PeerAddrHash
#include <novaworld/lobby_session.h>
#include <novaworld/protocol_message.h>

namespace opennova {
class ConnectionManager;
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
	// (libs/novaworld/include/novaworld/protocol_message.h notes). We
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
// PN dispatch happens at HELLO time — only "NOVAWORLDUDP" peers are
// accepted. Game-protocol (PN="JointOperations") clients should be hitting
// a future per-match game server, not us.
class NwUdpListener {
public:
	explicit NwUdpListener(ConnectionManager &manager);
	~NwUdpListener();

	// Optional DB handle. When set, the lobby session persists host state
	// to active_hosts / host_players (Phase I.2/I.3) and erase_lobby_state
	// removes the corresponding rows.
	void set_database(opennova::db::Database *db) { db_ = db; lobby_session_.set_database(db); }

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
	mutable std::mutex lobby_states_mu_;
	std::unordered_map<PeerAddr, LobbyConnState, PeerAddrHash> lobby_states_;
	opennova::db::Database *db_ = nullptr;
};

} // namespace opennova::server
