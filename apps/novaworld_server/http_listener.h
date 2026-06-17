#pragma once

#include <novacrypto/epask.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace opennova {
class ConnectionManager;
class UnknownTracker;
namespace db { class Database; }
} // namespace opennova

namespace opennova::server {

struct ServerConfig;
class NwUdpListener;
class SessionStore;

// Crow-backed HTTP listener serving:
//   GET /api/lobbies     — live connections from the ConnectionManager
//   GET /api/games       — DB-backed catalogue
//   GET /api/expansions  — DB-backed catalogue
//   GET /*               — fallback static-file serve from web/dist/
//
// Crow is async + multi-threaded internally; we just hand it a thread to
// own. start() spawns that thread; stop() terminates the Crow loop and
// joins.
class HttpListener {
public:
	HttpListener(ConnectionManager &manager, db::Database &db,
	             NwUdpListener &nw_udp, SessionStore &sessions);
	~HttpListener();

	// Optional unknown-message tracker. When set, the catch-all 404 path
	// records "http" sightings ("<METHOD> /<path>") and /api/unknowns serves
	// the live snapshot. Null is safe (the routes degrade to empty / no-op).
	void set_unknown_tracker(opennova::UnknownTracker *tracker) { tracker_ = tracker; }

	HttpListener(const HttpListener &) = delete;
	HttpListener &operator=(const HttpListener &) = delete;

	bool start(const ServerConfig &config);
	void stop();

	bool running() const { return running_.load(); }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	ConnectionManager &manager_;
	db::Database &db_;
	NwUdpListener &nw_udp_;
	SessionStore &sessions_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	// Round-robin counter for dev-mode user assignment. Each POST
	// /NWLogin.dll from a NEW retail process bumps this and picks
	// players[idx % N]. Same retail process keeps the same user across
	// reconnects via the persistent_to_user_id_ pinning below.
	std::atomic<size_t> next_dev_user_idx_{0};
	// PERSISTENTEXPRESSLOGINDATA cookie → players.id. Retail's IB3 sets
	// this cookie itself (we never set it) and ships it on every HTTP
	// request from the same retail process. Acts as a stable per-process
	// identifier even when our own NWHANDLE cookie is dropped (HTTP/1.0
	// IB3 unreliability) — used by /NWJoin.dll PUB* encoding to recover
	// the joiner's PCID when NWHANDLE doesn't arrive (G.8).
	mutable std::mutex persistent_user_mu_;
	std::unordered_map<std::string, int64_t> persistent_to_user_id_;
	// Per-server-process EPASK params advertised via the EPASK cookie at
	// /nwprepare.dll. Retail echoes the same params back as a form field
	// at POST /NWLogin.dll, so we use these to decrypt the encrypted
	// NAME/PASSWORD/template fields. Generated once at construction.
	opennova::EpaskParams epask_params_;
	// Counter for periodic LoginSession TTL sweeps (every Nth POST).
	std::atomic<uint64_t> login_post_counter_{0};
	// Optional unknown-message tracker (set via set_unknown_tracker). Read
	// by /api/unknowns; written by the catch-all 404 path. Null in tests.
	opennova::UnknownTracker *tracker_ = nullptr;
};

} // namespace opennova::server
