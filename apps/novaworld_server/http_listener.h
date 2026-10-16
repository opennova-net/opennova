#pragma once

#include "auth.h"
#include "hosted/placement.h"

#include <net/novacrypto/epask.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova {
class ConnectionManager;
class UnknownTracker;
namespace db { class ConnectionPool; class Database; }
} // namespace opennova

namespace opennova::novaworld_server {

struct ServerConfig;
class SessionStore;
class NwUdpListener;
class Watchdog;
namespace hosted { class AdminLinks; class LimitsHold; }
namespace billing { class Billing; }
namespace catalog { struct GameRow; }

// Crow-backed HTTP listener. start() registers six route families, each in
// its own registrar (bodies in http_listener.cpp, the listed servers' in
// host_routes.cpp). The machine agents' routes are never here: they are the
// internal listener's alone (hosted/internal_listener.h).
//   admin REST API      — /api/admin/* + dev host inject, for the Bearer
//                         ADMIN_API_TOKEN or an admin-role website session
//   public JSON API     — /api/* for the web portal, with the website's
//                         login / logout / me session routes, and the
//                         health probe GET /healthz (outside /api)
//   listed servers      — HostRoutes: GET /api/admin/hosts and GET
//                         /api/me/hosts, and the ServerCommand /
//                         ServerStopHosting pushes for an admin
//                         (/api/admin/hosts/<rid>/...) and for the
//                         server's owner (/api/me/hosts/<rid>/...)
//   support tiers       — BillingRoutes (billing/billing_routes.h): the tiers,
//                         the account's billing, checkout and portal, the
//                         Stripe webhook and the admin's billing view
//   legacy login chain  — retail NW*.dll prepare/start/login/logout/account
//   legacy host/join    — *.gsb browser blobs, /NWJoin.dll, /NWHost.dll
//   static + catch-all  — web/dist, /static/*, bare templates, 404 tracker
//
// One lobby per title: start() loads the games catalogue once
// (catalog::GameCatalog), registers each row's .gsb route, and every retail
// page fills in the requesting client's game's values (its gsb URL, its page
// names where the request names none, its expansion bits), the game its
// gate tag names (catalog::GameCatalog::for_client).
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

	// The NovaWorld UDP listener the host routes push statements through
	// (/api/admin/hosts/<rid>/command and /stop, and the owner's
	// /api/me/hosts/<rid>/command and /stop). Set before start(); it must
	// outlive the HTTP listener. Null answers those routes 503.
	void set_nw_udp_listener(NwUdpListener *listener) { nw_udp_ = listener; }

	// The hosted servers' admin links the console routes run their commands on
	// (hosted/console_routes.h). Set before start(); they must outlive the HTTP
	// listener. Null answers every console command 503.
	void set_admin_links(hosted::AdminLinks *links) { admin_links_ = links; }

	// The stall watchdog GET /healthz answers from (health.h). Set before
	// start(); it must outlive the HTTP listener. Null: /healthz answers "ok"
	// whenever Crow serves it.
	void set_watchdog(const Watchdog *watchdog) { watchdog_ = watchdog; }

	// Billing (billing/billing.h), which the billing routes run on
	// (billing/billing_routes.h). Set before start(); it must outlive the HTTP
	// listener. Null: billing is off, and those routes answer billing_disabled
	// (GET /api/billing/tiers answers {"enabled": false}).
	void set_billing(billing::Billing *billing) { billing_ = billing; }

	// The hosted servers' limits hold (hosted/limits_hold.h), which the admin's
	// billing reconcile clears and its billing view shows. Set before start();
	// it must outlive the HTTP listener. Null: none.
	void set_limits_hold(hosted::LimitsHold *hold) { limits_hold_ = hold; }

	// Each container's memory and CPUs (hosted/fleet_config.h's slot limits):
	// the admin usage view's limits. Set before start(); unset, the defaults.
	void set_fleet_slots(int memory_mb, double cpus) {
		fleet_slot_memory_mb_ = memory_mb;
		fleet_slot_cpus_ = cpus;
	}

	// The fleet's placement policy (hosted/fleet_config.h) the hosted routes ask
	// the supervisor's planner with: a start's at_capacity, the list's `full`.
	// Set before start(); unset, placement.h's defaults.
	void set_fleet_placement(const hosted::PlacementPolicy &policy) { fleet_placement_ = policy; }

	HttpListener(const HttpListener &) = delete;
	HttpListener &operator=(const HttpListener &) = delete;

	// Registers the routes and returns once Crow serves them, or false when
	// Crow's run() failed first (the port taken) — main() treats that as fatal.
	bool start(const ServerConfig &config);
	void stop();

	// The port Crow bound and serves (the OS's pick when config.http_port is
	// 0); valid once start() returned true.
	uint16_t bound_port() const { return bound_port_; }

	// The handler threads Crow runs: the core count, at least
	// kMinHandlerThreads. Handlers can wait without working (a hosted server's
	// console request waits on its admin link for up to 3 s), so a two-core
	// machine still runs eight; the console admits at most two fewer
	// waiting requests (hosted/console_routes.h). Each holds at most one of
	// the database pool's connections at a time (DATABASE_POOL_MAX of them);
	// a handler finding every one leased waits for the next one back.
	static constexpr unsigned kMinHandlerThreads = 8;
	static unsigned handler_threads() {
		return (std::max)(std::thread::hardware_concurrency(), kMinHandlerThreads);
	}

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
	// localhost). The browser list is the requesting client's game's
	// (catalog::GameRow::gsb_route: "/jop_2.gsb" for JO's).
	std::string host_url() const;
	std::string gsb_url(const catalog::GameRow &game) const;

	struct Impl;
	std::unique_ptr<Impl> impl_;
	ConnectionManager &manager_;
	db::ConnectionPool &db_pool_;
	SessionStore &sessions_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	uint16_t bound_port_ = 0;
	// PERSISTENTEXPRESSLOGINDATA cookie → the account a retail login with
	// it was made for, with the credentials generation that login read. Retail's
	// IB3 sets this cookie itself (we never set it) and ships it on every HTTP
	// request from the same retail process, so it names that process's login
	// to POST /NWLogin.dll (a resume with no password), /NWJoin.dll and
	// /NWAccount.dll after the LOGINSESSIONTAG (G.8). A pin counts only while
	// its account is still at its generation (pinned_user).
	struct RetailPin {
		int64_t user_id = 0;
		int64_t credentials_gen = 0;
	};
	mutable std::mutex persistent_user_mu_;
	std::unordered_map<std::string, RetailPin> persistent_pins_;
	// The in-memory half of an account's revocation (an admin password reset,
	// a status other than 'active', a delete; the same update moves the
	// account's credentials generation and ends its website sessions and keys,
	// update_revokes_sessions): drops every PERSISTENTEXPRESSLOGINDATA pin to
	// `user_id`, which would resume a retail login with no password (the
	// generation check already refuses such a pin; this frees it), and takes
	// the account off its listings' in-memory owner (NwUdpListener::forget_owner),
	// so an owner push already queued for one of them is dropped.
	void forget_revoked_account(int64_t user_id);
	// The account the pin `cookie` names, while the account is still at the
	// pin's credentials generation; a pin that lost it is dropped. nullopt for
	// an empty or unknown cookie. The account's status is the caller's to check.
	std::optional<UserRecord> pinned_user(const std::string &cookie, db::Database &db);
	// The account a retail request's own login names, for /NWJoin.dll's
	// joiner, /NWAccount.dll's identity and the HOSTKEY /NWHost.dll mints
	// (hostdb::record_host_key): the LOGINSESSIONTAG cookie of a live retail
	// login (its active_user_sessions row, get_user_by_login_session_tag), else
	// the PERSISTENTEXPRESSLOGINDATA pin a login made (pinned_user); `how` names
	// the cookie that resolved it. A row that resolves has its last_seen_at
	// moved to now, so the 2 h sweep measures the login's real idleness. The
	// account's status is the caller's to
	// check. The NWHANDLE cookie never counts: the service sets it in the clear
	// to the account's handle, so any client can send any account's (and a key
	// minted on it would list a server as anyone's). Throws db::Error.
	std::optional<UserRecord> retail_login_user(
			const std::map<std::string, std::string> &cookies, db::Database &db, const char *&how);
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
	// The push channel to listed servers (set_nw_udp_listener), handed to the
	// host routes at start(); null in tests that do not wire one.
	NwUdpListener *nw_udp_ = nullptr;
	// The console routes' admin links (set_admin_links); null in tests that do
	// not wire them.
	hosted::AdminLinks *admin_links_ = nullptr;
	// GET /healthz's watchdog (set_watchdog); null: always "ok".
	const Watchdog *watchdog_ = nullptr;
	// The billing routes' billing (set_billing); null: billing off.
	billing::Billing *billing_ = nullptr;
	// The hosted servers' limits hold (set_limits_hold); null: none.
	hosted::LimitsHold *limits_hold_ = nullptr;
	// The slots' limits (set_fleet_slots).
	int fleet_slot_memory_mb_ = 512;
	double fleet_slot_cpus_ = 1.0;
	// The hosted routes' placement policy (set_fleet_placement).
	hosted::PlacementPolicy fleet_placement_;
	// config.public_host, set in start() before Crow runs (host_url() /
	// gsb_url()).
	std::string public_host_;
};

} // namespace opennova::novaworld_server
