#pragma once

#include "core/health.h"
#include "program/catalog_repository.h"

#include <net/novaworld/db/database.h>
#include <net/novaworld/lobby_repository.h>
#include <net_sockets.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <mutex>
#include <thread>

namespace opennova {
class UnknownTracker;
}

namespace opennova::novaworld_server {

struct ServerConfig;

// Port-7597 UDP listener implementing the simple GATEPROTOCOL bootstrap
// (NWU-encrypted text-tag request -> VAR-encoded plain-text response).
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
// The reply is the same for every client but its reflected pair (and its
// TimeOnGateServer clock).
//
// One lobby per gate tag: the probe's tag selects its `games` row
// (catalog::GameCatalog::by_gate_tag), which supplies the reply's LobbyName,
// its startupurl's query and its GLSVSS leg (the configured one standing in
// for a row with none); a tag no row answers to gets no reply. The
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
	// How long, at least, between two reads of the games catalogue a probe with a tag no row has
	// makes (an account's own game's lobby, which its first publish adds,
	// products/product_repository.h publish_release, answers with no restart); a test's shorter
	// one, before start().
	static constexpr uint64_t kGamesRefreshMs = 30 * 1000;
	void set_games_refresh_ms(uint64_t ms) { games_refresh_ms_ = ms; }

	GateListener();
	~GateListener();

	GateListener(const GateListener &) = delete;
	GateListener &operator=(const GateListener &) = delete;

	// Optional unknown-message tracker. When set, gate probes carrying a
	// tag no games row answers to are recorded (deduped) for /api/unknowns.
	// Null is safe.
	void set_unknown_tracker(opennova::UnknownTracker *tracker) { tracker_ = tracker; }

	// Optional DB pool. When set, the receive thread leases one connection for
	// its lifetime and reads the games catalogue and the lobby instances on it
	// (start() waits for the first). Without a pool the catalogue is empty and
	// every probe goes unanswered.
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
	// the configured port is 0).
	uint16_t bound_port() const { return bound_port_; }

	// The games catalogue as the receive thread holds it now (the lobbies the gate answers for).
	catalog::GameCatalog games() const {
		std::lock_guard<std::mutex> lock(games_mu_);
		return games_;
	}

	// The receive loop's turns so far (one per datagram or 250 ms wait): the
	// stall watchdog's progress counter (core/health.h). Thread-safe.
	uint64_t loop_turns() const { return loop_turns_.load(std::memory_order_relaxed); }

	// The gate's heartbeat fields (core/health.h), from the main loop's connection:
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
	// What UDPNOVAWORLD advertises with no instance active, as the config
	// start() took names it (ONNET_LOBBY_ENDPOINT): the lobbies are other
	// processes, whose bound ports this one never learns.
	std::string lobby_endpoint_; // the fallback with no instance active ("": none)
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
	// Loaded by the receive thread before start() returns, and again when a
	// probe names a tag no row has, games_refresh_ms_ after the last read at
	// the soonest. The receive thread alone writes it, under games_mu_, and
	// reads it without; games() copies it under the lock.
	mutable std::mutex games_mu_;
	catalog::GameCatalog games_;
	uint64_t games_read_ms_ = 0;
	uint64_t games_refresh_ms_ = kGamesRefreshMs;
};

} // namespace opennova::novaworld_server
