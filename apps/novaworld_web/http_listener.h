#pragma once

#include "auth.h"
#include "hosted/placement.h"
#include "jobs/scheduler.h"
#include "retail_pins.h"
#include "session_store.h"

#include <net/novacrypto/epask.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opennova {
class UnknownTracker;
namespace db { class ConnectionPool; class Database; }
} // namespace opennova

namespace opennova::novaworld_server {

struct ServerConfig;
class RedisPool;
class Watchdog;
class ObjectStore;

namespace billing { class Billing; }
namespace catalog { struct GameRow; }
namespace infra { class InfraStatus; }

// Crow-backed HTTP listener. start() registers its route families, each in
// its own registrar (bodies in http_listener.cpp, the listed servers' in
// host_routes.cpp). The machine agents' routes are never here: they are the
// internal listener's alone (hosted/internal_listener.h).
//   admin REST API      — /api/admin/* + dev host inject, for the Bearer
//                         ADMIN_API_TOKEN or an admin-role website session
//   public JSON API     — /api/* for the web portal, with the website's
//                         login / logout / me session routes, the password
//                         reset routes (password_reset_routes.h), and the
//                         health probes GET /livez and GET /healthz
//                         (outside /api)
//   listed servers      — HostRoutes: GET /api/admin/hosts and GET
//                         /api/me/hosts, and the ServerCommand /
//                         ServerStopHosting pushes for an admin
//                         (/api/admin/hosts/<rid>/...) and for the
//                         server's owner (/api/me/hosts/<rid>/...); with
//                         ConnectionRoutes, the admin's sessions view.
//                         Each reads and addresses the lobby through its
//                         tables alone (lobby_repository.h): the HTTP
//                         listener holds none of the lobby's memory
//   support tiers       — BillingRoutes (billing/billing_routes.h): the tiers,
//                         the account's billing, checkout and portal, the
//                         Stripe webhook and the admin's billing view
//   background jobs     — JobRoutes (jobs/job_routes.h): the admin's view of
//                         the job queue and its actions on it
//   infrastructure      — InfraRoutes (infra/infra_routes.h): the admin's
//                         read-only view of the deployment as it runs (the
//                         programs' ECS services and tasks, the balancer, the
//                         database, the cache, the lobbies, the fleet,
//                         the machines)
//   game data           — DataSetRoutes and ReleaseRoutes (data_sets/): the
//                         hosted data sets, their tree uploads into the object
//                         store and the update releases, each part through
//                         this program (ADR 0064)
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
// handler's duration. What a later request reads back (the retail sessions and
// pins, the rate limiters' buckets) lives in `redis`, never in this process, so
// any number of listeners on one database and one Redis serve the same clients.
class HttpListener {
public:
	HttpListener(db::ConnectionPool &db_pool, RedisPool &redis);
	~HttpListener();

	// Optional unknown-message tracker. When set, the catch-all 404 path
	// records "http" sightings ("<METHOD> /<path>"); /api/unknowns reads the
	// unknown_messages rows every process flushes into, never a tracker's
	// memory. Null is safe (no sighting is recorded).
	void set_unknown_tracker(opennova::UnknownTracker *tracker) { tracker_ = tracker; }

	// The stall watchdog GET /livez answers from (health.h), and GET /healthz
	// with Redis's PING (redis_answers()). Set before start(); it must outlive
	// the HTTP listener. Null: /livez answers "ok", /healthz from the PING
	// alone.
	void set_watchdog(const Watchdog *watchdog) { watchdog_ = watchdog; }

	// How long GET /healthz reuses Redis's last answer to its PING, so the load
	// balancer's and the uptime checks' probes cost Redis one PING a second.
	static constexpr std::chrono::milliseconds kRedisHealthReuse{ 1000 };

	// Billing (billing/billing.h), which the billing routes run on
	// (billing/billing_routes.h). Set before start(); it must outlive the HTTP
	// listener. Null: billing is off, and those routes answer billing_disabled
	// (GET /api/billing/tiers answers {"enabled": false}).
	void set_billing(billing::Billing *billing) { billing_ = billing; }
	// The cloud's reader the infrastructure view's AWS sections come from
	// (infra/infra_status.h). Set before start(); it must outlive the HTTP
	// listener. Null: the view says no cloud is read here.
	void set_infra(infra::InfraStatus *status) { infra_ = status; }

	// The object store the hosted data sets' trees and the update packages
	// live in (object_store.h; parts.h make_object_store), which the admin's
	// data set and release routes upload into and read. Set before start(); it
	// must outlive the listener. Null: no store (those routes answer
	// store_unavailable).
	void set_object_store(ObjectStore *store) { object_store_ = store; }
	ObjectStore *object_store() const { return object_store_; }

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

	// The periodic jobs the deployment's fleet schedules (parts.h
	// periodic_jobs, from the same settings): the admin's jobs page shows each
	// one's schedule, and runs one now on request (jobs/job_routes.h). Set
	// before start(); unset, none.
	void set_job_schedules(std::vector<jobs::PeriodicJob> schedules);

	// Whether the welcome mail (POST /api/register) and the password reset
	// links (POST /api/auth/forgot) are sent: queued as mail.send jobs
	// (mailer.h) that a worker sends. Set before start(); off (the default),
	// each mail is logged as not sent.
	void set_mail_enabled(bool on) { mail_on_ = on; }

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
	// @GSB_SERVER@): config.web_host (ONNET_WEB_HOST, the one load-balanced
	// web endpoint, the same the gate and the lobby's SessionInit name) and the
	// port Crow serves (ONNET_HTTP_PORT; the OS's pick for port 0, a test's).
	// The retail client is REMOTE, so these must name the public endpoint,
	// never 127.0.0.1 (else the client POSTs its host registration / fetches
	// the server browser from its own localhost). The browser list is the
	// requesting client's game's (catalog::GameRow::gsb_route: "/jop_2.gsb"
	// for JO's).
	std::string host_url() const;
	std::string gsb_url(const catalog::GameRow &game) const;

	struct Impl;
	std::unique_ptr<Impl> impl_;
	db::ConnectionPool &db_pool_;
	// The retail NW*.dll dance's sessions (session_store.h).
	SessionStore sessions_;
	// PERSISTENTEXPRESSLOGINDATA pins (retail_pins.h).
	RetailPins pins_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	uint16_t bound_port_ = 0;
	// The rest of an account's revocation (an admin password reset, a status
	// other than 'active', a delete, a reset by link; the same update moves the
	// account's credentials generation and ends its website sessions and keys,
	// update_revokes_sessions): drops every PERSISTENTEXPRESSLOGINDATA pin to
	// `user_id`, on every replica, which would resume a retail login with no
	// password (the generation check already refuses such a pin; this drops
	// it), and asks the lobby, on `db`, to take the account off its listings'
	// owner (lobbydb::queue_forget_owner). An owner push already queued for one
	// of them is dropped as it goes out, by the live claim the update deleted.
	void forget_revoked_account(db::Database &db, int64_t user_id);
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
	// moved to now, so the 2 h prune (active_user_sessions.prune) measures the
	// login's real idleness. The account's status is the caller's to check.
	// The NWHANDLE cookie never counts: the service sets it in the clear
	// to the account's handle, so any client can send any account's (and a key
	// minted on it would list a server as anyone's). Throws db::Error.
	std::optional<UserRecord> retail_login_user(
			const std::map<std::string, std::string> &cookies, db::Database &db, const char *&how);
	// Per-server-process EPASK params advertised via the EPASK cookie at
	// /nwprepare.dll. Retail echoes the same params back as a form field
	// at POST /NWLogin.dll, and the fields are decrypted under the echoed
	// ones, so a login whose legs land on different listeners decrypts.
	// Generated once at construction.
	opennova::EpaskParams epask_params_;
	// Optional unknown-message tracker (set via set_unknown_tracker), written
	// by the catch-all 404 path (web's main loop flushes it). Null in
	// tests.
	opennova::UnknownTracker *tracker_ = nullptr;
	// GET /livez's and GET /healthz's watchdog (set_watchdog); null: no loop is
	// watched.
	const Watchdog *watchdog_ = nullptr;
	// The Redis every retail login route needs (the pool the sessions and pins
	// use), and GET /healthz's last PING of it: whether it answered and when,
	// reused for kRedisHealthReuse; one PING at a time, a probe that finds one
	// in flight answering from the last.
	RedisPool &redis_;
	std::mutex redis_health_mu_;
	bool redis_ok_ = false;
	bool redis_pinging_ = false;
	std::optional<std::chrono::steady_clock::time_point> redis_checked_at_;
	// Whether Redis answered GET /healthz's PING within kRedisHealthReuse.
	bool redis_answers();
	// The billing routes' billing (set_billing); null: billing off.
	billing::Billing *billing_ = nullptr;
	// The infrastructure view's cloud reader (set_infra); null: none.
	infra::InfraStatus *infra_ = nullptr;
	// The object store (set_object_store); null: none.
	ObjectStore *object_store_ = nullptr;
	// The slots' limits (set_fleet_slots).
	int fleet_slot_memory_mb_ = 512;
	double fleet_slot_cpus_ = 1.0;
	// The hosted routes' placement policy (set_fleet_placement).
	hosted::PlacementPolicy fleet_placement_;
	// The outbound mail (set_mail_enabled).
	bool mail_on_ = false;
	// config.web_host, set in start() before Crow runs (host_url() /
	// gsb_url()), and config.public_host (the switcher's /api/server-info).
	std::string web_host_;
	std::string public_host_;
	// config.site_url, set in start() before Crow runs: the welcome mail's link.
	std::string site_url_;
};

} // namespace opennova::novaworld_server
