#pragma once

#include "catalog_repository.h"
#include "health.h"

#include <net/novaworld/db/database.h>
#include <net/novaworld/lobby_repository.h>
#include <net_sockets.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace opennova {
class UnknownTracker;
}

namespace opennova::novaworld_server {

struct ServerConfig;

// Port-7597 UDP listener implementing the simple GATEPROTOCOL bootstrap
// (NWU-encrypted text-tag request -> VAR-encoded plain-text response) and,
// on the same socket, the POSTIPADDRESS:POSTIPPORT sink for the plaintext
// host-status heartbeat a hosting retail client posts every ~30 s
// [orig: Lobby_UpdateServerInfo @0x4ff448..0x4ff62c]. policy: the gate
// advertises its own port as POSTIPPORT so no second port needs opening;
// the blob is told apart from a probe by its "HostKey =" preamble before
// the NWU tag decrypt runs.
//
// The lobby it hands each client (udpnovaworld): the address every lobby
// instance announces, the deployment's one lobby address (on ECS the load
// balancer's, which spreads the clients across the lobby tasks and keeps a
// session's flow on one), while at least one instance takes clients
// (lobbydb::live_lobbies, read again every lobby_refresh_ms on the receive
// thread's connection, kLobbyRetryFactor times as long after a read that
// failed); with none, or while its last read failed (an instance read before
// may have drained and stopped since), the gate's fallback
// (ONNET_LOBBY_ENDPOINT, optional), and with no fallback the probe goes
// unanswered (logged every kNoLobbyLogMs at most, and counted in the
// heartbeat's probes_unanswered), which the client meets as a lost reply.
// The reply is the same for every client but its reflected pair:
// POSTIPADDRESS is this gate, and the web fields name the one web endpoint.
//
// One lobby per title: the probe's tag selects its `games` row
// (catalog::GameCatalog::by_gate_tag), which supplies the reply's LobbyName,
// its startupurl's query and its GLSVSS leg (the configured one standing in
// for a row with none); a tag no row (nor alias) answers to gets no reply. The
// receive thread loads the catalog once from its leased connection before it
// serves, so it answers a probe without touching the database.
//
// Runs on its own thread. start() returns immediately; the listener stays
// up until stop() is called or the destructor runs. Designed to be cheap
// to instantiate and own — the gate program owns one per process.
class GateListener {
public:
	// How long a read of the live instances serves before the next probe
	// reads them again (lobbydb::kGateRefreshSeconds); after a read that
	// failed (the fallback served meanwhile), this many times as long: a read
	// while the database is out blocks this thread up to its connect timeout.
	static constexpr uint64_t kLobbyRefreshMs =
			static_cast<uint64_t>(lobbydb::kGateRefreshSeconds) * 1000;
	static constexpr uint64_t kLobbyRetryFactor = 6;
	// How often, at most, a probe with no lobby to hand out is logged.
	static constexpr uint64_t kNoLobbyLogMs = 10000;
	// A test's shorter refresh; before start().
	void set_lobby_refresh_ms(uint64_t ms) { lobby_refresh_ms_ = ms; }

	GateListener();
	~GateListener();

	GateListener(const GateListener &) = delete;
	GateListener &operator=(const GateListener &) = delete;

	// Optional unknown-message tracker. When set, gate probes carrying a
	// tag no games row answers to are recorded (deduped) for /api/unknowns.
	// Null is safe.
	void set_unknown_tracker(opennova::UnknownTracker *tracker) { tracker_ = tracker; }

	// Optional DB pool. When set, the receive thread leases one connection for
	// its lifetime and reads the games catalogue on it (start() waits for
	// both), and a received host-status blob refreshes the active_hosts row
	// that owns its HostKey on it (hostdb::apply_status_blob). Without a pool
	// the catalogue is empty and every probe goes unanswered.
	void set_db_pool(opennova::db::ConnectionPool *pool) { db_pool_ = pool; }

	// Bind the UDP socket and spawn the receive loop, which leases its DB
	// connection, loads the games catalogue and owns the socket and the
	// connection until stop(). Returns false if the socket couldn't be bound
	// (port in use, perms, etc.) or the connection couldn't be opened or read
	// — main() should treat that as fatal.
	bool start(const ServerConfig &config);

	// Signal stop and join the worker thread. Idempotent.
	void stop();

	// The port start() bound and the receive loop serves (the OS's pick when
	// the configured port is 0); the gate response advertises it as POSTIPPORT.
	uint16_t bound_port() const { return bound_port_; }

	// The games catalogue start() loaded (the lobbies the gate answers for).
	const catalog::GameCatalog &games() const { return games_; }

	// The receive loop's turns so far (one per datagram or 250 ms wait): the
	// stall watchdog's progress counter (health.h). Thread-safe.
	uint64_t loop_turns() const { return loop_turns_.load(std::memory_order_relaxed); }

	// The gate's heartbeat fields (health.h), from the main loop's connection:
	// probes_unanswered, the probes left unanswered for want of a lobby (none
	// takes clients and no fallback) since the last call, and lobbies_live, the
	// lobby instances that take clients now, behind the one lobby address (read
	// on `db`; a read that throws leaves the field out, the count already
	// taken). A gate that answers no one is otherwise healthy: its loops turn
	// and its /livez says ok.
	void heartbeat(db::Database &db, Heartbeat &beat);

private:
	void run_loop(opennova::net::ScopedSocket socket,
	              std::optional<db::ConnectionPool::Lease> db_conn);

	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	std::atomic<uint64_t> loop_turns_{0};
	// The probes left unanswered for want of a lobby since heartbeat() last
	// took them (the receive thread adds, the main loop's heartbeat takes).
	std::atomic<uint64_t> probes_unanswered_{ 0 };
	uint16_t bound_port_ = 0;
	std::string public_host_;
	// What UDPNOVAWORLD and STARTUPURL advertise, as the config start() took
	// names them (ONNET_LOBBY_ENDPOINT; ONNET_WEB_HOST and ONNET_HTTP_PORT):
	// the lobbies and web are other processes, whose bound ports this one
	// never learns.
	std::string lobby_endpoint_; // the fallback with no instance active ("": none)
	std::string web_host_;
	uint16_t http_port_ = 0;
	// The receive thread's own: the live instances' address and count (none
	// after a read that failed) and when it read them (the steady clock in ms;
	// 0: never), and whether its last read failed (logged once until one
	// succeeds).
	uint64_t lobby_refresh_ms_ = kLobbyRefreshMs;
	lobbydb::LiveLobbies live_lobbies_;
	uint64_t lobbies_read_ms_ = 0;
	bool lobbies_read_failed_ = false;
	// The probes left unanswered for want of a lobby since the last line that
	// said so, and when that line went (the steady clock in ms; 0: never).
	uint64_t no_lobby_dropped_ = 0;
	uint64_t no_lobby_logged_ms_ = 0;
	// Reflection override (ONNET_CLIENT_REFLECT_IP/PORT). When set, advertised
	// as ReflectedIpAddress/Port instead of the observed source. 0 port = unset.
	std::string reflect_ip_;
	uint16_t reflect_port_ = 0;
	// MET endpoint + GLSVSS parameters (ServerConfig); emitted when configured.
	std::string met_ip_;
	uint16_t    met_port_ = 0;
	std::string met_label_;
	int         met_ping_ = 0;
	int         met_ext_  = 0;
	std::string glsvss_request_;
	int         glsvss_rims_  = 0;
	int         glsvss_agrms_ = 0;
	opennova::UnknownTracker *tracker_ = nullptr;
	opennova::db::ConnectionPool *db_pool_ = nullptr;
	// Loaded in start(), before the receive thread exists, and never written
	// after: the thread reads it without a lock.
	catalog::GameCatalog games_;
};

} // namespace opennova::novaworld_server
