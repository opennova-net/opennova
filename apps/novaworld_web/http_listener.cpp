#include "http_listener.h"

#include "accounts/auth.h"
#include "accounts/mailer.h"
#include "accounts/template_engine.h"
#include "admin_user_routes.h"
#include "billing/billing_repository.h"
#include "billing/billing_routes.h"
#include "billing/tiers.h"
#include "cd_key_routes.h"
#include "connection_routes.h"
#include "core/health.h"
#include "core/object_store.h"
#include "host_routes.h"
#include "hosted/address_routes.h"
#include "hosted/console_routes.h"
#include "hosted/fleet_routes.h"
#include "hosted/hosted_routes.h"
#include "hosted/model/net_cidr.h"
#include "hosted/stats_routes.h"
#include "http_cookies.h"
#include "http_listener_impl.h"
#include "infra/attention_routes.h"
#include "infra/cost_routes.h"
#include "infra/infra_routes.h"
#include "infra/log_routes.h"
#include "infra/usage_routes.h"
#include "jobs/job_routes.h"
#include "lobby_routes.h"
#include "password_reset_routes.h"
#include "products/product_routes.h"
#include "products/project_routes.h"
#include "products/release_routes.h"
#include "program/catalog_repository.h"
#include "program/server_config.h"
#include "redis_client.h"
#include "settings_routes.h"
#include "web_access.h"

#include <net/napi/session.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/join_identity.h>
#include <net/novaworld/lobby_repository.h>
#include <net/novaworld/relay_request.h>
#include <net/novaworld/unknown_tracker.h>

#include <base/io/cp1252.h>
#include <base/io/file_io.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <crow.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// Crow logs two Info lines a request: its "Request:" line, which ends with the
// path, and its "Response:" line, which names it before the code. The health
// probes (the balancer's and Route 53's GET /healthz through the portal, every
// few seconds; the container health check's GET /livez) would fill the log, so
// their lines are dropped, and every other line goes out as Crow's own handler
// writes it. Crow's logger is the process's one.
class QuietProbesLog : public crow::ILogHandler {
public:
	void log(std::string message, crow::LogLevel level) override {
		if (level <= crow::LogLevel::Info && names_probe(message))
			return;
		fallback_.log(std::move(message), level);
	}

private:
	static bool names_probe(const std::string &message) {
		for (const std::string_view path : { " /livez", " /healthz" }) {
			const size_t at = message.find(path);
			if (at == std::string::npos)
				continue;
			const size_t end = at + path.size();
			if (end == message.size() || message[end] == ' ')
				return true;
		}
		return false;
	}

	crow::CerrLogHandler fallback_;
};

// A random 8-hex-char PCID for /api/register, drawn from the OS CSPRNG
// (base/os_random) with no generator of its own: per-worker generators seeded
// by one random_device draw each started several Crow workers on the same
// sequence wherever random_device repeats a value across threads (libstdc++
// reads RDSEED, which AMD's erratum answers with 0 under concurrent use), and
// every registration on a trailing worker then spent its retries on PCIDs the
// leading one had just taken.
std::string next_pcid() {
	char pcid[16];
	std::snprintf(pcid, sizeof(pcid), "%08x", static_cast<unsigned>(os_random_u32()));
	return pcid;
}

// `res` is the file at `p` as a 200 of `type`; a file the route found that no
// longer reads (gone since, or unreadable) is a 404, never an empty 200.
void serve_file(crow::response &res, const std::filesystem::path &p, const std::string &type) {
	std::string body;
	std::string error;
	if (!io::read_file_text(p.string(), body, error)) {
		res.code = 404;
		return;
	}
	res.code = 200;
	res.set_header("Content-Type", type);
	res.body = std::move(body);
}

const char *content_type_for(const std::filesystem::path &p) {
	const auto ext = p.extension().string();
	if (ext == ".html") return "text/html";
	if (ext == ".htm")  return "text/html";          // retail templates
	if (ext == ".mnx")  return "text/html";          // IB3 markup retail parses as HTML
	if (ext == ".joi")  return "text/html";          // join descriptor (HTML w/ <TITLE> tokens)
	if (ext == ".js")   return "application/javascript";
	if (ext == ".css")  return "text/css";
	if (ext == ".json") return "application/json";
	if (ext == ".svg")  return "image/svg+xml";
	if (ext == ".png")  return "image/png";
	if (ext == ".ico")  return "image/x-icon";
	if (ext == ".tga")  return "image/x-tga";
	if (ext == ".woff2") return "font/woff2";
	if (ext == ".woff")  return "font/woff";
	return "application/octet-stream";
}

} // namespace

HttpListener::HttpListener(db::ConnectionPool &db_pool, RedisPool &redis) :
		impl_(std::make_unique<Impl>(db_pool, redis)),
		db_pool_(db_pool),
		sessions_(redis),
		pins_(redis),
		redis_(redis) {
	// A bundle that did not converge stays the zero one, which every client
	// refuses (the modexp gate), so the login leg fails rather than the service.
	if (!opennova::generate_epask(epask_params_)) {
		std::fprintf(stderr, "[http] EPASK params: generate_epask did not converge; logins will fail\n");
		return;
	}
	std::printf("[http] EPASK params: e=%u n=%u key=%s\n",
	            epask_params_.exponent, epask_params_.modulus,
	            epask_params_.key.c_str());
}

HttpListener::~HttpListener() { stop(); }

void HttpListener::set_job_schedules(std::vector<jobs::PeriodicJob> schedules) {
	impl_->jobs.set_schedules(std::move(schedules));
}

bool HttpListener::start(const ServerConfig &config) {
	if (running_.load()) return true;

	auto &app = impl_->app;
	app.loglevel(crow::LogLevel::Info);
	// The process's Crow log, with the health probes' request lines left out;
	// lives as long as the process.
	static QuietProbesLog quiet_log;
	crow::logger::setHandler(&quiet_log);

	// Per-request access log lives inside each handler below. The
	// /<path> wildcard logs 404 misses so we see retail asking for
	// resources we don't serve.

	const std::filesystem::path web_dist = config.web_dist_dir;
	const std::string templates_dir = config.templates_dir.string();
	const std::string static_dir    = config.static_dir.string();
	const std::string public_host   = config.public_host;
	impl_->access.configure(config);

	web_host_ = config.web_host; // host_url(game) / gsb_url(game), with the port Crow binds
	public_host_ = public_host;
	site_url_ = config.site_url; // the welcome mail's link

	// The games catalogue the retail routes answer from (one lobby per title):
	// each row's .gsb route registers below, and each retail page fills in its
	// client's game's values.
	try {
		impl_->games = catalog::GameCatalog::load(*db_pool_.acquire());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] games catalogue read failed: %s\n", e.what());
		return false;
	}
	if (impl_->games.empty())
		std::fprintf(stderr, "[http] WARN no games rows: no .gsb route, no game for a retail page\n");

	register_admin_api_routes(public_host);
	register_public_api_routes();
	impl_->resets.register_routes(impl_->app, mail_on_, config.site_url,
			[this](db::Database &db, int64_t id) { forget_revoked_account(db, id); });
	impl_->hosts.register_routes(impl_->app);
	impl_->connections.register_routes(impl_->app, config.heartbeat_timeout_ms);
	impl_->hosted_servers.configure(config, fleet_placement_);
	impl_->hosted_servers.set_address_zone(address_zone_);
	impl_->hosted_servers.set_novalogic_listing(novalogic_listing_);
	impl_->hosted_servers.register_routes(impl_->app);
	impl_->addresses.configure(addresses_on_, address_zone_, fleet_placement_.max_addresses);
	impl_->addresses.register_routes(impl_->app);
	impl_->fleet.configure(impl_->hosted_servers.hosting_on(), impl_->hosted_servers.provider(),
			impl_->hosted_servers.cap(), fleet_placement_);
	impl_->fleet.configure_roll(fleet_release_, infra_);
	impl_->fleet.register_routes(impl_->app);
	impl_->stats.configure(fleet_slot_memory_mb_, fleet_slot_cpus_);
	impl_->stats.register_routes(impl_->app);
	impl_->console.set_handler_threads(handler_threads());
	impl_->console.register_routes(impl_->app);
	impl_->lobbies.register_routes(impl_->app, novalogic_lobbies_);
	impl_->cd_keys.configure(config);
	impl_->cd_keys.register_routes(impl_->app);
	impl_->billing.set_billing(billing_);
	impl_->billing.set_fleet(fleet_placement_);
	impl_->billing.register_routes(impl_->app);
	impl_->jobs.register_routes(impl_->app);
	impl_->infra.configure(
			infra_, impl_->hosted_servers.hosting_on(), impl_->hosted_servers.provider());
	impl_->infra.register_routes(impl_->app);
	impl_->logs.configure(log_store_, log_store_none_);
	impl_->logs.register_routes(impl_->app);
	impl_->usage.configure(infra_, metrics_store_, fleet_slot_memory_mb_, fleet_slot_cpus_);
	impl_->usage.register_routes(impl_->app);
	impl_->attention.configure(
			alarm_store_, impl_->hosted_servers.hosting_on(), fleet_placement_, fleet_release_);
	impl_->attention.register_routes(impl_->app);
	impl_->costs.configure(cost_store_);
	impl_->costs.register_routes(impl_->app);
	impl_->settings.set_fleet(fleet_placement_);
	impl_->settings.register_routes(impl_->app);
	impl_->products.set_store(object_store_);
	impl_->products.register_routes(impl_->app);
	impl_->releases.set_store(object_store_);
	impl_->releases.register_routes(impl_->app);
	impl_->project_routes.set_store(object_store_);
	impl_->project_routes.set_urls(project_urls_);
	impl_->project_routes.register_routes(impl_->app);
	if (object_store_ != nullptr)
		std::printf("[http] games routes over the %s object store\n", object_store_->kind());
	else
		std::printf(
				"[http] games routes with no object store (hosting off): store routes refused\n");
	register_legacy_login_routes(templates_dir);
	register_legacy_host_join_routes(templates_dir);
	// The catch-all /<path> wildcard must register last: Crow rejects a
	// more-specific route registered after a wildcard with "handler
	// already exists", so the static family always closes registration.
	register_static_routes(web_dist, static_dir, templates_dir);

	// The embedder owns SIGINT/SIGTERM (main()'s handler starts the orderly
	// shutdown, which ends in stop()). Crow would otherwise take both through
	// its own signal_set and stop just this listener, leaving the process up.
	app.signal_clear();

	// Crow binds inside run(), on the worker thread, and v1.2.0 says nothing
	// when it serves: wait_for_server_start() never returns once run() has
	// thrown first (the port taken). Its tick runs on the serving thread, after
	// the bind and with the acceptor accepting, so the first tick hands start()
	// the port Crow took (the OS's pick for port 0); later ticks find the
	// handshake settled and return.
	Impl &impl = *impl_;
	app.tick(Impl::kServingTick, [&impl] {
		std::lock_guard<std::mutex> lock(impl.run_mu);
		if (impl.run != Impl::Run::Pending) return;
		impl.run = Impl::Run::Serving;
		impl.port = impl.app.port();
		impl.run_cv.notify_all();
	});
	// Crow v1.2.0 runs `concurrency` - 1 handler threads beside its acceptor
	// (Server::run's worker_thread_count), so this asks for handler_threads()
	// of them.
	app.port(config.http_port).concurrency(static_cast<std::uint16_t>(handler_threads() + 1));
	worker_ = std::thread([&impl] {
		try {
			impl.app.run();
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[http] crashed: %s\n", e.what());
		}
		{
			std::lock_guard<std::mutex> lock(impl.run_mu);
			impl.run = Impl::Run::Exited;
		}
		impl.run_cv.notify_all();
		std::printf("[http] loop exiting\n");
	});

	// Return once Crow serves. A run() that failed first fails the boot here
	// (main() treats it as fatal) rather than leaving the HTTP layer dead.
	bool serving = false;
	{
		std::unique_lock<std::mutex> lock(impl.run_mu);
		impl.run_cv.wait(lock, [&impl] { return impl.run != Impl::Run::Pending; });
		serving = impl.run == Impl::Run::Serving;
		bound_port_ = impl.port;
	}
	if (!serving) {
		worker_.join();
		return false;
	}
	running_.store(true);
	std::printf("[http] web host %s (HOST_URL / GSB_SERVER per game)\n", web_host_.c_str());
	std::printf("[http] listening on :%u\n", static_cast<unsigned>(bound_port_));
	return true;
}

// The port is Crow's: its run() stores the port it bound (the OS's pick for
// port 0) in the app before it accepts the first connection, so every handler
// reads it settled, and with a nonzero port it is the configured one.
void HttpListener::forget_revoked_account(db::Database &db, int64_t user_id) {
	try {
		pins_.revoke(user_id);
	} catch (const RedisError &e) {
		// The generation check still refuses the pins (pinned_user), and they
		// expire with their TTL.
		std::fprintf(stderr, "[http] WARN revoking user %lld's retail pins: %s\n",
				static_cast<long long>(user_id), e.what());
	}
	try {
		lobbydb::queue_forget_owner(db, user_id);
	} catch (const db::Error &e) {
		// The keys are gone all the same: no push of the account's goes out (the lobby reads the
		// live claim); only the lobby's own view of the owner stays until a re-host.
		std::fprintf(stderr, "[http] WARN forget_owner for user %lld: %s\n",
				static_cast<long long>(user_id), e.what());
	}
}

std::optional<UserRecord> HttpListener::pinned_user(const std::string &cookie, db::Database &db) {
	const auto pin = pins_.get(cookie);
	if (!pin)
		return std::nullopt;
	auto user = get_user_by_id(db, pin->user_id);
	if (user && user->credentials_gen == pin->credentials_gen)
		return user;
	// A reset, a ban or a deletion since the pin's login: it resumes nothing.
	pins_.drop_if(cookie, *pin);
	return std::nullopt;
}

std::optional<UserRecord> HttpListener::retail_login_user(
		const std::map<std::string, std::string> &cookies, db::Database &db, const char *&how) {
	if (const auto it = cookies.find("LOGINSESSIONTAG"); it != cookies.end()) {
		if (auto user = get_user_by_login_session_tag(db, it->second)) {
			// The login is in use: its row's idle clock restarts, so the 2 h
			// prune (the active_user_sessions.prune job) ends a login idle that
			// long, not one in the lobby since its last GET /NWLogin.dll.
			touch_active_user_session(db, user->id);
			how = "login-session";
			return user;
		}
	}
	if (const auto it = cookies.find("PERSISTENTEXPRESSLOGINDATA"); it != cookies.end()) {
		if (auto user = pinned_user(it->second, db)) {
			how = "persist-pin";
			return user;
		}
	}
	return std::nullopt;
}

std::string HttpListener::host_url(const catalog::GameRow &game) const {
	return host_url_for(web_host_, impl_->app.port(), game.host_page);
}

std::string HttpListener::gsb_url(const catalog::GameRow &game) const {
	return "http://" + web_host_ + ":" + std::to_string(impl_->app.port()) + game.gsb_route();
}

// Admin REST API: server status, dev host injection, the listing dump with
// its joiners (/api/hosts), the unknown-message samples (/api/unknowns), and
// the user routes (admin_user_routes.h; the listed servers and their pushes
// are host_routes.h's, the NovaWorld sessions connection_routes.h's). Every
// handler opens with WebAccess::require_admin: the Bearer
// ADMIN_API_TOKEN, or an admin-role website session (with the CSRF header on a
// state change).
void HttpListener::register_admin_api_routes(const std::string &public_host) {
	auto &app = impl_->app;

	CROW_ROUTE(app, "/api/admin/server-status").methods("GET"_method)(
	    [this](const crow::request &req) {
		auto caller = impl_->access.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto db_conn = db_pool_.acquire();
		auto status = get_server_status(*db_conn);
		crow::json::wvalue out;
		out["maintenance_enabled"] = status.maintenance_enabled;
		out["message"] = status.message;
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	CROW_ROUTE(app, "/api/admin/server-status").methods("PUT"_method)(
	    [this](const crow::request &req) {
		auto caller = impl_->access.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		const bool maintenance = body->flag("maintenance_enabled").value_or(false);
		const std::string message = body->text_or("message", "");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		auto db_conn = db_pool_.acquire();
		auto result = update_server_status(*db_conn, maintenance, message);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"] = result.error_code;
			if (!result.error_message.empty())
				out["message"] = result.error_message;
			crow::response res(500);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto status = get_server_status(*db_conn);
		out["maintenance_enabled"] = status.maintenance_enabled;
		out["message"] = status.message;
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// Dev/test helper: inject an active_hosts row so the GSB browser shows a
	// joinable game without a live host process. Admin-gated like every route
	// here, so prod's players never reach it. Defaults point the host at
	// this server's own NW UDP port (127.0.0.1:64206) so a joining OpenNova
	// client's JointOperations hello lands on our nw_udp_listener and routes to
	// the game-runtime PN path. The row (no lobby instance holds it) is wiped by the next lobby's
	// start (clear_unheld) and by the stale-host sweep; re-inject per run.
	CROW_ROUTE(app, "/api/admin/hosts").methods("POST"_method)(
	    [this, public_host](const crow::request &req) {
		auto caller = impl_->access.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		// An empty body takes every default; any other must be an object.
		auto body = JsonBody::parse(req.body.empty() ? std::string("{}") : req.body);
		if (!body) return json_error(400, "invalid_json");
		auto str_or = [&body](const char *k, const char *fallback) {
			return body->text_or(k, fallback);
		};
		auto int_or = [&body](const char *k, int fallback) {
			return static_cast<int>(body->integer(k).value_or(fallback));
		};

		hostdb::HostRow row;
		row.rid          = static_cast<uint32_t>(int_or("rid", 1));
		row.gsid         = str_or("gsid", "dev-gsid-1");
		row.game         = str_or("game", "jop_2_consumer");
		row.app_id       = str_or("app_id", "20");
		row.server_name  = str_or("server_name", "DEV Joinable");
		row.host_ip      = str_or("host_ip", public_host.empty() ? "127.0.0.1" : public_host.c_str());
		row.host_port    = int_or("host_port", 64206);
		row.host_key     = str_or("host_key", "DEVHOSTKEY0000000000000000000000000000000000");
		row.pcid_key     = str_or("pcid_key", "00000000000000000000");
		row.player_count = int_or("player_count", 0);
		row.max_players  = int_or("max_players", 16);
		row.region       = str_or("region", "dev");
		row.game_type    = str_or("game_type", "AAS");
		row.mission_name = str_or("mission_name", "DEV Mission");
		row.country      = str_or("country", "US");
		row.password     = str_or("password", "");
		row.locked       = str_or("locked", "0");
		row.dedicated    = str_or("dedicated", "0");
		row.exp_bits     = str_or("exp_bits", "3");
		row.peer_ip      = row.host_ip;
		row.peer_port    = row.host_port;
		if (body->wrong_type()) return json_error(400, "invalid_field");

		try {
			auto db_conn = db_pool_.acquire();
			hostdb::upsert_host(*db_conn, row);
		} catch (const std::exception &e) {
			// The reply names the code alone: what() names the failing statement.
			std::fprintf(stderr, "[http] POST /api/admin/hosts upsert failed: %s\n", e.what());
			return json_error(500, "upsert_failed");
		}
		std::printf("[http] POST /api/admin/hosts injected rid=%u host=%s:%d game=%s\n",
				static_cast<unsigned>(row.rid), loggable(row.host_ip).c_str(), row.host_port,
				loggable(row.game).c_str());
		crow::json::wvalue out;
		out["rid"] = static_cast<int>(row.rid);
		out["host_ip"] = row.host_ip;
		out["host_port"] = row.host_port;
		out["game"] = row.game;
		out["server_name"] = row.server_name;
		crow::response res(201);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// Every listing as the lobby stores it: its columns, its roster, and the
	// joiners /NWJoin.dll recorded (each one's handle, account id and address).
	// Admin only, like GET /api/admin/hosts: no page of the site reads it, and
	// the joiners' addresses are theirs. Backed by active_hosts + host_players.
	CROW_ROUTE(app, "/api/hosts")([this](const crow::request &req) {
		auto caller = impl_->access.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		std::vector<crow::json::wvalue> entries;
		try {
			auto db_conn = db_pool_.acquire();
			// One snapshot for the rows and every roster and player list read
			// below, so no host goes out beside a roster from another commit.
			db::ReadSnapshot snapshot(*db_conn);
			auto rows = hostdb::list_hosts(*db_conn);
			entries.reserve(rows.size());
			// The host's own text comes in its code page (a stock host's cp1252)
			// and goes out as UTF-8; the GSB keeps the host's bytes.
			const auto text = [](const std::string &s) { return retail_text_to_utf8(s); };
			for (const auto &h : rows) {
				crow::json::wvalue e;
				e["rid"]          = h.rid;
				e["gsid"]         = text(h.gsid);
				e["game"]         = text(h.game);
				e["app_id"]       = text(h.app_id);
				e["server_name"]  = text(h.server_name);
				e["host_ip"]      = text(h.host_ip);
				e["host_port"]    = h.host_port;
				e["players"]      = h.player_count;
				e["max_players"]  = h.max_players;
				e["region"]       = text(h.region);
				e["game_type"]    = text(h.game_type);
				e["mission_name"] = text(h.mission_name);
				e["country"]      = text(h.country);
				e["exp"]          = text(h.exp);
				e["exp_bits"]     = text(h.exp_bits);
				e["ver1"]         = text(h.ver1);
				e["time_left"]    = text(h.time_left);
				e["time_of_day"]  = text(h.time_of_day);
				e["msg"]          = text(h.msg);
				e["mod"]          = text(h.mod);
				e["age"]          = text(h.age);
				e["pb_server"]    = text(h.pb_server);
				// The host-reported PlayerList (one entry per slot).
				std::vector<crow::json::wvalue> roster_entries;
				try {
					auto roster = hostdb::list_roster(*db_conn, h.rid);
					roster_entries.reserve(roster.size());
					for (const auto &s : roster) {
						crow::json::wvalue re;
						re["slot"]        = s.slot;
						re["player_name"] = text(s.player_name);
						re["team"]        = text(s.team);
						re["type"]        = text(s.type);
						roster_entries.push_back(std::move(re));
					}
				} catch (const db::Error &ex) {
					// The host goes out without its roster.
					std::fprintf(stderr, "[http] /api/hosts roster failed: %s\n", ex.what());
				}
				e["roster"] = std::move(roster_entries);
				std::vector<crow::json::wvalue> player_entries;
				try {
					auto players = hostdb::list_players(*db_conn, h.rid);
					player_entries.reserve(players.size());
					for (const auto &p : players) {
						crow::json::wvalue pe;
						pe["nwhandle"] = text(p.nwhandle);
						pe["user_id"]  = p.user_id;
						pe["peer_ip"]  = text(p.peer_ip);
						player_entries.push_back(std::move(pe));
					}
				} catch (const db::Error &ex) {
					// The host goes out without its player list.
					std::fprintf(stderr, "[http] /api/hosts player list failed: %s\n", ex.what());
				}
				e["player_list"] = std::move(player_entries);
				entries.push_back(std::move(e));
			}
		} catch (const db::Error &e) {
			// A failed read is an error, not an empty listing (the public reads' rule).
			std::fprintf(stderr, "[http] /api/hosts list failed: %s\n", e.what());
			return json_error(500, "db_error");
		}
		crow::json::wvalue out;
		out["count"] = entries.size();
		std::printf("[http] /api/hosts -> %zu host(s)\n", entries.size());
		out["hosts"] = std::move(entries);
		return json_reply(200, out);
	});

	// The unknown-message record (unknown_messages), where every process
	// flushes its sightings (the gates and the lobbies, web's 404s:
	// no process reads another's memory), the latest seen first. Optional
	// ?channel= filter. Each entry carries the first captured sample as
	// lowercase hex so reverse-engineering can eyeball the bytes. Admin only: a
	// sample is what a client sent (the body of a request that 404'd, a gate or
	// lobby message's plaintext) and its sample_meta the sender's address.
	CROW_ROUTE(app, "/api/unknowns")([this](const crow::request &req) {
		auto caller = impl_->access.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		const std::string channel_filter =
			req.url_params.get("channel") ? req.url_params.get("channel") : "";
		std::vector<UnknownSighting> sightings;
		try {
			sightings = UnknownTracker::list(*db_pool_.acquire(), channel_filter);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] /api/unknowns: %s\n", e.what());
			return json_error(500, "db_error");
		}
		std::vector<crow::json::wvalue> arr;
		for (const auto &s : sightings) {
			crow::json::wvalue e;
			e["channel"] = s.channel;
			e["signature"] = s.signature;
			e["count"] = static_cast<int64_t>(s.count);
			e["first_seen_ms"] = static_cast<int64_t>(s.first_seen_ms);
			e["last_seen_ms"] = static_cast<int64_t>(s.last_seen_ms);
			e["sample_meta"] = s.sample_meta;
			e["sample_hex"] = strutil::bytes_to_hex(s.sample);
			arr.push_back(std::move(e));
		}
		crow::json::wvalue out;
		out["count"]    = arr.size();
		out["unknowns"] = std::move(arr);
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// The admin user routes (admin_user_routes.h).
	impl_->users.set_fleet(fleet_placement_);
	impl_->users.set_address_zone(address_zone_);
	impl_->users.set_store(object_store_);
	impl_->users.register_routes(
			app, [this](db::Database &db, int64_t id) { forget_revoked_account(db, id); });
}

// Public JSON API: what the web portal reads (lobbies, stats, self-signup, and
// the website's session routes; the logged-in account's listed servers are
// host_routes.h's), the games catalog, the health probes, and the switcher's
// server-info. None names a joiner, an owner or a sender's address: the
// listing dump (/api/hosts) and the unknown-message samples (/api/unknowns)
// are admin routes.
void HttpListener::register_public_api_routes() {
	auto &app = impl_->app;

	// GET /api/lobbies, /api/lobbies/<rid> and /api/lobbies/novalogic are lobby_routes.h's.

	// Phase I.5: aggregate counts for the Vue dashboard.
	CROW_ROUTE(app, "/api/stats")([this]() {
		crow::json::wvalue out;
		try {
			auto db_conn = db_pool_.acquire();
			auto agg = hostdb::aggregate(*db_conn);
			crow::json::wvalue stats;
			stats["games"]    = agg.games;
			stats["lobbies"]  = agg.lobbies;
			stats["players"]  = agg.players;
			using namespace std::chrono;
			const auto now_ms = duration_cast<milliseconds>(
				system_clock::now().time_since_epoch()).count();
			stats["updatedAtMs"] = static_cast<int64_t>(now_ms);
			out["stats"] = std::move(stats);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] /api/stats failed: %s\n", e.what());
			return json_error(500, "db_error");
		}
		return json_reply(200, out);
	});

	// POST /api/login, POST /api/logout, GET /api/me (web_access.cpp).
	impl_->access.register_routes();

	// POST /api/register — public self-signup. No admin token needed.
	// Server auto-generates a unique 8-hex-char PCID. nwhandle defaults
	// to username when omitted; either takes create_user's rules (a username
	// of at most 64 bytes, a handle of at most 30, no control byte in either).
	// The email is required: an address normalize_email takes (stored
	// lowercased) that no other account has (400 email_exists). Returns the
	// new user's public record on success; 400 on conflict / missing or refused
	// fields, 500 on DB error. Once the account is made it gets an admin's grant
	// of one shared server (billing/tiers.h kRegistrationCompServers; a tier
	// bought adds to it, an admin changes it like any grant), and its welcome
	// mail is queued (a mail.send job): a grant or a mail that does not land is
	// logged, never the registration's failure.
	// The X-OpenNova-Request header is required, checked first: a cross-site
	// text/plain form can carry a JSON body (the parse ignores Content-Type),
	// so without it any page could make its visitors create accounts and drain
	// their address's bucket. Then the per-address brake (429 + Retry-After),
	// before the body is read.
	CROW_ROUTE(app, "/api/register").methods("POST"_method)(
	    [this](const crow::request &req) {
		if (!WebAccess::has_csrf_header(req)) return json_error(403, "csrf_header_required");
		const std::string ip = impl_->access.client_ip(req);
		if (const int64_t wait = impl_->access.take_register(ip)) {
			std::printf("[http] /api/register rate-limited for %s\n", loggable(ip).c_str());
			return too_many_requests(wait);
		}
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		const std::string username = body->text_or("username", "");
		const std::string password = body->text_or("password", "");
		const std::string nwhandle = body->text_or("nwhandle", username);
		const std::string email = body->text_or("email", "");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		if (username.empty() || password.empty() || email.empty())
			return json_error(400, "missing_field", "username, password and email required");
		auto db_conn = db_pool_.acquire();
		// Try up to 5 random PCIDs to avoid the rare collision.
		MutationResult result;
		for (int attempt = 0; attempt < 5; ++attempt) {
			const std::string pcid = next_pcid();
			CreateUserParams p;
			p.username = username;
			p.password = password;
			p.pcid     = pcid;
			p.nwh      = "1";
			p.nwhandle = nwhandle;
			p.email    = email;
			result = create_user(*db_conn, p);
			if (result.ok || result.error_code != "pcid_exists") break;
		}
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"]   = result.error_code;
			if (!result.error_message.empty())
				out["message"] = result.error_message;
			crow::response res(result.error_code == "db_error" ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		try {
			billing::CompGrants grant;
			grant.servers = billing::kRegistrationCompServers;
			if (!billing::set_comp_grants(*db_conn, result.id, grant))
				std::fprintf(stderr, "[http] WARN /api/register: user %lld took no registration grant\n",
						static_cast<long long>(result.id));
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] WARN /api/register: user %lld's registration grant failed: %s\n",
					static_cast<long long>(result.id), e.what());
		}
		auto created = get_user_by_id(*db_conn, result.id);
		out["user"] = created ? user_to_json(*db_conn, *created) : crow::json::wvalue{};
		std::printf("[http] /api/register -> created user '%s' (id=%lld)\n",
		            loggable(username).c_str(), static_cast<long long>(result.id));
		if (created)
			queue_mail(
					*db_conn, mail_on_, welcome_mail(created->email, created->username, site_url_));
		crow::response res(201);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	CROW_ROUTE(app, "/api/games")([this]() {
		crow::json::wvalue out;
		try {
			std::vector<crow::json::wvalue> arr;
			auto db_conn = db_pool_.acquire();
			for (const auto &g : catalog::list_games(*db_conn)) {
				crow::json::wvalue e;
				// camelCase, mirroring the /api/lobbies casing.
				e["slug"]        = g.slug;
				e["displayName"] = g.display_name;
				e["lobbyName"]   = g.lobby_name;
				e["gateTag"]     = g.gate_tag;
				e["title"]       = g.title;
				e["ver1"]        = g.ver1;
				e["ver2"]        = g.ver2;
				arr.push_back(std::move(e));
			}
			out["games"] = std::move(arr);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] /api/games failed: %s\n", e.what());
			return json_error(500, "db_error");
		}
		return json_reply(200, out);
	});

	CROW_ROUTE(app, "/api/health")([]() {
		crow::json::wvalue out;
		out["status"] = "ok";
		return out;
	});

	// The process's liveness (core/health.h), web's container health check (the ECS
	// task's, the image's HEALTHCHECK): the stall watchdog alone. "ok", or 503
	// "stalled"; never another detail. A Redis outage fails no web task's
	// liveness: no restart and no new task mends it, and while it lasts each new
	// web task must still come up healthy (a deployment waits for it). Outside
	// /api, as /healthz.
	CROW_ROUTE(app, "/livez")([this]() {
		const bool stalled = watchdog_ != nullptr && !watchdog_->healthy();
		crow::response res(stalled ? 503 : 200);
		res.body = stalled ? "stalled" : "ok";
		res.set_header("Content-Type", "text/plain");
		res.set_header("Cache-Control", "no-store");
		return res;
	});

	// Whether the process serves a retail client now (core/health.h), the load
	// balancer's and the uptime checks' probe: outside /api, so
	// ONNET_API_PROXY_ONLY never refuses it. "ok"; 503 "stalled" while the stall
	// watchdog finds a loop stuck, else 503 "redis unreachable" while Redis does
	// not answer its PING (every retail login route needs it); never another
	// detail.
	CROW_ROUTE(app, "/healthz")([this]() {
		const bool stalled = watchdog_ != nullptr && !watchdog_->healthy();
		const bool redis_down = !stalled && !redis_answers();
		crow::response res(stalled || redis_down ? 503 : 200);
		res.body = stalled ? "stalled" : redis_down ? "redis unreachable" : "ok";
		res.set_header("Content-Type", "text/plain");
		res.set_header("Cache-Control", "no-store");
		return res;
	});

	// The OpenNova Switcher's (switcher/) one question: the address to point
	// retail's NovaWorld hostname at in the hosts file, the public_host the gate
	// advertises as POSTIPADDRESS. The site's domain is behind Cloudflare's proxy,
	// so DNS cannot give it. Read-only and unauthenticated (the address is
	// public); 503 when public_host is no dotted quad, which no hosts line can
	// name, or one no client can be sent to: 0.0.0.0/8, multicast and 240.0.0.0/4
	// (the switcher refuses those too).
	CROW_ROUTE(app, "/api/server-info")([host = public_host_]() {
		const auto address = hosted::parse_ipv4(host);
		const uint32_t first_octet = address ? *address >> 24 : 0;
		if (first_octet == 0 || first_octet >= 224)
			return json_error(503, "no_public_ipv4");
		crow::json::wvalue out;
		out["novaworld_ip"] = host;
		crow::response res = json_reply(200, out);
		res.set_header("Cache-Control", "no-store");
		return res;
	});

}

// Static serving: web/dist index + assets, retail /static/* client
// assets, bare .htm/.mnx/.joi template GETs, and the catch-all that
// records 404s into the unknown tracker. MUST register last (see the
// wildcard note at the call site in start()).
void HttpListener::register_static_routes(const std::filesystem::path &web_dist,
                                          const std::string &static_dir,
                                          const std::string &templates_dir) {
	auto &app = impl_->app;

	// (Static asset serving for the legacy /static/<game>/* paths is folded
	//  into the catch-all /<path> route below — Crow rejects more-specific
	//  routes registered after a wildcard with "handler already exists".)

	// Static-file fallback. Serves index.html for /, and any file under
	// web/dist/ matching the request path. Returns 404 when the file's
	// missing — useful so SPA history-mode routes degrade obviously
	// during dev rather than silently masking a missing build.
	CROW_ROUTE(app, "/")([web_dist]() {
		crow::response res;
		const auto path = web_dist / "index.html";
		if (!std::filesystem::exists(path)) {
			res.code = 404;
			res.body =
			    "This is the OpenNova NovaWorld API server (port 8080).\n"
			    "In dev the web UI is served by Vite at http://localhost:5173 (hot-reload).\n"
			    "web/dist is only populated for the all-in-one prod build "
			    "(run `npm run build` in web/).\n";
			return res;
		}
		serve_file(res, path, "text/html");
		return res;
	});

	CROW_ROUTE(app, "/<path>")(
	    [web_dist, static_dir, templates_dir, this](const crow::request &req,
	                                                const std::string &subpath) {
		crow::response res;
		// Record a 404 miss (method + path, query stripped) into the unknown
		// tracker so /api/unknowns + unknown_messages surface what retail
		// asked for that we don't serve. The request body is the sample.
		auto record_http_404 = [&]() {
			if (!tracker_) return;
			std::string sig = crow::method_name(req.method);
			sig += " /";
			sig += subpath;  // already query-stripped by Crow's route match
			using namespace std::chrono;
			const auto http_now = static_cast<uint64_t>(
				duration_cast<milliseconds>(
					system_clock::now().time_since_epoch()).count());
			const auto *body_ptr =
				reinterpret_cast<const uint8_t *>(req.body.data());
			tracker_->record("http", sig, body_ptr, req.body.size(),
			                 req.remote_ip_address, http_now);
		};

		// Defensive: reject anything that tries to escape the roots.
		if (subpath.find("..") != std::string::npos) {
			res.code = 400;
			return res;
		}

		// Every file below is reached through file_under_root / template_file
		// (accounts/template_engine.h): '/'-separated bare file names that stay under
		// their root, so no absolute path ("//etc/..."), backslash, dot segment
		// or symlink out of the root is served. Crow hands `subpath` over
		// undecoded, so a %2e%2e or %2f is a literal '%' the rule refuses.

		// /static/* — game-client assets (.tga login backgrounds, etc.)
		// served from apps/novaworld_web/static/.
		if (subpath.rfind("static/", 0) == 0) {
			if (const auto path = file_under_root(static_dir, subpath.substr(7))) {
				serve_file(res, *path, content_type_for(*path));
				return res;
			}
			record_http_404();
			res.code = 404;
			return res;
		}

		// *.htm / *.mnx / *.joi from apps/novaworld_web/templates/ by direct GET (retail's GET
		// /jop_2_main.mnx after the relay refresh): as stored but for {{WEB_ROOT}} (web_root_for:
		// JO's sign-in template's image lines; D-NET-1332); any other {{X}} stays as it is.
		const auto ext = std::filesystem::path(subpath).extension().string();
		if (ext == ".htm" || ext == ".mnx" || ext == ".joi") {
			if (const auto path = template_file(templates_dir, subpath)) {
				serve_file(res, *path, content_type_for(*path));
				res.body = render_template(
						res.body, {{"WEB_ROOT", web_root_for(web_host_, impl_->app.port())}});
				return res;
			}
		}

		// A root image from static/, as the live /nonbhdx2.tga (D-NET-1332).
		if (ext == ".tga" && subpath.find('/') == std::string::npos) {
			if (const auto path = file_under_root(static_dir, subpath)) {
				serve_file(res, *path, content_type_for(*path));
				return res;
			}
		}

		// Fallback — Vue SPA assets from web/dist/.
		const auto path = file_under_root(web_dist, subpath);
		if (!path) {
			std::printf("[http] 404 /%s (no match in static/, templates/, web/dist/)\n",
					loggable(subpath).c_str());
			record_http_404();
			res.code = 404;
			res.body = "not found: " + subpath;
			res.set_header("Content-Type", "text/plain");
			return res;
		}
		serve_file(res, *path, content_type_for(*path));
		return res;
	});
}

bool HttpListener::redis_answers() {
	{
		std::lock_guard<std::mutex> lock(redis_health_mu_);
		const auto now = std::chrono::steady_clock::now();
		if (redis_pinging_ || (redis_checked_at_ && now - *redis_checked_at_ < kRedisHealthReuse))
			return redis_ok_;
		redis_pinging_ = true;
	}
	bool ok = false;
	std::string why;
	try {
		// A pooled connection Redis dropped while it sat idle is no outage.
		ok = redis_.command({ "PING" }, RedisResend::IfDropped).string == "PONG";
	} catch (const std::exception &e) {
		why = e.what();
	}
	std::lock_guard<std::mutex> lock(redis_health_mu_);
	if (ok != redis_ok_ && redis_checked_at_) {
		if (ok)
			std::printf("[http] /healthz: redis answers again\n");
		else
			std::fprintf(stderr, "[http] WARN /healthz: redis does not answer: %s\n",
					why.empty() ? "no PONG" : why.c_str());
	}
	redis_ok_ = ok;
	redis_pinging_ = false;
	redis_checked_at_ = std::chrono::steady_clock::now();
	return ok;
}

void HttpListener::stop() {
	// start() returned true only once Crow served, so its stop() takes (one
	// before run() built the server would be a no-op). The stop closes every
	// io_context, so a held-open connection does not keep run() up.
	if (running_.exchange(false)) {
		impl_->app.stop();
	}
	if (worker_.joinable()) {
		worker_.join();
	}
}

} // namespace opennova::novaworld_server
