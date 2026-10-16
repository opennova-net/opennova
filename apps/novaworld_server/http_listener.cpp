#include "http_listener.h"

#include "admin_user_routes.h"
#include "auth.h"
#include "billing/billing_routes.h"
#include "catalog_repository.h"
#include "cd_key_routes.h"
#include "health.h"
#include "host_routes.h"
#include "hosted/console_routes.h"
#include "hosted/fleet_routes.h"
#include "hosted/hosted_routes.h"
#include "hosted/net_cidr.h"
#include "hosted/stats_routes.h"
#include "http_cookies.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#include "session_store.h"
#include "template_engine.h"
#include "web_access.h"

#include <net/napi/session.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/join_identity.h>
#include <net/novaworld/relay_request.h>
#include <net/novaworld/unknown_tracker.h>

#include <base/io/cp1252.h>
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
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// The page link's first-call query on NWJoin.dll / NWHost.dll: any of the
// fields the join / host page link carries (net/novaworld/relay_request.h).
bool has_relay_first_call_query(const crow::request &req) {
	for (const char *name : {"rid", "success", "failure", "relay", "msgbase", "needexpkey",
	                         "pfid", "mode"}) {
		if (req.url_params.get(name) != nullptr) return true;
	}
	return false;
}

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

std::string read_file_text(const std::filesystem::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream os;
	os << in.rdbuf();
	return os.str();
}

// ---- cookie + form helpers (Phase E.1; cookie reading: http_cookies.h) ----

std::string url_decode(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '+') {
			out.push_back(' ');
		} else if (s[i] == '%' && i + 2 < s.size()) {
			auto hex_to_int = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};
			int hi = hex_to_int(s[i + 1]);
			int lo = hex_to_int(s[i + 2]);
			if (hi < 0 || lo < 0) {
				out.push_back(s[i]);
			} else {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
			}
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

std::map<std::string, std::string> parse_form_body(std::string_view body) {
	std::map<std::string, std::string> out;
	size_t pos = 0;
	while (pos < body.size()) {
		const auto amp = body.find('&', pos);
		const auto chunk_end = (amp == std::string_view::npos) ? body.size() : amp;
		const auto eq = body.find('=', pos);
		if (eq != std::string_view::npos && eq < chunk_end) {
			out.emplace(url_decode(body.substr(pos, eq - pos)),
			            url_decode(body.substr(eq + 1, chunk_end - eq - 1)));
		}
		if (amp == std::string_view::npos) break;
		pos = amp + 1;
	}
	return out;
}

// Crow doesn't have a first-class set_cookie helper across all versions;
// append a Set-Cookie line (crow::response stores headers in a multimap).
// Through add_set_cookie (http_cookies.h): a value from a query, a form, a
// cookie or a stored account that holds a byte that could split the response
// or forge a cookie goes out empty.
void add_cookie(crow::response &res, const std::string &name, const std::string &value) {
	add_set_cookie(res, name, value, "Path=/");
}

void add_standard_headers(crow::response &res) {
	res.set_header("Expires", "Sat, 01 Jan 2000 00:00:00 GMT");
	res.set_header("Pragma", "no-cache");
	res.set_header("Cache-Control", "no-cache, must-revalidate");
}

// The message POST /NWLogin.dll's failure page carries when the login brake
// refuses the attempt (see handle_login_post).
constexpr const char *kLoginThrottledMessage =
		"Too many failed logins. Please try again later.";

// The game a retail request's client plays (catalog::GameCatalog::for_client):
// the gate tag it sends (the `gt` query /nwprepare.dll carries, else the GT
// cookie that page set, which a stock client sends back on every request,
// docs/net/novaworld-net-re.md section 7, Wave 6), else its product family
// (`pfid`, the request's own field, else the NWPF cookie /NWJoin.dll and
// /NWHost.dll set), else the prefix of the page it names (`page`). JO and
// OpenNova share pfid 28 and JO's pages, so only the tag tells them apart; a
// request with no tag is JO's. An empty catalogue (no games rows) answers a
// row with every field empty.
const catalog::GameRow &client_game(const catalog::GameCatalog &games,
                                    const crow::request &req,
                                    const std::map<std::string, std::string> &cookies,
                                    std::string_view pfid, std::string_view page) {
	static const catalog::GameRow kNoGame;
	auto cookie = [&cookies](const char *name) -> std::string_view {
		const auto it = cookies.find(name);
		return it == cookies.end() ? std::string_view() : std::string_view(it->second);
	};
	catalog::ClientHints hints;
	const char *gt = req.url_params.get("gt");
	hints.gate_tag = gt != nullptr && *gt != '\0' ? std::string_view(gt) : cookie("GT");
	hints.pfid = pfid.empty() ? cookie("NWPF") : pfid;
	hints.page = page;
	const catalog::GameRow *game = games.for_client(hints);
	return game != nullptr ? *game : kNoGame;
}

crow::response render_legacy_message(const std::string &templates_dir,
                                     const std::string &message,
                                     const std::string &failure_template,
                                     const std::string &msg_template,
                                     const std::string &remote_ip,
                                     const std::string &host_url,
                                     const std::string &gsb_url) {
	TemplateVars vars{
		{"MESSAGE",     message},
		{"IN",          failure_template},
		{"OUT",         failure_template},
		{"MSGBASE",     msg_template},
		{"HOST_URL",    host_url},
		{"GSB_SERVER",  gsb_url},
		{"JOINLAN_URL", ""},
	};
	crow::response res(200);
	res.body = render_template_file(templates_dir, msg_template, vars);
	res.set_header("Content-Type", "text/html");
	add_standard_headers(res);
	add_cookie(res, "YOURIP", remote_ip);
	return res;
}

bool parse_exp_bits(const std::string &value, uint64_t &out) {
	if (value.empty()) return false;
	char *end = nullptr;
	const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
	if (end == value.c_str() || *end != '\0') return false;
	out = static_cast<uint64_t>(parsed);
	return true;
}

bool expansion_bits_compatible(const std::string &owned,
                               const std::string &required) {
	if (required.empty()) return true;
	if (owned.empty()) return false;
	uint64_t owned_bits = 0;
	uint64_t required_bits = 0;
	if (parse_exp_bits(owned, owned_bits) && parse_exp_bits(required, required_bits)) {
		return (owned_bits & required_bits) == required_bits;
	}
	return owned == required;
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

// One-line summary of inbound cookies for logging. Each entry is
// `name=value` with the value truncated to ~20 chars, both loggable(); a
// bearer secret (a session tag or token, a persistent login: is_secret_cookie)
// shows only as its loggable_secret() stand-in. Tags showing up here that we
// didn't issue this run = retail bridging state from a prior process via its
// persistent cookie jar (PERSISTENTEXPRESSLOGINDATA is the usual suspect — see
// G.4).
std::string cookie_summary(std::string_view header) {
	std::string out;
	const auto cookies = parse_cookie_header(header);
	bool first = true;
	for (const auto &[k, v] : cookies) {
		if (!first) out += ',';
		out += loggable(k);
		out += '=';
		out += is_secret_cookie(k) ? loggable_secret(v) : loggable(v, 20);
		first = false;
	}
	return out;
}

const char *connection_state_name(ConnectionState s) {
	switch (s) {
	case ConnectionState::Handshaking: return "handshaking";
	case ConnectionState::Active:      return "active";
	case ConnectionState::Closing:     return "closing";
	}
	return "unknown";
}

} // namespace

struct HttpListener::Impl {
	explicit Impl(db::ConnectionPool &pool) :
			access(app, pool),
			hosts(access, pool),
			hosted_servers(access, pool),
			fleet(access, pool),
			stats(access, pool),
			console(access, pool),
			cd_keys(access, pool),
			users(access, pool),
			billing(access, pool) {}

	WebApp app;
	// start()'s handshake with the Crow thread: Pending until Crow serves
	// (Serving, with the port it bound) or until run() returns (Exited).
	enum class Run { Pending, Serving, Exited };
	std::mutex run_mu;
	std::condition_variable run_cv;
	Run run = Run::Pending;
	uint16_t port = 0;
	// How often Crow runs the tick whose first call tells start() it serves.
	static constexpr std::chrono::milliseconds kServingTick{50};
	// Who each /api request comes from (web_access.h); declared after `app`,
	// which it registers its routes on.
	WebAccess access;
	// The listed servers' routes (host_routes.h), over `access`.
	HostRoutes hosts;
	// The hosted servers' routes (hosted/hosted_routes.h), over `access`.
	hosted::HostedRoutes hosted_servers;
	// The admin's fleet view and the machines' routes (hosted/fleet_routes.h).
	hosted::FleetRoutes fleet;
	// The hosted servers' stats (hosted/stats_routes.h).
	hosted::StatsRoutes stats;
	// Their console routes over the admin links (hosted/console_routes.h).
	hosted::ConsoleRoutes console;
	// The CD keys' admin routes and the retail login's DFX2 activation (cd_key_routes.h).
	CdKeyRoutes cd_keys;
	// The admin's user routes (admin_user_routes.h).
	AdminUserRoutes users;
	// The support tiers' routes (billing/billing_routes.h).
	billing::BillingRoutes billing;
	// The games catalogue, loaded once in start() before the routes register
	// and never written after, so handlers read it without a lock.
	catalog::GameCatalog games;
};

HttpListener::HttpListener(ConnectionManager &manager, db::ConnectionPool &db_pool,
                           SessionStore &sessions)
	: impl_(std::make_unique<Impl>(db_pool)), manager_(manager), db_pool_(db_pool),
	  sessions_(sessions) {
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


bool HttpListener::start(const ServerConfig &config) {
	if (running_.load()) return true;

	auto &app = impl_->app;
	app.loglevel(crow::LogLevel::Info);

	// Per-request access log lives inside each handler below. The
	// /<path> wildcard logs 404 misses so we see retail asking for
	// resources we don't serve.

	const std::filesystem::path web_dist = config.web_dist_dir;
	const std::string templates_dir = config.templates_dir.string();
	const std::string static_dir    = config.static_dir.string();
	const std::string public_host   = config.public_host;
	impl_->access.configure(config);

	public_host_ = public_host; // host_url() / gsb_url(), with the port Crow binds
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
	impl_->hosts.register_routes(impl_->app, nw_udp_);
	impl_->hosted_servers.configure(config, fleet_placement_);
	impl_->hosted_servers.register_routes(impl_->app);
	impl_->fleet.configure(impl_->hosted_servers.enabled(), impl_->hosted_servers.provider(),
			impl_->hosted_servers.cap());
	impl_->fleet.register_routes(impl_->app);
	impl_->stats.configure(fleet_slot_memory_mb_, fleet_slot_cpus_);
	impl_->stats.register_routes(impl_->app, admin_links_);
	impl_->console.set_handler_threads(handler_threads());
	impl_->console.register_routes(impl_->app, admin_links_);
	impl_->cd_keys.configure(config);
	impl_->cd_keys.register_routes(impl_->app);
	impl_->billing.set_billing(billing_);
	impl_->billing.set_limits_hold(limits_hold_);
	impl_->billing.register_routes(impl_->app);
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
	std::printf("[http] HOST_URL=%s\n", host_url().c_str());
	std::printf("[http] listening on :%u\n", static_cast<unsigned>(bound_port_));
	return true;
}

// The port is Crow's: its run() stores the port it bound (the OS's pick for
// port 0) in the app before it accepts the first connection, so every handler
// reads it settled, and with a nonzero port it is the configured one.
void HttpListener::forget_revoked_account(int64_t user_id) {
	{
		std::lock_guard<std::mutex> lock(persistent_user_mu_);
		for (auto it = persistent_pins_.begin(); it != persistent_pins_.end();) {
			it = it->second.user_id == user_id ? persistent_pins_.erase(it) : std::next(it);
		}
	}
	if (nw_udp_ != nullptr) nw_udp_->forget_owner(user_id);
}

std::optional<UserRecord> HttpListener::pinned_user(const std::string &cookie, db::Database &db) {
	if (cookie.empty())
		return std::nullopt;
	RetailPin pin;
	{
		std::lock_guard<std::mutex> lk(persistent_user_mu_);
		const auto it = persistent_pins_.find(cookie);
		if (it == persistent_pins_.end())
			return std::nullopt;
		pin = it->second;
	}
	auto user = get_user_by_id(db, pin.user_id);
	if (user && user->credentials_gen == pin.credentials_gen)
		return user;
	// A reset, a ban or a deletion since the pin's login: it resumes nothing.
	std::lock_guard<std::mutex> lk(persistent_user_mu_);
	const auto it = persistent_pins_.find(cookie);
	if (it != persistent_pins_.end() && it->second.user_id == pin.user_id &&
			it->second.credentials_gen == pin.credentials_gen) {
		persistent_pins_.erase(it);
	}
	return std::nullopt;
}

std::optional<UserRecord> HttpListener::retail_login_user(
		const std::map<std::string, std::string> &cookies, db::Database &db, const char *&how) {
	if (const auto it = cookies.find("LOGINSESSIONTAG"); it != cookies.end()) {
		if (auto user = get_user_by_login_session_tag(db, it->second)) {
			// The login is in use: its row's idle clock restarts, so the 2 h
			// sweep (evict_active_user_sessions_older_than) ends a login idle
			// that long, not one in the lobby since its last GET /NWLogin.dll.
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

std::string HttpListener::host_url() const {
	return "http://" + public_host_ + ":" + std::to_string(impl_->app.port()) + "/nwhost.dll";
}

std::string HttpListener::gsb_url(const catalog::GameRow &game) const {
	return "http://" + public_host_ + ":" + std::to_string(impl_->app.port()) + game.gsb_route();
}

// Admin REST API: server status, dev host injection, connection dump, the
// listing dump with its joiners (/api/hosts), the unknown-message samples
// (/api/unknowns), and the user routes (admin_user_routes.h; the listed
// servers and their pushes are host_routes.h's). Every
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
	// the game-runtime PN path. The row is wiped on the next boot (clear_all)
	// and by the stale-host sweep; re-inject per run.
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

	// Connection-registry debug dump.
	CROW_ROUTE(app, "/api/admin/connections")([this](const crow::request &req) {
		auto caller = impl_->access.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto snapshot = manager_.registry().snapshot();
		crow::json::wvalue out;
		out["count"] = snapshot.size();
		out["heartbeat_timeout_ms"] = manager_.heartbeat_timeout_ms();
		std::vector<crow::json::wvalue> entries;
		entries.reserve(snapshot.size());
		for (const auto &c : snapshot) {
			crow::json::wvalue e;
			e["id"]            = c.id;
			e["state"]         = connection_state_name(c.state);
			e["pn"]            = c.pn;
			e["identity"]      = c.identity;
			e["created_ms"]    = c.created_ms;
			e["last_seen_ms"]  = c.last_seen_ms;
			e["addr"]          = peer_addr_to_string(c.addr);
			entries.push_back(std::move(e));
		}
		out["connections"] = std::move(entries);
		crow::response res(200);
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
						if (p.user_id) pe["user_id"] = static_cast<int64_t>(*p.user_id);
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
			std::fprintf(stderr, "[http] /api/hosts list failed: %s\n", e.what());
		}
		crow::json::wvalue out;
		out["count"] = entries.size();
		out["hosts"] = std::move(entries);
		std::printf("[http] /api/hosts -> %zu host(s)\n", entries.size());
		return json_reply(200, out);
	});

	// Live unknown-message snapshot (in-memory, reflects THIS run — the
	// durable cross-run record is the unknown_messages table). Optional
	// ?channel= filter. Each entry carries the first captured sample as
	// lowercase hex so reverse-engineering can eyeball the bytes. Admin only: a
	// sample is what a client sent (the body of a request that 404'd, a gate or
	// lobby message's plaintext) and its sample_meta the sender's address.
	CROW_ROUTE(app, "/api/unknowns")([this](const crow::request &req) {
		auto caller = impl_->access.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		const std::string channel_filter =
			req.url_params.get("channel") ? req.url_params.get("channel") : "";
		std::vector<crow::json::wvalue> arr;
		if (tracker_) {
			for (const auto &s : tracker_->snapshot()) {
				if (!channel_filter.empty() && s.channel != channel_filter) continue;
				crow::json::wvalue e;
				e["channel"]       = s.channel;
				e["signature"]     = s.signature;
				e["count"]         = static_cast<int64_t>(s.count);
				e["first_seen_ms"] = static_cast<int64_t>(s.first_seen_ms);
				e["last_seen_ms"]  = static_cast<int64_t>(s.last_seen_ms);
				e["sample_meta"]   = s.sample_meta;
				e["sample_hex"]    = strutil::bytes_to_hex(s.sample);
				arr.push_back(std::move(e));
			}
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
	impl_->users.register_routes(app, [this](int64_t id) { forget_revoked_account(id); });
}

// Public JSON API: what the web portal reads (lobbies, stats, self-signup, and
// the website's session routes; the logged-in account's listed servers are
// host_routes.h's), the games catalog, the health probes, and the switcher's
// server-info. None names a joiner, an owner or a sender's address: the
// listing dump (/api/hosts) and the unknown-message samples (/api/unknowns)
// are admin routes.
void HttpListener::register_public_api_routes() {
	auto &app = impl_->app;

	CROW_ROUTE(app, "/api/lobbies")([this]() {
		// Phase I.4: game-centric format mirroring onnet's api.py:21-49.
		// {"games":[{"slug","displayName","hosts":[...]}]}
		// Vue lobby browser keys off this exact shape. One group per games
		// row, live from the table; a listing lands under the row whose
		// LobbyName it registered with (active_hosts.game).
		std::vector<crow::json::wvalue> games_json;
		try {
			auto db_conn   = db_pool_.acquire();
			auto games     = catalog::list_games(*db_conn);
			auto host_rows = hostdb::list_hosts(*db_conn);
			games_json.reserve(games.size());
			for (const auto &g : games) {
				crow::json::wvalue game;
				game["slug"]        = g.slug;
				game["displayName"] = g.display_name;
				std::vector<crow::json::wvalue> hosts;
				for (const auto &h : host_rows) {
					if (h.game != g.lobby_name) continue;
					// camelCase to match the web LobbyHost type (snake_case here rendered
					// every host as "Unnamed Server 0/0" — the Vue card reads
					// serverName/maxPlayers). hostIp/hostPort surface the join
					// address so a host is identifiable, not just named. The host's own
					// text comes in its code page (a stock host's cp1252) and goes out
					// as UTF-8; the GSB keeps the host's bytes for stock clients.
					crow::json::wvalue hj;
					hj["id"]          = h.rid;
					hj["serverName"]  = retail_text_to_utf8(h.server_name);
					hj["hostIp"]      = retail_text_to_utf8(h.host_ip);
					hj["hostPort"]    = h.host_port;
					hj["players"]     = h.player_count;
					hj["maxPlayers"]  = h.max_players;
					hj["region"]      = retail_text_to_utf8(h.region);
					hosts.push_back(std::move(hj));
				}
				game["hosts"] = std::move(hosts);
				games_json.push_back(std::move(game));
			}
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] /api/lobbies failed: %s\n", e.what());
		}
		crow::json::wvalue out;
		out["games"] = std::move(games_json);
		return out;
	});

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
			out["error"] = "db_error";
		}
		return out;
	});

	// POST /api/login, POST /api/logout, GET /api/me (web_access.cpp).
	impl_->access.register_routes();

	// POST /api/register — public self-signup. No admin token needed.
	// Server auto-generates a unique 8-hex-char PCID. nwhandle defaults
	// to username when omitted; either takes create_user's rules (a username
	// of at most 64 bytes, a handle of at most 30, no control byte in either).
	// Returns the new user's public record on success; 400 on conflict /
	// missing or refused fields, 500 on DB error.
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
		if (body->wrong_type()) return json_error(400, "invalid_field");
		if (username.empty() || password.empty()) {
			crow::response res(400);
			res.body = "{\"error\":\"missing_field\","
			           "\"message\":\"username and password required\"}";
			res.set_header("Content-Type", "application/json");
			return res;
		}
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
		auto created = get_user_by_id(*db_conn, result.id);
		out["user"] = created ? user_to_json(*db_conn, *created) : crow::json::wvalue{};
		std::printf("[http] /api/register -> created user '%s' (id=%lld)\n",
		            loggable(username).c_str(), static_cast<long long>(result.id));
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
			out["error"] = "db_error";
		}
		return out;
	});

	CROW_ROUTE(app, "/api/health")([]() {
		crow::json::wvalue out;
		out["status"] = "ok";
		return out;
	});

	// The container runtime's and the uptime checks' probe (health.h): outside
	// /api, so ONNET_API_PROXY_ONLY never refuses it. "ok", or 503 "stalled"
	// while the stall watchdog finds a loop stuck; never a detail.
	CROW_ROUTE(app, "/healthz")([this]() {
		const bool healthy = watchdog_ == nullptr || watchdog_->healthy();
		crow::response res(healthy ? 200 : 503);
		res.body = healthy ? "ok" : "stalled";
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

// Retail NW*.dll login/session chain: prepare, start, the EPASK login
// POST + relay GET, logout, and the character/account template pages.
void HttpListener::register_legacy_login_routes(const std::string &templates_dir) {
	auto &app = impl_->app;

	// ----- Phase E.1: Legacy NW*.dll login chain ---------------------------
	// Routes registered for both lowercase (retail capture) and Title-case
	// spellings since Crow routes are case-sensitive. Behavior mirrors
	// onnet's onnw/controllers/nova_world/{prepare,login}.py.

	// GET /nwprepare.dll: the gate's startupurl, whose `gt` is the client's
	// gate tag (the [GT] it substitutes), so the page and the GT cookie it sets
	// name the client's game; `url` defaults to that game's start page. DFX2's
	// startupurl also carries its pfid.
	auto handle_prepare = [this, templates_dir](const crow::request &req) {
		const std::string url_param = req.url_params.get("url") ? req.url_params.get("url") : "";
		const char *pfid_param = req.url_params.get("pfid");
		const catalog::GameRow &game =
				client_game(impl_->games, req, parse_cookie_header(request_cookie_header(req)),
						pfid_param ? pfid_param : "", url_param);
		const std::string url   = url_param.empty() ? game.start_page : url_param;
		const std::string ver1  = req.url_params.get("ver1") ? req.url_params.get("ver1") : "";
		const std::string ver2  = req.url_params.get("ver2") ? req.url_params.get("ver2") : "";
		const std::string gt    = req.url_params.get("gt")   ? req.url_params.get("gt")   : "";
		const std::string cc    = req.url_params.get("cc")   ? req.url_params.get("cc")   : "";

		const std::string epask = opennova::epask_to_string(epask_params_);
		std::printf("[http] GET %s url=%s gt=%s game=%s ver=%s/%s cookies=[%s]\n",
				loggable(req.url).c_str(), loggable(url).c_str(), loggable(gt).c_str(),
				loggable(game.slug).c_str(), loggable(ver1).c_str(), loggable(ver2).c_str(),
				cookie_summary(request_cookie_header(req)).c_str());

		TemplateVars vars{
			{"HOST_URL",     host_url()},
			{"GSB_SERVER",   gsb_url(game)},
			{"JOINLAN_URL",  ""},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, url, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP", req.remote_ip_address);
		add_cookie(res, "VER1",   ver1);
		add_cookie(res, "VER2",   ver2);
		add_cookie(res, "GT",     gt);
		add_cookie(res, "EPASK",  epask);
		if (!cc.empty()) add_cookie(res, "CC", cc);
		return res;
	};
	CROW_ROUTE(app, "/nwprepare.dll").methods("GET"_method)(handle_prepare);
	CROW_ROUTE(app, "/NWPrepare.dll").methods("GET"_method)(handle_prepare);

	// GET /nwstart.dll
	auto handle_start = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		const char *named_page = req.url_params.get("IN") ? req.url_params.get("IN")
		                         : req.url_params.get("OUT") ? req.url_params.get("OUT")
		                                                     : req.url_params.get("MSGBASE");
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {},
		                                           named_page ? named_page : "");
		const std::string in_p   = req.url_params.get("IN")      ? req.url_params.get("IN")      : game.page("main.htm");
		const std::string out_p  = req.url_params.get("OUT")     ? req.url_params.get("OUT")     : game.page("login.htm");
		const std::string msgbase = req.url_params.get("MSGBASE")? req.url_params.get("MSGBASE") : game.page("msg.htm");
		const auto epask = cookies.count("EPASK") ? cookies.at("EPASK") : opennova::epask_to_string(epask_params_);
		// An unactivated DFX2 client (its NWCDKIID no wire key) gets the activation login, ACT.
		const char *act_p = req.url_params.get("ACT");
		const auto nwcdkiid = cookies.find("NWCDKIID");
		const std::string nwcdkiid_v = nwcdkiid == cookies.end() ? std::string() : nwcdkiid->second;
		const bool activating = act_p != nullptr && *act_p != '\0' &&
				cdkeys::needs_activation(game.title, nwcdkiid_v);
		const std::string page_p = activating ? std::string(act_p) : out_p;

		std::printf("[http] GET %s IN=%s OUT=%s MSGBASE=%s cookies=[%s]\n",
				loggable(req.url).c_str(), loggable(in_p).c_str(), loggable(out_p).c_str(),
				loggable(msgbase).c_str(), cookie_summary(request_cookie_header(req)).c_str());

		// Hardcoded version-OK; render OUT (the login page).
		TemplateVars vars{
			{"IN",          in_p},
			{"OUT",         out_p},
			{"MSGBASE",     msgbase},
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", ""},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, page_p, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",      req.remote_ip_address);
		add_cookie(res, "USEJUNCTION", "0");
		add_cookie(res, "EPASK",       epask);
		return res;
	};
	CROW_ROUTE(app, "/nwstart.dll").methods("GET"_method)(handle_start);
	CROW_ROUTE(app, "/NWStart.dll").methods("GET"_method)(handle_start);

	// POST /NWLogin.dll
	auto handle_login_post = [this, templates_dir](const crow::request &req) {
		auto db_conn = db_pool_.acquire();
		// Periodic TTL sweep on the LoginSession map (5-min max age) so
		// the in-memory tag dicts don't grow unbounded over uptime. Runs
		// every 64th POST so it doesn't dominate request latency.
		if ((login_post_counter_.fetch_add(1) & 0x3F) == 0) {
			const auto dropped = sessions_.evict_older_than(5 * 60 * 1000);
			if (dropped > 0) {
				std::printf("[http] session TTL sweep dropped %zu stale entries\n", dropped);
			}
			try {
				const auto active_dropped =
					evict_active_user_sessions_older_than(*db_conn, 2 * 60 * 60);
				if (active_dropped > 0) {
					std::printf("[http] active-session TTL sweep dropped %zu stale entries\n",
					            active_dropped);
				}
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[http] active-session TTL sweep failed: %s\n", e.what());
			}
		}

		const auto form = parse_form_body(req.body);
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		auto pick = [&](const char *name) {
			auto it = form.find(name);
			return it == form.end() ? std::string() : it->second;
		};

		// Real-auth path: when retail POSTs the form, the EPASK field
		// echoes the e:n:key bundle we sent on /nwprepare.dll, and the
		// other fields (NAME, PASSWORD, relay, msgbase, success, ...)
		// arrive encrypted under it. Decode them here. Falls back
		// silently when EPASK is absent — that lets curl walkthroughs
		// hit the same handler with raw values.
		std::optional<opennova::EpaskParams> epask_in;
		if (auto e_str = pick("EPASK"); !e_str.empty()) {
			opennova::EpaskParams parsed;
			std::string why;
			if (opennova::epask_from_string(e_str, parsed, &why)) {
				epask_in = parsed;
			} else {
				std::fprintf(
						stderr, "[http] WARN EPASK form field bad: %s\n", loggable(why).c_str());
			}
		}
		auto epask_decode = [&](const std::string &name) -> std::string {
			const auto v = pick(name.c_str());
			if (v.empty() || !epask_in) return v;
			std::string plain;
			std::string why;
			if (!opennova::epask_decrypt(v, *epask_in, plain, &why)) {
				std::fprintf(stderr, "[http] WARN EPASK decrypt(%s) failed: %s\n", name.c_str(),
						loggable(why).c_str());
				return std::string();
			}
			return plain;
		};

		// looks_encrypted is now only relevant for the no-EPASK fallback
		// path (curl walkthroughs that don't carry an EPASK form field).
		auto looks_encrypted = [](const std::string &v) {
			if (v.size() < 16) return false;
			for (char c : v) if (c < 'A' || c > 'P') return false;
			return true;
		};
		auto field_or = [&](const char *name, const std::string &fallback) {
			const auto v = epask_in ? epask_decode(name) : pick(name);
			return (v.empty() || (!epask_in && looks_encrypted(v))) ? fallback : v;
		};

		const std::string login_name     = epask_in ? epask_decode("NAME")     : std::string();
		const std::string login_password = epask_in ? epask_decode("PASSWORD") : std::string();
		// The login's game: the client's gate tag (the GT cookie), else the
		// form's pfid and success page. Its access row and expansion bits are
		// the ones this login checks and grants.
		const catalog::GameRow &game = client_game(impl_->games, req, cookies,
				field_or("pfid", std::string()), field_or("success", std::string()));
		const std::string &game_slug = game.slug;
		auto render_login_message = [&](const std::string &message) {
			const std::string fail_tpl = field_or("failure", game.page("main.htm"));
			const std::string msg_tpl  = field_or("msgbase", game.page("msg.htm"));
			return render_legacy_message(templates_dir, message, fail_tpl, msg_tpl,
			                             req.remote_ip_address, host_url(), gsb_url(game));
		};

		const auto server_status = get_server_status(*db_conn);
		if (server_status.maintenance_enabled) {
			std::printf("[http] POST /NWLogin.dll rejected: maintenance\n");
			return render_login_message(server_status.message.empty()
				? std::string("NovaWorld is temporarily unavailable.")
				: server_status.message);
		}

		LoginSession s;
		s.session_tag  = sessions_.generate_tag("NWLogin.dll");

		// Three-tier identity resolution:
		//  1. EPASK-decrypted NAME + PASSWORD → authenticate_user.
		//     This is the real-auth path retail follows.
		//  2. PERSISTENTEXPRESSLOGINDATA pin (G.8) → DB lookup.
		//     Used when retail somehow skips the form (rare) or when
		//     the persistent cookie is the only thing identifying the
		//     process.
		//  3. Round-robin fallback over seeded `players`.
		//     Only kicks in for curl walkthroughs that don't carry an
		//     EPASK form field (real retail always does).
		const auto persist_it = cookies.find("PERSISTENTEXPRESSLOGINDATA");
		const std::string persist_cookie = persist_it == cookies.end() ? std::string() : persist_it->second;

		std::optional<UserRecord> user;
		const char *resolution = nullptr;
		if (!login_name.empty() && !login_password.empty()) {
			// The login brake POST /api/login runs (WebAccess) on its retail
			// buckets: the address's attempts, then one token from the
			// username's failure buckets, handed back when the password is
			// right. Keyed by the TCP peer: a stock client reaches this port
			// directly (the gate's startupurl names ONNET_PUBLIC_HOST and the
			// HTTP port, through no proxy), so no header names the client and
			// nothing the website's brake sees can brake this one; only the
			// per-username cap is shared, and it spares the account's known
			// addresses. Its refusal is this route's own failure reply, the
			// msgbase page with a message (as a bad password and maintenance
			// get), which a stock client shows; no retail throttling reply is
			// witnessed (service policy).
			const std::string &peer = req.remote_ip_address;
			LoginTicket ticket;
			if (impl_->access.take_login_address(LoginRoute::Retail, peer) != 0 ||
					impl_->access.begin_password_check(
							*db_conn, LoginRoute::Retail, login_name, peer, ticket) != 0) {
				std::fprintf(stderr,
						"[http] POST /NWLogin.dll rate-limited for user '%s' from %s\n",
						loggable(login_name).c_str(), loggable(peer).c_str());
				return render_login_message(kLoginThrottledMessage);
			}
			user = authenticate_user(*db_conn, login_name, login_password);
			if (user) {
				resolution = "epask-auth";
				impl_->access.password_accepted(*db_conn, ticket, user->id, user->credentials_gen);
			} else {
				// Bad credentials → render the failure template (jop_2_main.htm
				// per retail's form `failure` arg) with a "Invalid username or
				// password" message and bail before storing a session.
				std::fprintf(stderr, "[http] POST /NWLogin.dll auth failed for user '%s'\n",
				             loggable(login_name).c_str());
				return render_login_message("Invalid username or password");
			}
		}
		if (!user && !persist_cookie.empty()) {
			user = pinned_user(persist_cookie, *db_conn);
			if (user)
				resolution = "persist-pin";
		}
		if (!user) {
			// No EPASK-authenticated user and no persist-pin match → reject.
			// Login requires real authentication; we never invent a session
			// from a seeded/stub user. Retail always carries EPASK creds or a
			// persist cookie, so a legitimate client never lands here. (Same
			// behaviour in dev and prod — dev just seeds the test accounts.)
			std::fprintf(stderr,
			             "[http] POST /NWLogin.dll rejected: unauthenticated\n");
			return render_login_message("Invalid username or password");
		}
		std::string effective_exp_bits = game.exp_bits;
		if (user->id != 0) {
			const std::string &account_status = user->account_status;
			if (account_status == "banned") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld banned\n",
				            static_cast<long long>(user->id));
				return render_login_message("This NovaWorld account has been banned. NWEC11");
			}
			if (account_status != "active") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld status=%s\n",
						static_cast<long long>(user->id), loggable(account_status).c_str());
				return render_login_message("This NovaWorld account is restricted. NWEC12");
			}

			const auto access = get_game_access(*db_conn, user->id, game_slug);
			if (!access) {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld no access to %s\n",
				            static_cast<long long>(user->id), game_slug.c_str());
				return render_login_message("This NovaWorld account does not have access to this game.");
			}
			if (access->status == "banned") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld game=%s banned\n",
				            static_cast<long long>(user->id), game_slug.c_str());
				return render_login_message("This NovaWorld account has been banned for this game. NWEC11");
			}
			if (access->status != "active") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld game=%s status=%s\n",
						static_cast<long long>(user->id), game_slug.c_str(),
						loggable(access->status).c_str());
				return render_login_message("This NovaWorld account is restricted for this game. NWEC12");
			}
			if (!access->exp_bits.empty()) {
				effective_exp_bits = access->exp_bits;
			}
			// Re-login supersedes a prior session rather than being rejected.
			// Retail has no witnessed "already logged in" gate — the active-user
			// row is cleared on /NWLogout.dll, not used to block login
			// (docs/net/novaworld-net-re.md:55). A hard reject here stranded the
			// account whenever a prior session ended without a clean logout
			// (client crash / GOODBYE-less exit): the orphaned row blocked every
			// retry until the 2h TTL sweep. register_active_user_session() below
			// is an upsert on the user_id PK, so the stale row is simply
			// overwritten by this fresh login.
		}
		// The login's session row and its pin are written only while the account
		// is active and still at the credentials generation its password check
		// (or pin) read, the row tested by its own insert, so a reset or a ban
		// that committed meanwhile leaves this login neither, and the reply is a
		// bad password's. A pin stored after such a commit still carries the old
		// generation, which pinned_user refuses.
		bool registered = false;
		try {
			registered = register_active_user_session(*db_conn, user->id, user->credentials_gen,
					user->username, s.session_tag, persist_cookie, req.remote_ip_address,
					req.get_header_value("User-Agent"));
		} catch (const std::exception &e) {
			std::fprintf(
					stderr, "[http] WARN active_user_sessions register failed: %s\n", e.what());
			return render_login_message("NovaWorld is temporarily unavailable.");
		}
		if (!registered) {
			std::fprintf(stderr,
					"[http] POST /NWLogin.dll rejected: user %lld changed during the check\n",
					static_cast<long long>(user->id));
			return render_login_message("Invalid username or password");
		}
		if (!persist_cookie.empty()) {
			std::lock_guard<std::mutex> lk(persistent_user_mu_);
			persistent_pins_[persist_cookie] = RetailPin{ user->id, user->credentials_gen };
		}
		s.user_id  = user->id;
		s.username = user->username;
		s.pcid     = user->pcid;
		s.nwh      = user->nwh;
		s.nwhandle = user->nwhandle;
		s.exp_bits = effective_exp_bits;
		std::printf(
				"[http]   user resolved (%s): id=%lld username=%s pcid=%s nwhandle=%s persist=%s\n",
				resolution ? resolution : "?", static_cast<long long>(s.user_id),
				loggable(s.username).c_str(), loggable(s.pcid).c_str(),
				loggable(s.nwhandle).c_str(), loggable_secret(persist_cookie).c_str());

		// The relay template renders this response only; nothing reads it back
		// from the stored session.
		const std::string relay_template = field_or("relay", game.page("relay.htm"));
		s.msgbase      = field_or("msgbase",     game.page("msg.htm"));
		s.success      = field_or("success",     game.page("main.htm"));
		s.failure      = field_or("failure",     game.page("main.htm"));

		// Capture everything we need from `s` BEFORE moving it into the
		// session map — a use-after-move here once returned empty strings →
		// empty response bodies → retail re-POST'd until GOODBYE.
		const std::string tag = s.session_tag;

		sessions_.put_login(tag, std::move(s));

		std::printf("[http] POST %s body=%zuB user=%s relay->%s success->%s -> tag %s\n",
				loggable(req.url).c_str(), req.body.size(),
				loggable(sessions_.get_login(tag).value().username).c_str(),
				loggable(relay_template).c_str(),
				loggable(sessions_.get_login(tag).value().success).c_str(),
				loggable_secret(tag).c_str());

		// Relay refresh target is the BARE "NWLogin.dll" — matching onnet AND
		// the genuine NovaLogic server (Wireshark capture, docs/net/
		// novaworld-net-re.md:1303-1352: retail polls plain `GET /NWLogin.dll`
		// and its LOGINSESSIONTAG cookie round-trips, then the server plants
		// NWHANDLE/PCID). The prior `?tag=` workaround (for a supposed HTTP/1.0
		// cookie drop the capture shows the real client does NOT have on the
		// bare URL) re-routed the completion GET so retail never stored the
		// identity cookies — they never reached /NWJoin.dll, leaving PUBPCID
		// empty ("login information is absent (GDC024)"). The handler still
		// falls back to the LOGINSESSIONTAG cookie to recover the tag.
		TemplateVars vars{
			{"MESSAGE",          "Contacting login databases..."},
			{"REFRESH_ENDPOINT", "NWLogin.dll"},
			{"HOST_URL",         host_url()},
			{"GSB_SERVER",       gsb_url(game)},
			{"JOINLAN_URL",      ""},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, relay_template, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",         req.remote_ip_address);
		add_cookie(res, "LOGINSESSIONTAG", tag);
		// Empty placeholders the relay GET will populate after auth.
		for (const char *name : {"NWH","NWHANDLE","CHAR","NWI","NWV","NWD","STATSDISPLAYCHID","PCID"}) {
			add_cookie(res, name, "");
		}
		add_cookie(res, "EXPBITS", effective_exp_bits);
		return res;
	};
	CROW_ROUTE(app, "/NWLogin.dll").methods("POST"_method)(handle_login_post);
	CROW_ROUTE(app, "/nwlogin.dll").methods("POST"_method)(handle_login_post);

	// GET /NWLogin.dll  (relay refresh — completes auth)
	auto handle_login_get = [this, templates_dir](const crow::request &req) {
		// Prefer ?tag= query param (set by our relay template's refresh
		// URL). Fall back to LOGINSESSIONTAG cookie for clients that do
		// preserve it (curl, Vue dev, Godot).
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		std::string tag;
		if (req.url_params.get("tag")) {
			tag = req.url_params.get("tag");
		} else {
			auto tag_it = cookies.find("LOGINSESSIONTAG");
			if (tag_it != cookies.end()) tag = tag_it->second;
		}
		if (tag.empty()) {
			std::printf("[http] GET %s -> 400 (no tag in query or cookie)\n",
					loggable(req.url).c_str());
			crow::response res(400);
			res.body = "missing LOGINSESSIONTAG (cookie or ?tag= query)";
			return res;
		}
		auto session = sessions_.get_login(tag);
		if (!session) {
			// Retail can land here with a tag from a previous server run
			// (cached in PERSISTENTEXPRESSLOGINDATA / cookie jar). Rather
			// than 400 → blank screen, mint a fresh login session and
			// round-robin a dev user. Log it loudly so the cookie bridge
			// is visible.
			std::printf("[http] GET %s WARN stale/unknown tag '%s' — minting fresh login session\n",
					loggable(req.url).c_str(), loggable_secret(tag).c_str());
			// Mint a fresh, UNAUTHENTICATED session so retail doesn't hit a
			// blank screen on a cross-restart tag — but it carries no user.
			// The POST /NWLogin.dll path is the only auth gate; a stale tag
			// must re-authenticate there rather than be silently re-bound to
			// an account here. (user_id stays 0 → no active session, and any
			// authenticated action downstream still requires a real login.)
			const catalog::GameRow &fresh_game = client_game(impl_->games, req, cookies, {}, {});
			LoginSession fresh;
			fresh.session_tag = sessions_.generate_tag("NWLogin.dll");
			fresh.success = fresh_game.page("main.htm");
			fresh.failure = fresh_game.page("main.htm");
			fresh.msgbase = fresh_game.page("msg.htm");
			tag = fresh.session_tag;
			sessions_.put_login(tag, std::move(fresh));
			session = sessions_.get_login(tag);
		}

		std::printf("[http] GET %s tag=%s -> auth ok (user=%s pcid=%s)\n",
				loggable(req.url).c_str(), loggable_secret(tag).c_str(),
				loggable(session->username).c_str(), loggable(session->pcid).c_str());
		if (session->user_id != 0) {
			try { touch_active_user_session(*db_pool_.acquire(), session->user_id); }
			catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN active_user_sessions touch failed: %s\n",
				             e.what());
			}
		}

		// The main page this renders lists the client's game's servers.
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {}, session->success);
		TemplateVars vars{
			{"IN",           session->success},
			{"OUT",          session->failure},
			{"MSGBASE",      session->msgbase},
			{"HOST_URL",     host_url()},
			{"GSB_SERVER",   gsb_url(game)},
			{"JOINLAN_URL",  ""},
		};
		const std::string success_template = session->success.empty() ? game.page("main.htm") : session->success;
		crow::response res(200);
		res.body = render_template_file(templates_dir, success_template, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",          req.remote_ip_address);
		add_cookie(res, "LOGINSESSIONTAG", tag);
		add_cookie(res, "NWH",      session->nwh.empty()      ? "1"          : session->nwh);
		add_cookie(res, "NWHANDLE", session->nwhandle.empty() ? "DevUser"    : session->nwhandle);
		add_cookie(res, "CHAR",     session->nwhandle.empty() ? "DevUser"    : session->nwhandle);
		add_cookie(res, "PCID",     session->pcid.empty()     ? "00000001"   : session->pcid);
		add_cookie(res, "EXPBITS",  session->exp_bits.empty() ? "3" : session->exp_bits);
		// A DFX2 client not yet activated: its machine's NWACT, which its NovaWorld screen
		// writes to the registry CDKIID (cd_key_routes.h).
		const std::string nwact = impl_->cd_keys.login_activation(session->user_id, game, cookies);
		if (!nwact.empty())
			add_cookie(res, "NWACT", nwact);

		// Echo the client's persistent/express-login cookies back so retail's
		// logged-in state stays valid and these survive to later requests —
		// notably the joiner's identity at /NWJoin.dll. Without this the joiner
		// fails with "Logging information is missing or invalid" (its PUB* join
		// cookies come out empty because no identity carried over). Echo only
		// what the client sent. Mirrors onnw/controllers/nova_world/login.py:159-171.
		{
			const auto req_cookies = parse_cookie_header(request_cookie_header(req));
			for (const char *name : {"PERSISTENTREMEMBERLOGINDATA",
			                         "PERSISTENTEXPRESSLOGINDATA",
			                         "NWI", "NWV", "NWD", "STATSDISPLAYCHID"}) {
				auto it = req_cookies.find(name);
				if (it != req_cookies.end()) add_cookie(res, name, it->second);
			}
		}

		sessions_.erase_login(tag);
		return res;
	};
	CROW_ROUTE(app, "/NWLogin.dll").methods("GET"_method)(handle_login_get);
	CROW_ROUTE(app, "/nwlogin.dll").methods("GET"_method)(handle_login_get);

	// ----- Phase I.1: /NWLogout.dll real teardown ------------------------
	// Drops the HTTP-side LoginSession and the persistent-cookie pin so
	// the same retail process won't auto-resume as the prior identity.
	// UDP side keeps running until GOODBYE/heartbeat-timeout — that's the
	// protocol's contract; logout here is HTTP-only.
	auto handle_logout = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		std::string tag;
		if (req.url_params.get("tag")) {
			tag = req.url_params.get("tag");
		} else if (auto it = cookies.find("LOGINSESSIONTAG"); it != cookies.end()) {
			tag = it->second;
		}
		if (!tag.empty()) {
			sessions_.erase_login(tag);
			try { clear_active_user_session_by_tag(*db_pool_.acquire(), tag); }
			catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN active_user_sessions clear by tag failed: %s\n",
				             e.what());
			}
		}

		const auto persist_it = cookies.find("PERSISTENTEXPRESSLOGINDATA");
		if (persist_it != cookies.end() && !persist_it->second.empty()) {
			int64_t pinned_id = 0;
			std::lock_guard<std::mutex> lk(persistent_user_mu_);
			auto pin_it = persistent_pins_.find(persist_it->second);
			if (pin_it != persistent_pins_.end())
				pinned_id = pin_it->second.user_id;
			persistent_pins_.erase(persist_it->second);
			if (pinned_id != 0) {
				try { clear_active_user_session(*db_pool_.acquire(), pinned_id); }
				catch (const std::exception &e) {
					std::fprintf(stderr, "[http] WARN active_user_sessions clear failed: %s\n",
					             e.what());
				}
			}
		}

		const char *success_param = req.url_params.get("success");
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {},
		                                           success_param ? success_param : "");
		const std::string success_tpl = success_param ? success_param : game.page("login.htm");
		std::printf("[http] /NWLogout.dll tag=%s persist_dropped=%d -> %s\n",
				loggable_secret(tag).c_str(), persist_it != cookies.end() ? 1 : 0,
				loggable(success_tpl).c_str());

		TemplateVars vars{
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", ""},
		};
		crow::response res(200);
		if (template_file(templates_dir, success_tpl)) {
			res.body = render_template_file(templates_dir, success_tpl, vars);
			res.set_header("Content-Type", "text/html");
		} else {
			res.body = "Logged out.";
			res.set_header("Content-Type", "text/plain");
		}
		add_standard_headers(res);
		add_cookie(res, "YOURIP", req.remote_ip_address);
		// Clear identity cookies so retail's IB3 doesn't auto-resume.
		// (We don't explicitly clear PERSISTENTEXPRESSLOGINDATA — retail
		// owns that one.)
		for (const char *name : {"NWH","NWHANDLE","CHAR","NWI","NWV","NWD",
		                          "STATSDISPLAYCHID","PCID","EXPBITS",
		                          "LOGINSESSIONTAG"}) {
			add_cookie(res, name, "");
		}
		return res;
	};
	app.route_dynamic("/NWLogout.dll")(handle_logout);
	app.route_dynamic("/nwlogout.dll")(handle_logout);

	// Phase I.6: per-user template renderer for /NWCharacter.dll and
	// /NWAccount.dll. Both are retail's "tell me about my account/char"
	// queries; onnet doesn't even route them but retail's IB3 expects
	// them to return a template populated with the current user's
	// identity (so the in-game UI can show "Logged in as X").
	//
	// Identity is the request's own retail login (retail_login_user: its
	// LOGINSESSIONTAG, else its PERSISTENTEXPRESSLOGINDATA pin), an active
	// account only; never the NWHANDLE cookie, which names an account but
	// proves nothing.
	auto handle_generic = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		const char *success_param = req.url_params.get("success");
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {},
		                                           success_param ? success_param : "");
		const std::string tpl = success_param ? success_param : game.page("main.htm");
		const bool exists = template_file(templates_dir, tpl).has_value();

		// Resolve identity. Best-effort — if neither cookie resolves we
		// just render with empty identity vars (the template can still
		// be served for unauthenticated paths).
		auto db_conn = db_pool_.acquire();
		std::optional<UserRecord> user;
		try {
			const char *how = "";
			user = retail_login_user(cookies, *db_conn, how);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] WARN retail login lookup failed: %s\n", e.what());
		}
		if (user && user->account_status != "active")
			user.reset();
		std::printf("[http] %s -> %s%s user=%s\n", loggable(req.url).c_str(), loggable(tpl).c_str(),
				exists ? "" : " (MISSING — 404)",
				user ? loggable(user->username).c_str() : "(unknown)");
		if (!exists) {
			crow::response res(404);
			// The name is echoed only when it is one a template may have.
			res.body = "template not found: " + (is_bare_file_name(tpl) ? tpl : std::string());
			res.set_header("Content-Type", "text/plain");
			return res;
		}
		TemplateVars vars{
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", ""},
			{"NWHANDLE",    user ? user->nwhandle : std::string()},
			{"PCID",        user ? user->pcid     : std::string()},
			{"NWH",         user ? user->nwh      : std::string()},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, tpl, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP", req.remote_ip_address);
		// Refresh identity cookies so retail keeps consistent state.
		if (user) {
			add_cookie(res, "NWHANDLE", user->nwhandle);
			add_cookie(res, "CHAR",     user->nwhandle);
			add_cookie(res, "PCID",     user->pcid);
			add_cookie(res, "NWH",      user->nwh);
		}
		return res;
	};
	for (const char *route : {"/NWCharacter.dll","/nwcharacter.dll",
	                          "/NWAccount.dll","/nwaccount.dll"}) {
		app.route_dynamic(route)(handle_generic);
	}
}

// Retail host/join flow: the per-game GSB binary browser blobs, the
// /NWJoin.dll two-phase relay, and the /NWHost.dll host-key mint. Join and
// host register both lowercase and Title-case spellings for the same reason
// as the login chain: Crow routes are case-sensitive.
void HttpListener::register_legacy_host_join_routes(
		const std::string &templates_dir) {
	auto &app = impl_->app;

	// ----- Phase E.2: GSB (Game Server Browser) ---------------------------
	// Build a GSB binary blob from the in-memory hosted-server list (every
	// connection that issued ClientHostRequest in the lobby session). Wire
	// format from engine/net/novaworld/gsb.h is byte-exact with
	// onnet's onnw/gsb.py, so retail's IB3 browser parser accepts it.
	//
	// One route per games row, at its gsb_path (/jop_2.gsb, /dfx2_0.gsb,
	// /opennova_0.gsb), listing the hosts registered under its LobbyName; a
	// column a host reported nothing for shows its game's VER1 / Expbits.
	auto handle_gsb = [this](const catalog::GameRow &game) {
		// Phase I.2: backed by active_hosts, filtered to the requested game
		// (onnet serves each *.gsb from a per-game query — without the filter
		// a DFX2 host would leak into the JO browser and vice versa).
		std::vector<opennova::GsbServerEntry> entries;
		try {
			auto db_conn = db_pool_.acquire();
			// One snapshot for the rows and their rosters: a row's player count
			// and the names in its tail come from the same commit.
			db::ReadSnapshot snapshot(*db_conn);
			auto rows = hostdb::list_hosts_by_game(*db_conn, game.lobby_name);
			entries.reserve(rows.size());
			for (const auto &h : rows) {
				// Every FLDS column carries the host-reported value and the row
				// tail carries the host's roster names (hostdb::gsb_entry_from_host).
				entries.push_back(hostdb::gsb_entry_from_host(
						h, hostdb::list_roster(*db_conn, h.rid), {game.ver1, game.exp_bits}));
			}
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] GSB list failed: %s\n", e.what());
		}
		const auto blob = opennova::gsb_build_response(entries);
		std::printf("[http] GET %s (%s) -> %zu hosts, %zu bytes\n", game.gsb_route().c_str(),
		            game.slug.c_str(), entries.size(), blob.size());
		crow::response res(200);
		res.set_header("Content-Type", "application/octet-stream");
		res.body.assign(blob.begin(), blob.end());
		return res;
	};
	std::set<std::string> gsb_routes;
	for (const catalog::GameRow &game : impl_->games.games()) {
		const std::string route = game.gsb_route();
		if (game.gsb_path.empty() || !gsb_routes.insert(route).second) {
			std::fprintf(stderr, "[http] WARN games row %s: no .gsb route (gsb_path '%s' %s)\n",
			             loggable(game.slug).c_str(), loggable(game.gsb_path).c_str(),
			             game.gsb_path.empty() ? "empty" : "served by another row");
			continue;
		}
		app.route_dynamic(route)([handle_gsb, game](const crow::request &) { return handle_gsb(game); });
	}

	// ----- Phase E.3: /NWJoin.dll two-phase relay -------------------------
	// Mirror onnet's onnw/controllers/nova_world/join.py.
	//   First call (no NWJOINSESSIONTAG cookie):  store params -> render
	//                                             relay template, set
	//                                             session cookie.
	//   Second call (with cookie):                look up session, find
	//                                             hosted entry by RID, encode
	//                                             NK/CK, render .joi
	//                                             success template.
	auto handle_join = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		// Same HTTP/1.0 cookie-loss workaround as /NWLogin.dll: prefer
		// ?tag= URL param (we set it in REFRESH_ENDPOINT below) and fall
		// back to NWJOINSESSIONTAG cookie for clients that preserve it.
		std::string tag_from_request;
		if (req.url_params.get("tag")) {
			tag_from_request = req.url_params.get("tag");
		} else {
			auto tag_it = cookies.find("NWJOINSESSIONTAG");
			if (tag_it != cookies.end()) tag_from_request = tag_it->second;
		}

		// The page link's query (rid=NNN and the page fields) is the first call
		// even when the client's persistent jar still carries the tag a previous
		// join or host left: a stock client sends every jar cookie on every GET
		// (net/novaworld/relay_request.h). Only our client's `?tag=` follow-up and
		// the relay page's bare refresh resolve a tag.
		const bool first_call =
				classify_relay_request(req.url_params.get("tag") != nullptr,
				                       has_relay_first_call_query(req),
				                       !tag_from_request.empty()) ==
				RelayLeg::First;

		if (first_call) {
			const char *pfid_param = req.url_params.get("pfid");
			const char *success_param = req.url_params.get("success");
			const catalog::GameRow &game = client_game(impl_->games, req, cookies,
					pfid_param ? pfid_param : "", success_param ? success_param : "");
			JoinSession s;
			s.session_tag = sessions_.generate_tag("NWJoin.dll");
			s.success     = success_param                    ? success_param                    : game.page("join.joi");
			s.failure     = req.url_params.get("failure")    ? req.url_params.get("failure")    : game.page("main.htm");
			const std::string relay_template =
					req.url_params.get("relay") ? req.url_params.get("relay") : game.page("relay.htm");
			s.msgbase     = req.url_params.get("msgbase")    ? req.url_params.get("msgbase")    : game.page("msg.htm");
			s.needexpkey  = req.url_params.get("needexpkey") ? req.url_params.get("needexpkey") : "";
			s.pfid        = req.url_params.get("pfid")       ? req.url_params.get("pfid")       : "";
			s.rid         = req.url_params.get("rid")        ? req.url_params.get("rid")        : "";
			const std::string tag = s.session_tag;
			sessions_.put_join(tag, std::move(s));

			std::printf(
					"[http] /NWJoin.dll (first call) rid=%s success=%s -> tag %s cookies=[%s]\n",
					req.url_params.get("rid") ? loggable(req.url_params.get("rid")).c_str()
											  : "(none)",
					req.url_params.get("success") ? loggable(req.url_params.get("success")).c_str()
												  : "(default)",
					loggable_secret(tag).c_str(),
					cookie_summary(request_cookie_header(req)).c_str());

			// Bare "NWJoin.dll" refresh — matches onnet and the genuine server;
			// the NWJOINSESSIONTAG cookie carries the tag (see the /NWLogin.dll
			// relay note above re: dropping the divergent `?tag=` workaround).
			TemplateVars vars{
				{"MESSAGE",          "Contacting game server...."},
				{"REFRESH_ENDPOINT", "NWJoin.dll"},
				{"HOST_URL",         host_url()},
				{"GSB_SERVER",       gsb_url(game)},
				{"JOINLAN_URL",      ""},
			};
			crow::response res(200);
			res.body = render_template_file(templates_dir, relay_template, vars);
			res.set_header("Content-Type", "text/html");
			add_standard_headers(res);
			add_cookie(res, "YOURIP",            req.remote_ip_address);
			add_cookie(res, "NWJOINSESSIONTAG",  tag);
			return res;
		}

		// Second call — tag came either from ?tag= or NWJOINSESSIONTAG.
		// Diagnostic (join-failure triage): show how the tag arrived (proves the
		// NWJOINSESSIONTAG cookie round-trips on the bare refresh URL) and the
		// full inbound cookie set — LOGINSESSIONTAG / PERSISTENTEXPRESSLOGINDATA
		// decide the joiner identity that PUBPCID is encoded from.
		std::printf("[http] /NWJoin.dll (second call) tag=%s (from %s) cookies=[%s]\n",
				loggable_secret(tag_from_request).c_str(),
				req.url_params.get("tag") ? "query" : "cookie",
				cookie_summary(request_cookie_header(req)).c_str());
		const auto session = sessions_.get_join(tag_from_request);
		if (!session) {
			std::printf("[http] /NWJoin.dll (second call) WARN unknown tag %s\n",
					loggable_secret(tag_from_request).c_str());
			crow::response res(400);
			res.body = "unknown NWJOINSESSIONTAG";
			return res;
		}

		// Resolve RID — query param wins, falls back to stored value.
		std::string rid_str = req.url_params.get("rid") ? req.url_params.get("rid") : session->rid;
		if (rid_str.empty()) {
			crow::response res(400);
			res.body = "missing RID";
			return res;
		}
		const auto rid_parsed = opennova::strutil::parse_ulong(rid_str);
		if (!rid_parsed) {
			crow::response res(400);
			res.body = "bad RID";
			return res;
		}
		const uint32_t rid_value = static_cast<uint32_t>(*rid_parsed);

		// Look up the hosted entry from active_hosts (Phase I.2 — DB-backed).
		auto db_conn = db_pool_.acquire();
		auto host_row = hostdb::find_host_by_rid(*db_conn, rid_value);
		if (!host_row) {
			std::fprintf(stderr, "[http] /NWJoin.dll RID %u not in active_hosts\n",
			             static_cast<unsigned>(rid_value));
			crow::response res(404);
			res.body = "host not found";
			return res;
		}
		const auto &host = *host_row;

		// _encode_token (from base.py:113-116):
		//   chr(ord(value[i]) + ord(key[i]) - 48)
		// NOVAKEY_KEY/CK_KEY are 21 chars; encoded values must fit.
		static constexpr const char *NOVAKEY_KEY = "diheijefhgcdjcgcjcfbd";
		static constexpr const char *CK_KEY      = "cfhdcegjigecjehcgjdhe";
		static constexpr const char *BK_VALUE    = "986119";
		auto encode_token = [](const std::string &value, const char *key) -> std::string {
			std::string out;
			const size_t key_len = std::strlen(key);
			out.reserve(value.size());
			for (size_t i = 0; i < value.size() && i < key_len; ++i) {
				out.push_back(static_cast<char>(value[i] + key[i] - 48));
			}
			return out;
		};

		const std::string nk_plain = host.host_ip + ":" + std::to_string(host.host_port);
		const std::string ck_plain = host.app_id;
		const std::string nk_token = encode_token(nk_plain, NOVAKEY_KEY);
		const std::string ck_token = encode_token(ck_plain, CK_KEY);

		std::printf("[http] /NWJoin.dll (second call) rid=%u host=%s:%d nk=%s ck=%s\n",
				static_cast<unsigned>(rid_value), loggable(host.host_ip).c_str(), host.host_port,
				loggable(nk_token).c_str(), loggable(ck_token).c_str());

		// The joiner's game, and the listing's: the row its LobbyName names
		// (the joiner's for a listing with none; null for a LobbyName no row
		// has, which no game's access covers).
		const catalog::GameRow &game =
				client_game(impl_->games, req, cookies, session->pfid, session->success);
		const catalog::GameRow *host_game =
				host.game.empty() ? &game : impl_->games.by_lobby_name(host.game);
		TemplateVars vars{
			{"NK",          nk_token},
			{"CK",          ck_token},
			{"NI",          host.host_ip},
			{"NP",          std::to_string(host.host_port)},
			{"BK",          BK_VALUE},
			{"SERVER_NAME", host.server_name.empty() ? std::string("OpenNova Server") : host.server_name},
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", ""},
		};
		const std::string success_template = session->success.empty() ? game.page("join.joi") : session->success;
		crow::response res(200);
		res.body = render_template_file(templates_dir, success_template, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",           req.remote_ip_address);
		add_cookie(res, "NWJOINSESSIONTAG", tag_from_request);

		// PUBcrypto-encoded join cookies (Phase E.4). Retail host-side join
		// reads CD/<localaddr>{PCID,NAMEINFO,SQUADINFO}; SQUADINFO supplies
		// the remote player's display/short names before validation.
		// The joiner is the request's own retail login (retail_login_user: its
		// LOGINSESSIONTAG, else the PERSISTENTEXPRESSLOGINDATA pin its login
		// made, G.8), never the NWHANDLE cookie, which any client can set to
		// any account's handle. An account that is not active is refused with
		// the failure page a missing expansion gets, carrying the game login's
		// NWEC11 / NWEC12 message.
		std::string pub_pcid;
		std::string pub_nameinfo;
		std::string pub_squadinfo;
		const std::string host_pcid_key = host.pcid_key;
		std::string joiner_pcid;
		std::string joiner_nwhandle;
		int64_t     joiner_user_id = 0;
		std::string joiner_label;  // for logging
		{
			const std::string msg_tpl =
					session->msgbase.empty() ? game.page("msg.htm") : session->msgbase;
			const std::string fail_tpl =
					session->failure.empty() ? game.page("main.htm") : session->failure;
			std::optional<UserRecord> joiner;
			const char *how = "";
			try {
				joiner = retail_login_user(cookies, *db_conn, how);
			} catch (const db::Error &e) {
				std::fprintf(
						stderr, "[http] /NWJoin.dll WARN joiner lookup failed: %s\n", e.what());
			}
			if (joiner && joiner->account_status != "active") {
				std::printf("[http] /NWJoin.dll rejected: joiner=%lld status=%s\n",
						static_cast<long long>(joiner->id),
						loggable(joiner->account_status).c_str());
				return render_legacy_message(templates_dir,
						joiner->account_status == "banned"
								? "This NovaWorld account has been banned. NWEC11"
								: "This NovaWorld account is restricted. NWEC12",
						fail_tpl, msg_tpl, req.remote_ip_address, host_url(), gsb_url(game));
			}
			if (joiner) {
				joiner_user_id = joiner->id;
				joiner_pcid = joiner->pcid;
				joiner_nwhandle = joiner->nwhandle;
				joiner_label = std::string(how) + "=" + joiner->nwhandle;
			}
			if (joiner_user_id != 0) {
				const std::string host_slug = host_game ? host_game->slug : host.game;
				const std::string required_exp_bits = !host.exp_bits.empty() ? host.exp_bits
					: host_game ? host_game->exp_bits : std::string();
				const auto access = get_game_access(*db_conn, joiner_user_id, host_slug);
				const bool access_ok = access && access->status == "active" &&
					expansion_bits_compatible(access->exp_bits, required_exp_bits);
				if (!access_ok) {
					std::printf("[http] /NWJoin.dll rejected: joiner=%lld game=%s owned_exp=%s "
								"required_exp=%s status=%s\n",
							static_cast<long long>(joiner_user_id), loggable(host_slug).c_str(),
							access ? loggable(access->exp_bits).c_str() : "(none)",
							loggable(required_exp_bits).c_str(),
							access ? loggable(access->status).c_str() : "(none)");
					return render_legacy_message(templates_dir,
							"This NovaWorld account does not have the required expansion key.",
							session->needexpkey.empty() ? fail_tpl : session->needexpkey, msg_tpl,
							req.remote_ip_address, host_url(), gsb_url(game));
				}
			}
			if (host_pcid_key.empty()) {
				std::fprintf(stderr, "[http] /NWJoin.dll WARN host pcid_key missing - PUB* cookies will be empty (joiner cookies=[%s])\n",
				             cookie_summary(request_cookie_header(req)).c_str());
			} else if (joiner_pcid.empty()) {
				std::fprintf(stderr, "[http] /NWJoin.dll WARN no joiner identity - PUB* cookies will be empty (joiner cookies=[%s])\n",
				             cookie_summary(request_cookie_header(req)).c_str());
			} else {
				const auto payloads =
					opennova::build_pub_join_identity_plaintexts(joiner_pcid, joiner_nwhandle);
				// A refused encode (only an empty key is refused, ruled out above)
				// leaves every PUB* cookie empty, as a missing host key does.
				const bool encoded =
					opennova::encode_pub_value(payloads.pcid, host_pcid_key, pub_pcid) &&
					(payloads.name_info.empty() ||
					 opennova::encode_pub_value(payloads.name_info, host_pcid_key, pub_nameinfo)) &&
					(payloads.squad_info.empty() ||
					 opennova::encode_pub_value(payloads.squad_info, host_pcid_key, pub_squadinfo));
				if (!encoded) {
					pub_pcid.clear();
					pub_nameinfo.clear();
					pub_squadinfo.clear();
					std::fprintf(stderr, "[http] /NWJoin.dll PUB* encode refused (host_key=%zuB)\n",
					             host_pcid_key.size());
				} else {
					if (payloads.name_info.empty() || payloads.squad_info.empty()) {
						std::fprintf(stderr,
								"[http] /NWJoin.dll WARN no joiner display handle - "
								"PUBNAMEINFO/PUBSQUADINFO will be empty (joiner=%s cookies=[%s])\n",
								loggable(joiner_label).c_str(),
								cookie_summary(request_cookie_header(req)).c_str());
					}
					std::printf("[http] /NWJoin.dll PUB* encoded for joiner=%s pcid=%s nwhandle=%s host_key=%zuB name=%zuB squad=%zuB\n",
					            loggable(joiner_label).c_str(), loggable(joiner_pcid).c_str(),
					            loggable(joiner_nwhandle).c_str(), host_pcid_key.size(),
					            payloads.name_info.size(), payloads.squad_info.size());
				}
			}
			// Phase I.3: persist the join in host_players. Cascade-deletes
			// when the host row goes away (host disconnect). We don't have
			// the joiner's UDP peer port at this point — they're still on
			// HTTP — so peer_port=0 placeholder; a future refinement would
			// correlate the joiner's lobby UDP src with this row.
			if (joiner_user_id != 0) {
				try {
					hostdb::PlayerRow p;
					p.host_rid  = host.rid;
					p.user_id   = joiner_user_id;
					p.nwhandle  = joiner_nwhandle.empty()
					                ? joiner_label
					                : joiner_nwhandle;
					p.peer_ip   = req.remote_ip_address;
					p.peer_port = 0;
					hostdb::add_player(*db_conn, p);
				} catch (const std::exception &e) {
					std::fprintf(stderr, "[http] /NWJoin.dll WARN host_players add: %s\n", e.what());
				}
			}
		}

		add_cookie(res, "PUBPCID",       pub_pcid);
		add_cookie(res, "PUBNAMEINFO",   pub_nameinfo);
		add_cookie(res, "PUBSQUADINFO",  pub_squadinfo);
		// JOINTICKET: onnet hardcodes a literal "JT:..." string (witnessed
		// from a real retail capture); not derived per-session in dev.
		add_cookie(res, "PUBJOINTICKET", "JT:0a000013143f1efe6c8000767fd748e1127304c5b6d728");

		// NW*/AFF* still stubbed empty — onnet sets them via setdefault to "",
		// which is what we do here. They feed deeper post-game stat replication
		// that we don't implement yet.
		for (const char *name : {"NWPCID","NWNAMEINFO","NWSQUADINFO","NWTI","NWTV",
		                          "NWJOINTICKET",
		                          "AFFPCID","AFFNAMEINFO","AFFSQUADINFO","AFFJOINTICKET"}) {
			add_cookie(res, name, "");
		}
		add_cookie(res, "NWPF",  session->pfid);
		add_cookie(res, "NWPF2", "0");
		return res;
	};
	CROW_ROUTE(app, "/NWJoin.dll").methods("GET"_method)(handle_join);
	CROW_ROUTE(app, "/nwjoin.dll").methods("GET"_method)(handle_join);

	// ----- Phase F.2: /NWHost.dll two-phase relay -------------------------
	// Mirrors onnet's onnw/controllers/nova_world/host.py.
	//   First call (no NWJOINSESSIONTAG cookie OR ?tag= query):
	//     - generate session + 48-char A-P host_key (24 random bytes nibble-encoded)
	//     - record the key's hash with the account the request's retail login
	//       names, an active account (retail_login_user -> hostdb::record_host_key), if any
	//     - store HostSession with the client's game's default templates (JO's
	//       jop_2_host2.htm etc.) where the page link names none
	//     - render relay template with REFRESH_ENDPOINT="NWHost.dll" (bare; the
	//       NWJOINSESSIONTAG cookie carries the tag — matches onnet/genuine)
	//   Second call (tag in query or cookie):
	//     - render success template (jop_2_host2.htm) with {{HOSTKEY}} substituted
	//       so retail's IB3 parser extracts <TITLE>[HOSTKEY=...&]</TITLE>
	//       and uses it for subsequent UDP ClientHostRequest.
	auto handle_host = [this, templates_dir](const crow::request &req) {
		auto looks_encrypted = [](const std::string &v) {
			if (v.size() < 16) return false;
			for (char c : v) if (c < 'A' || c > 'P') return false;
			return true;
		};
		auto field_or = [&](const char *name, const std::string &fallback) -> std::string {
			const char *v = req.url_params.get(name);
			if (!v || !*v) return fallback;
			std::string s(v);
			return looks_encrypted(s) ? fallback : s;
		};

		// Resolve tag — prefer ?tag= query, fall back to NWJOINSESSIONTAG cookie.
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		std::string tag;
		if (req.url_params.get("tag")) {
			tag = req.url_params.get("tag");
		} else {
			auto it = cookies.find("NWJOINSESSIONTAG");
			if (it != cookies.end()) tag = it->second;
		}

		// The host page link's query is the first call even with a stale tag
		// cookie in the client's jar (net/novaworld/relay_request.h).
		const bool host_first_call =
				classify_relay_request(req.url_params.get("tag") != nullptr,
				                       has_relay_first_call_query(req),
				                       !tag.empty()) == RelayLeg::First;
		if (host_first_call) {
			// First call — generate session.
			HostSession s;
			s.session_tag = sessions_.generate_tag("NWHost.dll");
			// 48-char A-P encoded host_key from 24 OS CSPRNG bytes (base/os_random).
			std::array<uint8_t, 24> raw{};
			os_random_bytes(raw.data(), raw.size());
			std::string hk;
			hk.reserve(48);
			for (const uint8_t b : raw) {
				hk.push_back(static_cast<char>('A' + ((b >> 4) & 0x0F)));
				hk.push_back(static_cast<char>('A' + (b & 0x0F)));
			}
			s.host_key   = std::move(hk);
			// The host's game (its gate tag, else the page link's pfid and
			// success page): the default pages and the NWPF are its.
			const std::string pfid_param = field_or("pfid", std::string());
			const std::string success_param = field_or("success", std::string());
			const catalog::GameRow &game =
					client_game(impl_->games, req, cookies, pfid_param, success_param);
			s.success    = success_param.empty() ? game.page("host2.htm") : success_param;
			const std::string relay_tpl = field_or("relay", game.page("relay.htm"));
			s.pfid       = pfid_param.empty() ? game.pfid : pfid_param;

			// The listing's owner (service policy, unwitnessed in retail): the
			// account this request's own retail login names gets the key, kept as
			// its hash (host_keys), so the host that presents it on its NovaWorld
			// session lists its server as that account's. With no such login
			// nothing is recorded, and that host lists anonymously (or not at all
			// under ONNET_REQUIRE_HOST_KEY). Either way the pages are the same bytes.
			std::string owner_label = "none";
			try {
				auto db_conn = db_pool_.acquire();
				const char *how = "";
				const auto owner = retail_login_user(cookies, *db_conn, how);
				// Recorded only while the account is still active and at the
				// generation the login read, tested by the insert: a reset that
				// commits after the lookup leaves the key unrecorded.
				if (owner && owner->account_status == "active" &&
						hostdb::record_host_key(
								*db_conn, s.host_key, owner->id, owner->credentials_gen)) {
					owner_label = std::to_string(owner->id) + " (" + how + ")";
				}
			} catch (const std::exception &e) {
				owner_label = "none (db error)";
				std::fprintf(stderr, "[http] /NWHost.dll WARN host key record: %s\n", e.what());
			}

			const std::string new_tag    = s.session_tag;
			const std::string key_head   = s.host_key.substr(0, 6);
			sessions_.put_host(new_tag, std::move(s));

			std::printf("[http] /NWHost.dll (first call) -> tag %s host_key=%s... owner=%s\n",
					loggable_secret(new_tag).c_str(), key_head.c_str(), owner_label.c_str());

			TemplateVars vars{
				{"MESSAGE",          "Contacting NovaWorld...."},
				{"REFRESH_ENDPOINT", "NWHost.dll"},
				{"HOST_URL",         host_url()},
				{"GSB_SERVER",       gsb_url(game)},
				{"JOINLAN_URL",      ""},
			};
			crow::response res(200);
			res.body = render_template_file(templates_dir, relay_tpl, vars);
			res.set_header("Content-Type", "text/html");
			add_standard_headers(res);
			add_cookie(res, "YOURIP",           req.remote_ip_address);
			add_cookie(res, "NWJOINSESSIONTAG", new_tag);
			return res;
		}

		// Second call — take the session (served once: a stock client follows
		// the relay page's refresh once and reads the key from that page's
		// TITLE, so the tag never serves the key again), render success with
		// HOSTKEY.
		const auto session = sessions_.take_host(tag);
		if (!session) {
			std::printf("[http] /NWHost.dll (second call) -> 400 (unknown tag %s)\n",
					loggable_secret(tag).c_str());
			crow::response res(400);
			res.body = "unknown NWJOINSESSIONTAG (host)";
			return res;
		}

		std::printf("[http] /NWHost.dll (second call) tag=%s -> %s with HOSTKEY=%.6s...\n",
				loggable_secret(tag).c_str(), loggable(session->success).c_str(),
				session->host_key.c_str());

		const catalog::GameRow &game =
				client_game(impl_->games, req, cookies, session->pfid, session->success);
		TemplateVars vars{
			{"HOSTKEY",     session->host_key},
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", ""},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, session->success, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",           req.remote_ip_address);
		add_cookie(res, "NWJOINSESSIONTAG", tag);
		// PUBcrypto-encoded fields stubbed empty for first pass.
		for (const char *name : {"PUBPCID","PUBNAMEINFO","PUBSQUADINFO",
		                          "NWPCID","NWNAMEINFO","NWSQUADINFO","NWTI","NWTV",
		                          "AFFPCID","AFFNAMEINFO","AFFSQUADINFO"}) {
			add_cookie(res, name, "");
		}
		add_cookie(res, "NWPF",  session->pfid);
		add_cookie(res, "NWPF2", "0");
		return res;
	};
	CROW_ROUTE(app, "/NWHost.dll").methods("GET"_method)(handle_host);
	CROW_ROUTE(app, "/nwhost.dll").methods("GET"_method)(handle_host);
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
		res.code = 200;
		res.set_header("Content-Type", "text/html");
		res.body = read_file_text(path);
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
					steady_clock::now().time_since_epoch()).count());
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
		// (template_engine.h): '/'-separated bare file names that stay under
		// their root, so no absolute path ("//etc/..."), backslash, dot segment
		// or symlink out of the root is served. Crow hands `subpath` over
		// undecoded, so a %2e%2e or %2f is a literal '%' the rule refuses.

		// /static/* — game-client assets (.tga login backgrounds, etc.)
		// served from apps/novaworld_server/static/.
		if (subpath.rfind("static/", 0) == 0) {
			if (const auto path = file_under_root(static_dir, subpath.substr(7))) {
				res.code = 200;
				res.set_header("Content-Type", content_type_for(*path));
				res.body = read_file_text(*path);
				return res;
			}
			record_http_404();
			res.code = 404;
			return res;
		}

		// *.htm / *.mnx / *.joi — game-client templates from
		// apps/novaworld_server/templates/. No {{VAR}} substitution since
		// these are loaded via direct GET (e.g. retail's GET /jop_2_main.mnx
		// after the relay refresh).
		const auto ext = std::filesystem::path(subpath).extension().string();
		if (ext == ".htm" || ext == ".mnx" || ext == ".joi") {
			if (const auto path = template_file(templates_dir, subpath)) {
				res.code = 200;
				res.set_header("Content-Type", content_type_for(*path));
				res.body = read_file_text(*path);
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
		res.code = 200;
		res.set_header("Content-Type", content_type_for(*path));
		res.body = read_file_text(*path);
		return res;
	});
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
