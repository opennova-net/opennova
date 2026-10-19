#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <net/novaworld/db/database.h>
#include <net/npwire/cs_config.h>
#include <net_sockets.h>
#include <net/novaworld/connection/registry.h>  // PeerAddr / PeerAddrHash
#include <net/novaworld/lobby_repository.h>
#include <net/novaworld/lobby_session.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/host_session.h>

namespace opennova {
class ConnectionManager;
class UnknownTracker;
namespace bms { struct File; }
namespace world {
class World;
}
}

namespace opennova::novaworld_server {

// The longest lobby statement a connection may reassemble (service policy, not
// retail's): far past the longest a stock client or host sends (a cookie-heavy
// ClientRequestVerifyResult, a few KB), so a client cannot grow the buffer
// without end; a longer stream is dropped whole.
inline constexpr size_t kLobbyStatementMaxBytes = 64 * 1024;

// Per-connection state owned by NwUdpListener (in addition to the wire-level
// info ConnectionRegistry tracks). Keyed by peer address.
//
// WHY THIS IS NOT A DUPLICATE REGISTRY (checked 2026-07-28; a quality-campaign
// slice proposed folding it into ConnectionRegistry and that would regress):
//   * LIFETIME. THIS record carries what the wire row does not: the cached
//     ServerAuth the 0x42 retransmit leg replays for the live connection's own
//     CI and CK (the D-NET-104 invariant: a ClientAuth repeat replays the cached
//     ServerAuth and never re-mints session material), and the lobby, sequence
//     and reassembly state a stray Hello must not disturb (a Hello re-names an
//     Active row's Hello identity only, D-NET-630).
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
	// The NOVAWORLDUDP connection template the 0x82 advertised: the ordered receive gate's
	// queue bound (field 10), the reply pool (field 11), the reply packet ceiling (field 13).
	CsConfig cs = novaworld_service_cs_config();
	// Inbound runs the ordered gate: only the next sequence dispatches, a later packet is held
	// until its gap closes, and the receive batch's end asks for a surviving gap with a 0x84.
	// Outbound retains every reply's reliable records under its packet sequence until the
	// client's ACK covers it, so a stock client's 0x44 gets them back (D-NET-291).
	SessionSequencing sequencing;
	// When this end last framed a packet (a reply, a push, the send-interval leg's header-only
	// one; not a 0x44 resend), the steady clock in ms: the ACTIVE send-interval leg's base.
	uint64_t last_framed_send_ms = 0;
	// Exact ClientAuth fingerprint and the already-enveloped ServerAuth reply.
	// A lost/delayed 0x82 makes the client retransmit the same 0x42. Replaying
	// these cached bytes keeps SK/SCRK/NWUID stable and, critically, does not
	// rewind sequencing that may already have admitted lobby traffic.
	uint32_t client_ci = 0;
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
	// and only dispatch the assembled payload when FRAG_CONT clears,
	// up to kLobbyStatementMaxBytes.
	ProtocolReassemblyState reassembly{/*buffer=*/{}, /*max_bytes=*/kLobbyStatementMaxBytes};
};

struct ServerConfig;

// A connection's clock stamp (Connection::created_ms / last_seen_ms) as Unix epoch milliseconds,
// as the lobby writes it on its session rows. The stamps are the steady clock in ms, the clock
// the listener stamps and the reap reads, which counts from an unspecified start (on Linux the
// machine's boot), so a stamp read as an epoch time lands in January 1970. This is the wall time
// `unix_now_ms` less the stamp's age on the steady clock at `steady_now_ms`; a stamp ahead of
// `steady_now_ms` (none is) reads as now.
int64_t unix_ms_of_steady(uint64_t steady_ms, uint64_t steady_now_ms, int64_t unix_now_ms);

// How often the receive thread takes the commands addressed to it (lobbydb::take_commands), at
// most, and the most it takes at once; and how often, at most, it writes its sessions' last-heard
// times. Service policy.
inline constexpr uint64_t kCommandDrainMs = 250;
inline constexpr std::size_t kCommandsPerDrain = 64;
inline constexpr uint64_t kSessionSeenWriteMs = 10000;
// A receive batch ends at the empty read (retail's batch boundary, below) or once it took this
// many datagrams, so a flood that never empties the socket cannot hold off the legs that run
// between batches (the in-match pump, the commands, the session times, the instance's heartbeat,
// the send intervals). A malformed datagram is logged once per kBadEnvelopeLogMs at most, with
// the count of those since. Service policy.
inline constexpr std::size_t kReceiveBatchMaxDatagrams = 256;
inline constexpr uint64_t kBadEnvelopeLogMs = 10000;

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
// container path; a title's in-match PN (classify_session_protocol), with that
// title's complete in-match identity, uses the shared authoritative inmatch host
// lifecycle on the same retail UDP session port when the host speaks that title
// (JO's, "JOINTOPERATIONS").
//
// The lobby's memory is its own: nothing outside the listener reads or changes
// it but the ConnectionManager's drops (erase_lobby_state, the on_lost sink) and
// the tests (snapshot_hosted). It keeps a row per lobby session in
// lobby_sessions (lobby_repository.h),
// which the admin's sessions view and the push routes read, and it takes the
// commands other threads address to it from lobby_commands on its receive
// thread, every kCommandDrainMs at most: the ServerCommand / ServerStopHosting
// pushes to a listed server, the admin's end of a session, and an account's
// loss of its listings (forget_owner). So the routes and the supervisor that
// insert them could as well run in another process.
class NwUdpListener {
public:
	explicit NwUdpListener(ConnectionManager &manager);
	~NwUdpListener();

	// Optional DB pool. When set, the receive thread leases one connection for
	// its lifetime (start() waits for it, returning false when it cannot be
	// opened) and gives it to the lobby session to persist host state to
	// active_hosts / host_players (Phase I.2/I.3), writes its session rows and
	// takes its commands on it; erase_lobby_state, which also runs on the main
	// thread (the ConnectionManager's on_lost from tick(), and stop()), writes on
	// the calling thread's own connection. With none (tests that bring up no
	// database) the lobby keeps no session rows and takes no commands.
	void set_db_pool(opennova::db::ConnectionPool *pool) { db_pool_ = pool; }

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

	// The task this lobby runs as (program_usage.h find_usage_task), which its instance row
	// names (lobby_instances.task_id: the admin's link to its usage). Set before start(); ""
	// (the default, the tests') names none.
	void set_task_id(std::string task_id) { task_id_ = std::move(task_id); }

	NwUdpListener(const NwUdpListener &) = delete;
	NwUdpListener &operator=(const NwUdpListener &) = delete;

	// Binds the UDP socket, which the receive thread then owns until stop(),
	// so the port start() reports is the one it serves. With a database, the
	// receive thread then registers this lobby instance, a row of its own
	// (lobbydb::register_instance), at config.lobby_endpoint, which it needs
	// then (never derived from the port it bound): the deployment's one lobby
	// address, which every instance announces alike. It deletes the instances
	// not seen for kInstanceStaleSeconds with their rows
	// (reap_stale_instances) and clears the rows no instance holds
	// (hostdb::clear_unheld), all before it serves. False when a database is
	// set and the config names no endpoint, or when the bind, the DB lease or
	// the registration fails; the lobby program treats that as fatal.
	bool start(const ServerConfig &config);
	// The receive thread joined, every connection dropped (their rows with
	// them), then this instance's row deleted (and whatever still names it).
	void stop();

	// This lobby instance's id (lobby_instances; 0 without a database or
	// before start()).
	int64_t lobby_id() const { return lobby_id_.load(); }
	// The endpoint its instance row names (the gate's udpnovaworld: the lobby
	// address every instance announces).
	const std::string &instance_endpoint() const { return instance_endpoint_; }
	// Marks the instance draining (the lobby's own as it stops; an admin's goes
	// through the row): no gate counts it among the instances that take
	// clients from then on, and the sessions it holds stay. Which lobby a
	// client reaches is the load balancer's (a task leaves it by its
	// deregistration, before its stop). On a connection of the calling
	// thread's; false when it could not. Thread-safe.
	bool drain();

	// Actual port selected by start(), including an OS-assigned port when the
	// configured value is zero.
	uint16_t bound_port() const { return bound_port_; }
	// Whether the in-match JO host runs (ServerConfig::embedded_jo_host); read
	// after start() returns, which waits for the worker to set it up.
	bool embedded_jo_host_running() const { return jo_owner_ != nullptr; }

	// The receive loop's turns so far (one per receive, which waits at most a
	// pump period): the stall watchdog's progress counter (core/health.h).
	// Thread-safe.
	uint64_t loop_turns() const { return loop_turns_.load(std::memory_order_relaxed); }

	// Snapshot of every connection that has issued a ClientHostRequest
	// (and therefore appears in the lobby browser). Thread-safe. The tests'
	// view of the lobby: the service reads lobby_sessions instead.
	struct HostedSnapshot {
		uint32_t connection_id;
		LobbyState lobby;
	};
	std::vector<HostedSnapshot> snapshot_hosted() const;

	// Leaves the commands in lobby_commands while `paused` (the receive thread skips its drain
	// leg), so a test can change a connection or the database between a command's insert and its
	// run. Never paused in service. Thread-safe.
	void pause_command_drain(bool paused) { command_drain_paused_.store(paused); }

	// Drop the per-connection lobby state for the connection at `peer`.
	// Called from the lobby program's ConnectionManager::on_lost handler so
	// heartbeat-timeouts free the lobby (otherwise hosts would persist
	// in /api/hosts forever after a retail process disappears without a
	// GOODBYE). Logs a "[lobby] stopped" line if the dropped entry was
	// hosting. Idempotent.
	//
	// Was keyed by ClientHello.ci before 2026-04-28 (G.7), but two
	// concurrent retail processes both send ci=0x00000001, so the second
	// AUTH overwrote the first's per-connection state. PeerAddr is the
	// only reliable per-process key for loopback testing.
	//
	// The rows go on `db`, the calling thread's own connection, when the
	// caller has it to hand; with none, on the one the calling thread holds
	// its lease on (ConnectionPool::leased_by_this_thread: the on_lost sink
	// runs on the receive thread for a GOODBYE or a replacing Hello, and on
	// main for a timeout or the shutdown), else on one leased for the call.
	// The connection's lobby_sessions row goes with them, and the commands
	// addressed to it (their foreign key cascades).
	void erase_lobby_state(const PeerAddr &peer, const char *reason, db::Database *db = nullptr);

private:
	void run_loop(opennova::net::ScopedSocket socket,
	              std::optional<db::ConnectionPool::Lease> db_conn);

	// One admitted peer (lobby_peers_): its lobby_sessions row (0 until written, and with no
	// database), what that row holds (a write goes out only when it changes), and the registry's
	// last-heard stamp its last_seen_at was last written from.
	struct LobbyPeer {
		int64_t session_id = 0;
		lobbydb::SessionRow written;
		uint64_t written_seen_ms = 0;
	};
	// The peer's session row as it stands: the registry's half from `conn`, the lobby's from its
	// LobbyConnState when it has one. Under lobby_states_mu_.
	lobbydb::SessionRow session_row_locked(const PeerAddr &peer, const Connection &conn) const;
	// Writes the peer's session row when it is new or changed (insert_session / update_session).
	// Under lobby_states_mu_, on the receive thread's connection; a failure is logged and the
	// next change writes again. Nothing without a database.
	void sync_session_locked(const PeerAddr &peer, db::Database *db);
	// The last-heard times that moved since they were written, every kSessionSeenWriteMs, and the
	// rows of no session this lobby holds deleted (a drop's delete that failed).
	void write_sessions_seen(db::Database &db);
	// start()'s instance on the receive thread's connection (register, reap, clear); throws
	// db::Error.
	void register_instance(db::Database &db);
	// The instance's heartbeat, every kInstanceTouchSeconds on the receive thread's connection,
	// which reads its row's state (draining_): a row found gone (reaped while this lobby could
	// not reach the database, its rows with it) registers the instance again
	// (reregister_instance).
	void touch_instance(db::Database &db);
	// The instance under a new row, draining when draining_ says so, and the rows the reap took
	// written anew from the lobby's memory: every admitted peer's session row inserted again (its
	// old id forgotten), every listing upserted again (LobbySession::relist). The lobby serves on
	// throughout; a command the reap took is lost (best effort, as every command). Throws
	// db::Error when the registration fails (the next heartbeat tries again).
	void reregister_instance(db::Database &db);
	// The receive batch's end: a lobby connection that saw a packet past a gap and still holds
	// one asks for the gap with one 0x84.
	void send_missing_requests(opennova::net::Socket &socket);

	// The receive thread's legs between receive batches: the commands addressed to the lobby
	// (every kCommandDrainMs), then the send-interval leg over every lobby connection.
	void drain_commands(opennova::net::Socket &socket, db::Database &db);
	// The peer whose current session row is `session_id`; null when none is (it was dropped, or
	// a fresh 0x42 replaced it under a new row). Under lobby_states_mu_.
	const PeerAddr *peer_of_session_locked(int64_t session_id) const;
	// A ServerCommand / ServerStopHosting: framed on the session's connection exactly as a reply
	// (one reliable record on its sequencing, retained until the client's ACK covers it, so the
	// client's ordered receive and a 0x44 recover a lost one; the send-interval leg makes a gap
	// visible when nothing follows), while the session still hosts and, for an owner's push, its
	// listing is still that account's (in memory, and by the live claim:
	// hostdb::listing_owned_by). A ServerStopHosting is the sysop punt from the host's side: a
	// stock host drops to verified with no ClientStopHosting [orig:
	// CNapiGameSession_HandleServerMessage @0x4d1c50 — state 4, the word to 1], and in a
	// NovaWorld match its next 62-frame block's NovaWorld exit ends the match for everyone, sends
	// the host to the main menu and resets its NWU session [orig: Game_ProcessMainFrame
	// @0x52654f..0x52657c, g_MissionExitReason = 12; the router @0x568552 -> @0x5686cb..0x5686fe];
	// once its record went out framed the connection also leaves the browser
	// (LobbySession::end_hosting). Under lobby_states_mu_.
	void run_push_locked(opennova::net::Socket &socket, db::Database &db,
	                     const lobbydb::Command &command);
	// The admin's End session (service policy: how the retail service ended a session is
	// unwitnessed): the session ends the way the NAPI server role ends a connection it asked to
	// disconnect. The receive thread latches the pending-disconnect record
	// {1, 8, 0, 0, "", 0, "NP.C:PMP:PDESTME"}, sends the client its ServerGoodBye burst keyed by
	// the client's CK (CS field 1 times; a stock client tears its session down at the first
	// one), then the caller drops the connection (ConnectionManager Logout, whose on_lost erases
	// its lobby state). One that never completed its 0x42 is dropped with no burst, as
	// SendDisconnectPacket sends none for it. A session that hosts by now is left as it is (the
	// listing's stop ends it). The address to drop once the lock is released, or none. Under
	// lobby_states_mu_.
	// [orig: NapiNPProtocol_Pump @0x62A650 — the PDESTME latch @0x62a72f..0x62a788, then
	//  CNapiNPConnection_Destroy @0x62A4B0 -> TeardownActiveConnection @0x6253C0, the burst
	//  @0x6253ef..0x625424; CNapiNPConnection_SendDisconnectPacket @0x61F2A0, the conn_flag0 gate
	//  @0x61f30b, opcode 0x86 @0x61f367, the client's key @0x61f3af]
	std::optional<PeerAddr> run_end_locked(opennova::net::Socket &socket,
	                                       const lobbydb::Command &command);
	// Takes `user_id` off every lobby connection (and parked lobby state) whose listing it owns
	// (LobbyState::host_user_id), and off their rows: the account lost its keys, and with them
	// the stored owner (service policy). The listing stays listed; its next write stores no
	// owner. Under lobby_states_mu_.
	void forget_owner_locked(db::Database &db, int64_t user_id);
	void pump_send_intervals(opennova::net::Socket &socket);
	void initialize_jo_host();
	void reset_per_run_state(const char *reason);
	static void observe_jo_event(void *context, const inmatch::HostAcceptEvent &event);

	ConnectionManager &manager_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	std::atomic<uint64_t> loop_turns_{0};
	uint16_t bound_port_ = 0;
	// web_host:http_port, the NovaworldWebDomainNameAndPortNumber CU value
	// (no scheme — the client adds "http://"), as the config start() took
	// names them (ONNET_WEB_HOST, ONNET_HTTP_PORT): web's endpoint,
	// another process's, the same one the gate's startupurl names.
	std::string web_domain_;

	// Layer-4 lobby state. The dispatcher is stateless; per-connection
	// state lives in the map (keyed by PeerAddr — see erase_lobby_state
	// comment above for why CI was the wrong key).
	LobbySession lobby_session_;
	// JO's in-match peers run through the same HostOwner/start_host_session/
	// host_session_pump lifecycle as the game's host. This listener contributes
	// only UDP protocol demultiplexing; the minimal authoritative World makes
	// the complete named-spawn stream reachable.
	std::unique_ptr<world::World> jo_world_;
	std::unique_ptr<bms::File> jo_mission_;
	std::unique_ptr<inmatch::HostOwner> jo_owner_;
	std::unordered_set<PeerAddr, PeerAddrHash> jo_peers_;
	mutable std::mutex lobby_states_mu_;
	// Every peer address this listener admitted onto the lobby route, with its
	// session row. This is deliberately broader than lobby_states_: a valid
	// ClientHello owns a ConnectionManager entry (and a session row) before
	// ClientAuth creates LobbyConnState. Per-run teardown snapshots these exact
	// addresses so a shared manager keeps connections owned by other listeners.
	std::unordered_map<PeerAddr, LobbyPeer, PeerAddrHash> lobby_peers_;
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
	std::atomic<bool> command_drain_paused_{false}; // pause_command_drain
	// The receive thread's own: when it last took its commands and wrote its sessions' times
	// (the steady clock in ms), and when it last logged a drain that failed.
	uint64_t last_command_drain_ms_ = 0;
	uint64_t last_seen_write_ms_ = 0;
	uint64_t last_drain_warning_ms_ = 0;
	// The receive thread's own: when it last logged a malformed datagram, and how many it has
	// not logged since.
	uint64_t last_bad_envelope_log_ms_ = 0;
	uint64_t bad_envelopes_unlogged_ = 0;
	// The lobby instance (start(): lobbydb::register_instance; a new one when
	// the heartbeat registers again), its endpoint, and when the receive
	// thread last moved its last_seen_at.
	std::atomic<int64_t> lobby_id_{ 0 };
	// Whether the instance drains: its own drain() set it, or its heartbeat last read its row
	// draining (an admin's drain). A registration again keeps it.
	std::atomic<bool> draining_{ false };
	std::string instance_endpoint_;
	std::string task_id_; // set_task_id
	uint64_t last_instance_touch_ms_ = 0;
	opennova::db::ConnectionPool *db_pool_ = nullptr;
	opennova::UnknownTracker *tracker_ = nullptr;
};

} // namespace opennova::novaworld_server
