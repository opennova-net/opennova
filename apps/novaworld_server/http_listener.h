#pragma once

#include <net/novacrypto/epask.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace opennova {
class ConnectionManager;
class UnknownTracker;
namespace db { class ConnectionPool; }
} // namespace opennova

namespace opennova::novaworld_server {

struct ServerConfig;
class SessionStore;

// Crow-backed HTTP listener. start() registers five route families, each in
// its own private registrar (bodies in http_listener.cpp):
//   admin REST API      — /api/admin/* + dev host inject, for the Bearer
//                         ADMIN_API_TOKEN or an admin-role website session
//   public JSON API     — /api/* for the web portal, with the website's
//                         login / logout / me session routes
//   legacy login chain  — retail NW*.dll prepare/start/login/logout/account
//   legacy host/join    — *.gsb browser blobs, /NWJoin.dll, /NWHost.dll
//   static + catch-all  — web/dist, /static/*, bare templates, 404 tracker
//
// Crow is async + multi-threaded internally; we just hand it a thread to
// own. start() spawns that thread and returns once Crow serves; stop()
// terminates the Crow loop and joins. Handlers run on Crow's worker threads
// at once, so each request leases its own connection from `db_pool` for the
// handler's duration.
class HttpListener {
public:
	HttpListener(ConnectionManager &manager, db::ConnectionPool &db_pool,
	             SessionStore &sessions);
	~HttpListener();

	// Optional unknown-message tracker. When set, the catch-all 404 path
	// records "http" sightings ("<METHOD> /<path>") and /api/unknowns serves
	// the live snapshot. Null is safe (the routes degrade to empty / no-op).
	void set_unknown_tracker(opennova::UnknownTracker *tracker) { tracker_ = tracker; }

	HttpListener(const HttpListener &) = delete;
	HttpListener &operator=(const HttpListener &) = delete;

	// Registers the routes and returns once Crow serves them, or false when
	// Crow's run() failed first (the port taken) — main() treats that as fatal.
	bool start(const ServerConfig &config);
	void stop();

	// The port Crow bound and serves (the OS's pick when config.http_port is
	// 0); valid once start() returned true.
	uint16_t bound_port() const { return bound_port_; }

private:
	// Route-family registrars called once from start(), in registration
	// order; the static/catch-all family must stay last (Crow rejects a
	// specific route registered after the /<path> wildcard). Parameters are
	// the config-derived strings the handlers capture by value.
	void register_admin_api_routes(const std::string &public_host);
	void register_public_api_routes();
	void register_legacy_login_routes(const std::string &templates_dir);
	void register_legacy_host_join_routes(
			const std::string &templates_dir);
	void register_static_routes(const std::filesystem::path &web_dist,
	                            const std::string &static_dir,
	                            const std::string &templates_dir);

	// Client-facing URLs injected into the menu templates (@HOST_URL@ /
	// @GSB_SERVER@): config.public_host and the port Crow serves, the
	// configured one or the OS's pick for port 0. The retail client is REMOTE,
	// so these must point at the public host, never 127.0.0.1 (else the client
	// POSTs its host registration / fetches the server browser from its own
	// localhost).
	std::string host_url() const;
	std::string gsb_url() const;

	struct Impl;
	std::unique_ptr<Impl> impl_;
	ConnectionManager &manager_;
	db::ConnectionPool &db_pool_;
	SessionStore &sessions_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	uint16_t bound_port_ = 0;
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
	// config.public_host, set in start() before Crow runs (host_url() /
	// gsb_url()).
	std::string public_host_;
};

} // namespace opennova::novaworld_server
