#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <net/novaworld/connection/registry.h>  // PeerAddr / PeerAddrHash
#include <net/novaworld/lobby_session.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/host_session.h>

namespace opennova {
class ConnectionManager;
class UnknownTracker;
namespace db { class Database; }
namespace bms { struct File; }
namespace world {
class World;
}
}

namespace opennova::server {

// Per-connection state owned by NwUdpListener (in addition to the wire-level
// info ConnectionRegistry tracks). Keyed by peer address.
//
// WHY THIS IS NOT A DUPLICATE REGISTRY (checked 2026-07-28; a quality-campaign
// slice proposed folding it into ConnectionRegistry and that would regress):
//   * LIFETIME. A ClientHello re-inserts the peer into ConnectionRegistry as
//     Handshaking, clearing its keys. THIS record survives that, and the 0x42
//     retransmit leg restores the registry from it (notify_active_addr with the
//     cached scrk). That is the D-NET-104 invariant — an exact ClientAuth
//     repeat replays the cached ServerAuth and never re-mints session material.
//     One merged store cannot express "wire row reset, session material kept".
//   * EVICTION IS ALREADY SINGLE. erase_lobby_state is the ConnectionManager
//     on_lost sink as well as the local cleanup hook, so there is one teardown
//     entry point, not two to keep in sync.
//   * COST. ConnectionRegistry::find_by_addr returns Connection BY VALUE and
//     runs per datagram; moving the auth-datagram cache and reassembly buffers
//     into it would copy kilobytes on the hot path.
// lobby_peers_ is likewise deliberate: the ConnectionManager may be shared
// across listeners, so teardown is scoped to the peers this listener admitted.
struct LobbyConnState {
	LobbyState lobby;
	// Inbound keeps the shared no-queue high-water policy (newer packets skip a
	// permanent loss; zero/stale/duplicate packets never redispatch). Outbound
	// retains every reply's reliable records under its packet sequence until the
	// client's ACK covers it, so a stock client's 0x44 gets them back (D-NET-291).
	SessionSequencing sequencing;
	// Exact ClientAuth fingerprint and the already-enveloped ServerAuth reply.
	// A lost/delayed 0x82 makes the client retransmit the same 0x42. Replaying
	// these cached bytes keeps SK/SCRK/NWUID stable and, critically, does not
	// rewind sequencing that may already have admitted lobby traffic.
	uint32_t client_ci = 0;
	std::vector<uint8_t> client_auth_body;
	std::string server_scrk;
	std::vector<uint8_t> server_auth_datagram;
	// Client's ClientAuth.ck — this is retail's "local_key" per the
	// session_id validation in NapiNPProtocol_HandleSessionPacket
	// (engine/net/npwire/protocol_message.h notes). We
	// must echo it as the session_id field in S→C SESSION packets so
	// retail's validator accepts them.
	uint32_t client_ck = 0;
	// Per-connection server SK we generated and advertised in
	// ServerAuth.SK. Retail echoes this as session_id on inbound SESSION
	// packets. Was a hardcoded constant before G.2 (2026-04-28).
	uint32_t server_sk = 0;
	// The reconnect counters the 0x42 handed this connection: DCNT as sent and RCNT one past
	// the client's when DCNT > 0, echoed in the 0x82.
	// [orig: NapiNPProtocol_HandleClientJoin @0x62c28d..0x62c2a3; SendSessionInit @0x62125d]
	uint32_t dcnt = 0;
	uint32_t rcnt = 0;
	// Inbound fragment reassembly state. Multi-packet messages
	// (e.g. ClientRequestVerifyResult ~3.4 KB per
	// docs/net/novaworld-net-re.md) carry FRAG_CONT (0x04) on
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
//   0x43 SESSION       -> 0x83 SESSION (a NOVAWORLDUDP peer's reassembled lobby
//                         containers go through LobbySession::dispatch and each
//                         reply container rides back as one layer-4 message)
//   0x46 ClientGoodBye -> 0x86 ServerGoodBye + drop the connection
//
// PN dispatch happens at HELLO time. "NOVAWORLDUDP" peers use the lobby
// container path; "JointOperations"/"JOINTOPERATIONS" peers use the shared
// authoritative inmatch host lifecycle on the same retail UDP session port.
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

	// Actual port selected by start(), including an OS-assigned port when the
	// configured value is zero.
	uint16_t bound_port() const { return bound_port_; }

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
	void initialize_jo_host();
	void reset_per_run_state(const char *reason);
	static void observe_jo_event(void *context, const inmatch::HostAcceptEvent &event);

	ConnectionManager &manager_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	uint16_t bound_port_ = 0;
	// public_host:http_port, the NovaworldWebDomainNameAndPortNumber CU value
	// (no scheme — the client adds "http://").
	std::string web_domain_;

	// Layer-4 lobby state. The dispatcher is stateless; per-connection
	// state lives in the map (keyed by PeerAddr — see erase_lobby_state
	// comment above for why CI was the wrong key).
	LobbySession lobby_session_;
	// JointOperations peers run through the same HostOwner/start_host_session/
	// host_session_pump lifecycle as the game's host. This listener contributes
	// only UDP protocol demultiplexing; the minimal authoritative World makes
	// the complete named-spawn stream reachable.
	std::unique_ptr<world::World> jo_world_;
	std::unique_ptr<bms::File> jo_mission_;
	std::unique_ptr<inmatch::HostOwner> jo_owner_;
	std::unordered_set<PeerAddr, PeerAddrHash> jo_peers_;
	mutable std::mutex lobby_states_mu_;
	// Every peer address this listener admitted onto the lobby route. This is
	// deliberately broader than lobby_states_: a valid ClientHello owns a
	// ConnectionManager entry before ClientAuth creates LobbyConnState.
	// Per-run teardown snapshots this exact set so a shared manager keeps
	// connections owned by other listeners.
	std::unordered_set<PeerAddr, PeerAddrHash> lobby_peers_;
	std::unordered_map<PeerAddr, LobbyConnState, PeerAddrHash> lobby_states_;
	// A dropped connection's lobby identity, parked for its NWU reconnect: the client keeps its
	// CI and address and re-joins with a fresh CK, counting the disconnect (DCNT > 0); that 0x42
	// takes the parked state back, so its re-host (CurrentlyHosting=1) keeps its RID and GSID.
	// Service policy (the retail service side has no witness); an entry lapses after
	// kParkedLobbyTtlMs or on any fresh join from its address. Guarded by lobby_states_mu_.
	struct ParkedLobbyState {
		uint32_t client_ci = 0;
		LobbyState lobby;
		uint64_t parked_ms = 0;
	};
	std::unordered_map<PeerAddr, ParkedLobbyState, PeerAddrHash> parked_lobby_states_;
	opennova::db::Database *db_ = nullptr;
	opennova::UnknownTracker *tracker_ = nullptr;
};

} // namespace opennova::server
