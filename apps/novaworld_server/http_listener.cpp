#include "http_listener.h"

#include "auth.h"
#include "catalog_repository.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#include "session_store.h"
#include "template_engine.h"

#include <net/napi/session.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/join_identity.h>
#include <net/novaworld/relay_request.h>
#include <net/novaworld/unknown_tracker.h>

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

// ---- cookie + form helpers (Phase E.1) ---------------------------------

std::map<std::string, std::string> parse_cookie_header(std::string_view header) {
	std::map<std::string, std::string> out;
	size_t pos = 0;
	while (pos < header.size()) {
		// Cookie pairs are separated by ';' OR ','. Retail's IB3 client uses
		// commas (RFC 2965 style); splitting on ';' alone swallows every pair
		// after the first into one value (e.g. NWHANDLE hidden inside the
		// NWJOINSESSIONTAG value), so the joiner can't be identified at
		// /NWJoin.dll -> empty PUBPCID -> "login information is absent (GDC024)".
		// werkzeug (onnet) splits on both — match it.
		while (pos < header.size() &&
		       (header[pos] == ';' || header[pos] == ',' || header[pos] == ' ')) ++pos;
		const auto eq = header.find('=', pos);
		if (eq == std::string_view::npos) break;
		const auto end = header.find_first_of(";,", eq + 1);
		const auto val_end = (end == std::string_view::npos) ? header.size() : end;
		std::string name(header.substr(pos, eq - pos));
		std::string value(header.substr(eq + 1, val_end - eq - 1));
		// Defensive: strip any leftover leading comma (onnet's lstrip(",")).
		while (!name.empty() && name.front() == ',') name.erase(0, 1);
		out.emplace(std::move(name), std::move(value));
		pos = val_end + 1;
	}
	return out;
}

// Crow stores request headers in a case-INSENSITIVE multimap and
// get_header_value() returns only the FIRST match. Retail's IB3 client sends
// each cookie as its OWN "Cookie:" header, so reading a single header silently
// drops every other cookie — at /NWJoin.dll that loses NWHANDLE, the joiner
// can't be identified, PUBPCID comes out empty and the client reports "login
// info invalid or expired" (host/login happen to work because the one cookie
// Crow returns is the session tag they need). werkzeug (onnet) merges every
// Cookie header into request.cookies; match it by concatenating them all here
// before parsing. [verified: 3 separate Cookie: headers -> Crow exposes 1]
std::string request_cookie_header(const crow::request &req) {
	std::string combined;
	const auto range = req.headers.equal_range("Cookie");
	for (auto it = range.first; it != range.second; ++it) {
		if (it->second.empty()) continue;
		if (!combined.empty()) combined += "; ";
		combined += it->second;
	}
	return combined;
}

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
// use add_header to append a Set-Cookie line. Multiple Set-Cookies are
// allowed (crow::response stores headers in a multimap).
void add_cookie(crow::response &res, const std::string &name, const std::string &value) {
	res.add_header("Set-Cookie", name + "=" + value + "; Path=/");
}

void add_standard_headers(crow::response &res) {
	res.set_header("Expires", "Sat, 01 Jan 2000 00:00:00 GMT");
	res.set_header("Pragma", "no-cache");
	res.set_header("Cache-Control", "no-cache, must-revalidate");
}

std::string legacy_game_slug(const std::string &pfid,
                             const std::string &template_hint) {
	if (pfid == "38" || template_hint.rfind("dfx2_", 0) == 0) {
		return "dfx2_consumer";
	}
	return "jop_2_consumer";
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
// `name=value` with the value truncated to ~20 chars. Tags showing up
// here that we didn't issue this run = retail bridging state from a
// prior process via its persistent cookie jar (PERSISTENTEXPRESSLOGINDATA
// is the usual suspect — see G.4).
std::string cookie_summary(std::string_view header) {
	std::string out;
	const auto cookies = parse_cookie_header(header);
	bool first = true;
	for (const auto &[k, v] : cookies) {
		if (!first) out += ',';
		out += k;
		out += '=';
		if (v.size() <= 20) {
			out += v;
		} else {
			out += v.substr(0, 20);
			out += "..";
		}
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

// Serialize a user row to JSON (no password_hash). Shared by the
// public /api/register response and the admin user CRUD routes.
crow::json::wvalue user_to_json(const UserRecord &u) {
	crow::json::wvalue e;
	e["id"]       = u.id;
	e["username"] = u.username;
	e["pcid"]     = u.pcid;
	e["nwh"]      = u.nwh;
	e["nwhandle"] = u.nwhandle;
	e["account_status"] = u.account_status;
	return e;
}

} // namespace

struct HttpListener::Impl {
	crow::SimpleApp app;
	// start()'s handshake with the Crow thread: Pending until Crow serves
	// (Serving, with the port it bound) or until run() returns (Exited).
	enum class Run { Pending, Serving, Exited };
	std::mutex run_mu;
	std::condition_variable run_cv;
	Run run = Run::Pending;
	uint16_t port = 0;
	// How often Crow runs the tick whose first call tells start() it serves.
	static constexpr std::chrono::milliseconds kServingTick{50};
};

HttpListener::HttpListener(ConnectionManager &manager, db::ConnectionPool &db_pool,
                           SessionStore &sessions)
	: impl_(std::make_unique<Impl>()), manager_(manager), db_pool_(db_pool),
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
	const std::string admin_token   = config.admin_api_token;
	const std::string public_host   = config.public_host;
	std::printf("[http] admin api %s\n",
	            admin_token.empty() ? "DISABLED (set ADMIN_API_TOKEN to enable)"
	                                : "ENABLED");

	public_host_ = public_host; // host_url() / gsb_url(), with the port Crow binds

	register_admin_api_routes(admin_token, public_host);
	register_public_api_routes();
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
	app.port(config.http_port).multithreaded();
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
std::string HttpListener::host_url() const {
	return "http://" + public_host_ + ":" + std::to_string(impl_->app.port()) + "/nwhost.dll";
}

std::string HttpListener::gsb_url() const {
	return "http://" + public_host_ + ":" + std::to_string(impl_->app.port()) + "/jop_2.gsb";
}

// Admin REST API (Bearer ADMIN_API_TOKEN): server status, dev host
// injection, connection dump, user CRUD.
void HttpListener::register_admin_api_routes(const std::string &admin_token,
                                             const std::string &public_host) {
	auto &app = impl_->app;

	// Constant-time string compare (timing-safe). Returns false on length
	// mismatch or any byte difference. Used by admin endpoints below.
	auto admin_authorized = [admin_token](const crow::request &req) {
		if (admin_token.empty()) return false;
		std::string auth = req.get_header_value("Authorization");
		// Expect "Bearer <token>"
		if (auth.rfind("Bearer ", 0) != 0) return false;
		const std::string presented = auth.substr(7);
		if (presented.size() != admin_token.size()) return false;
		unsigned diff = 0;
		for (size_t i = 0; i < presented.size(); ++i) {
			diff |= static_cast<unsigned>(presented[i])
			      ^ static_cast<unsigned>(admin_token[i]);
		}
		return diff == 0;
	};

	CROW_ROUTE(app, "/api/admin/server-status").methods("GET"_method)(
	    [this, admin_authorized](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
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
	    [this, admin_authorized](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto body = crow::json::load(req.body);
		if (!body) {
			crow::response res(400);
			res.body = "{\"error\":\"invalid_json\"}";
			res.set_header("Content-Type", "application/json");
			return res;
		}
		const bool maintenance = body.has("maintenance_enabled")
			? static_cast<bool>(body["maintenance_enabled"].b())
			: false;
		const std::string message = body.has("message")
			? std::string(body["message"].s())
			: std::string();
		auto db_conn = db_pool_.acquire();
		auto result = update_server_status(*db_conn, maintenance, message);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"] = result.error_code;
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
	// joinable game without a live host process. Bearer-gated (CLOSED unless
	// ADMIN_API_TOKEN is set), so prod is unaffected. Defaults point the host at
	// this server's own NW UDP port (127.0.0.1:64206) so a joining OpenNova
	// client's JointOperations hello lands on our nw_udp_listener and routes to
	// the game-runtime PN path. The row is wiped on the next boot (clear_all)
	// and by the stale-host sweep; re-inject per run.
	CROW_ROUTE(app, "/api/admin/hosts").methods("POST"_method)(
	    [this, admin_authorized, public_host](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto body = crow::json::load(req.body);
		auto str_or = [&](const char *k, const char *fallback) {
			return (body && body.has(k)) ? std::string(body[k].s()) : std::string(fallback);
		};
		auto int_or = [&](const char *k, int fallback) {
			return (body && body.has(k)) ? static_cast<int>(body[k].i()) : fallback;
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

		try {
			auto db_conn = db_pool_.acquire();
			hostdb::upsert_host(*db_conn, row);
		} catch (const std::exception &e) {
			crow::response res(500);
			crow::json::wvalue out;
			out["error"] = "upsert_failed";
			out["message"] = e.what();
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		std::printf("[http] POST /api/admin/hosts injected rid=%u host=%s:%d game=%s\n",
		            static_cast<unsigned>(row.rid), row.host_ip.c_str(), row.host_port,
		            row.game.c_str());
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

	// The service's statements to a listed server, pushed over that server's own
	// NovaWorld UDP session (NwUdpListener::push_server_command / push_stop_hosting;
	// docs/net/novaworld-net-re.md "the service side"). 202 once queued for the
	// receive thread, 404 for a RID no live connection holds, 409 for one whose
	// connection is not hosting, 503 when no NW UDP listener is wired.
	auto json_reply = [](int code, const crow::json::wvalue &out) {
		crow::response res(code);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	};
	auto json_error = [json_reply](int code, const char *error, const std::string &message) {
		crow::json::wvalue out;
		out["error"] = error;
		if (!message.empty()) out["message"] = message;
		return json_reply(code, out);
	};
	auto push_reply = [json_reply, json_error](HostPushResult result, uint32_t rid,
	                                           crow::json::wvalue out) {
		switch (result) {
		case HostPushResult::Queued:
			out["rid"] = rid;
			out["status"] = host_push_result_name(result);
			return json_reply(202, out);
		case HostPushResult::NotHosted:
			return json_error(409, host_push_result_name(result),
			                  "the connection holding this RID is not hosting");
		case HostPushResult::UnknownRid:
			break;
		}
		return json_error(404, host_push_result_name(HostPushResult::UnknownRid),
		                  "no live NovaWorld connection holds this RID");
	};

	// POST /api/admin/hosts/<rid>/command — a ServerCommand. Body JSON:
	//   {"verb": "SetServerName", "target": "None|ByIndex|ByIpAndPort|ByName|ByPCID",
	//    "args": ["..."]}
	// composed through server_command_text, so a line the host's reader would
	// drop (a verb/target pairing, too few args, a quote or NUL, past 511
	// characters) is a 400 with its reason.
	CROW_ROUTE(app, "/api/admin/hosts/<uint>/command").methods("POST"_method)(
	    [this, admin_authorized, json_error, push_reply](const crow::request &req, uint64_t rid) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto body = crow::json::load(req.body);
		if (!body || body.t() != crow::json::type::Object) {
			return json_error(400, "invalid_json", "");
		}
		if (!body.has("verb") || body["verb"].t() != crow::json::type::String) {
			return json_error(400, "invalid_command", "\"verb\" (a string) is required");
		}
		ServerCommandVerb verb = ServerCommandVerb::None;
		if (!server_command_verb_from_name(std::string(body["verb"].s()), verb)) {
			return json_error(400, "invalid_command", "\"verb\" names no ServerCommand verb");
		}
		ServerCommandTarget target = ServerCommandTarget::None;
		if (body.has("target")) {
			if (body["target"].t() != crow::json::type::String ||
			    !server_command_target_from_name(std::string(body["target"].s()), target)) {
				return json_error(400, "invalid_command",
				                  "\"target\" is None, ByIndex, ByIpAndPort, ByName or ByPCID");
			}
		}
		std::vector<std::string> args;
		if (body.has("args")) {
			if (body["args"].t() != crow::json::type::List) {
				return json_error(400, "invalid_command", "\"args\" is a list of strings");
			}
			for (const auto &arg : body["args"]) {
				if (arg.t() != crow::json::type::String) {
					return json_error(400, "invalid_command", "\"args\" is a list of strings");
				}
				args.emplace_back(arg.s());
			}
		}
		const char *refusal = nullptr;
		const std::string cmd = server_command_text(verb, target, args, &refusal);
		if (cmd.empty()) {
			return json_error(400, "invalid_command", refusal != nullptr ? refusal : "");
		}
		if (nw_udp_ == nullptr) {
			return json_error(503, "no_session_listener", "the NovaWorld UDP listener is not wired");
		}
		const uint32_t rid32 = rid <= UINT32_MAX ? static_cast<uint32_t>(rid) : 0u;
		const HostPushResult result = nw_udp_->push_server_command(rid32, cmd);
		std::printf("[http] POST /api/admin/hosts/%llu/command '%s' -> %s\n",
		            static_cast<unsigned long long>(rid), cmd.c_str(),
		            host_push_result_name(result));
		crow::json::wvalue out;
		out["statement"] = "ServerCommand";
		out["cmd"] = cmd;
		return push_reply(result, rid32, std::move(out));
	});

	// POST /api/admin/hosts/<rid>/stop — a ServerStopHosting with MsgCode 7,
	// NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT (SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT;
	// which code the retail service sent is unwitnessed). Once it is sent the
	// server leaves the browser. No body.
	CROW_ROUTE(app, "/api/admin/hosts/<uint>/stop").methods("POST"_method)(
	    [this, admin_authorized, json_error, push_reply](const crow::request &req, uint64_t rid) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		if (nw_udp_ == nullptr) {
			return json_error(503, "no_session_listener", "the NovaWorld UDP listener is not wired");
		}
		const uint32_t rid32 = rid <= UINT32_MAX ? static_cast<uint32_t>(rid) : 0u;
		const HostPushResult result =
				nw_udp_->push_stop_hosting(rid32, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT);
		std::printf("[http] POST /api/admin/hosts/%llu/stop -> %s\n",
		            static_cast<unsigned long long>(rid), host_push_result_name(result));
		crow::json::wvalue out;
		out["statement"] = "ServerStopHosting";
		out["msg_code"] = SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT;
		return push_reply(result, rid32, std::move(out));
	});

	// Connection-registry debug dump. Admin-token gated.
	CROW_ROUTE(app, "/api/admin/connections")([this, admin_authorized](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
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

	// ----- Phase J: admin user CRUD ------------------------------------
	// GET /api/admin/users — list every player (no password_hash).
	CROW_ROUTE(app, "/api/admin/users").methods("GET"_method)(
	    [this, admin_authorized](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto db_conn = db_pool_.acquire();
		std::vector<crow::json::wvalue> arr;
		for (const auto &u : list_users(*db_conn)) arr.push_back(user_to_json(u));
		crow::json::wvalue out;
		out["users"] = std::move(arr);
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// POST /api/admin/users — create. Body JSON:
	//   {username, password, pcid, nwhandle, nwh? (default "1")}
	CROW_ROUTE(app, "/api/admin/users").methods("POST"_method)(
	    [this, admin_authorized](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto body = crow::json::load(req.body);
		if (!body) {
			crow::response res(400);
			res.body = "{\"error\":\"invalid_json\"}";
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto js = [&](const char *k, const char *fallback) {
			return body.has(k) ? std::string(body[k].s())
			                   : std::string(fallback);
		};
		CreateUserParams p;
		p.username = js("username", "");
		p.password = js("password", "");
		p.pcid     = js("pcid",     "");
		p.nwh      = js("nwh",      "1");
		p.nwhandle = js("nwhandle", "");
		auto db_conn = db_pool_.acquire();
		auto result = create_user(*db_conn, p);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"]   = result.error_code;
			out["message"] = result.error_message;
			crow::response res(result.error_code == "db_error" ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto created = get_user_by_id(*db_conn, result.id);
		out["user"] = created ? user_to_json(*created) : crow::json::wvalue{};
		crow::response res(201);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// DELETE /api/admin/users/<id>
	CROW_ROUTE(app, "/api/admin/users/<int>").methods("DELETE"_method)(
	    [this, admin_authorized](const crow::request &req, int id) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto db_conn = db_pool_.acquire();
		auto result = delete_user(*db_conn, id);
		crow::response res(result.ok ? 204 :
		                   result.error_code == "not_found" ? 404 : 500);
		if (!result.ok) {
			crow::json::wvalue err;
			err["error"]   = result.error_code;
			err["message"] = result.error_message;
			res.body = err.dump();
			res.set_header("Content-Type", "application/json");
		}
		return res;
	});

	// PUT /api/admin/users/<id> — partial update. Any field omitted is
	// left untouched. password (plaintext) triggers a fresh bcrypt hash.
	CROW_ROUTE(app, "/api/admin/users/<int>").methods("PUT"_method)(
	    [this, admin_authorized](const crow::request &req, int id) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto body = crow::json::load(req.body);
		if (!body) {
			crow::response res(400);
			res.body = "{\"error\":\"invalid_json\"}";
			res.set_header("Content-Type", "application/json");
			return res;
		}
		UpdateUserParams p;
		if (body.has("username")) p.username           = std::string(body["username"].s());
		if (body.has("password")) p.password_plaintext = std::string(body["password"].s());
		if (body.has("pcid"))     p.pcid               = std::string(body["pcid"].s());
		if (body.has("nwh"))      p.nwh                = std::string(body["nwh"].s());
		if (body.has("nwhandle")) p.nwhandle           = std::string(body["nwhandle"].s());
		if (body.has("account_status")) p.account_status = std::string(body["account_status"].s());
		auto db_conn = db_pool_.acquire();
		auto result = update_user(*db_conn, id, p);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"]   = result.error_code;
			out["message"] = result.error_message;
			crow::response res(
				result.error_code == "not_found" ? 404 :
				result.error_code == "db_error"  ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto updated = get_user_by_id(*db_conn, id);
		out["user"] = updated ? user_to_json(*updated) : crow::json::wvalue{};
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// PUT /api/admin/users/<id>/game-access — per-game expansion/access gate.
	CROW_ROUTE(app, "/api/admin/users/<int>/game-access").methods("PUT"_method)(
	    [this, admin_authorized](const crow::request &req, int id) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto body = crow::json::load(req.body);
		if (!body) {
			crow::response res(400);
			res.body = "{\"error\":\"invalid_json\"}";
			res.set_header("Content-Type", "application/json");
			return res;
		}
		UpdateGameAccessParams p;
		p.game_slug = body.has("game_slug") ? std::string(body["game_slug"].s()) : "";
		p.status    = body.has("status")    ? std::string(body["status"].s())    : "active";
		p.exp_bits  = body.has("exp_bits")  ? std::string(body["exp_bits"].s())  : "";
		auto db_conn = db_pool_.acquire();
		auto result = update_game_access(*db_conn, id, p);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"] = result.error_code;
			out["message"] = result.error_message;
			crow::response res(result.error_code == "not_found" ? 404 :
			                   result.error_code == "db_error"  ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto access = get_game_access(*db_conn, id, p.game_slug);
		if (access) {
			out["game_slug"] = access->game_slug;
			out["status"] = access->status;
			out["exp_bits"] = access->exp_bits;
		}
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});
}

// Public JSON API consumed by the web portal: lobbies, stats, self-signup,
// games, hosts, health, unknowns.
void HttpListener::register_public_api_routes() {
	auto &app = impl_->app;

	CROW_ROUTE(app, "/api/lobbies")([this]() {
		// Phase I.4: game-centric format mirroring onnet's api.py:21-49.
		// {"games":[{"slug","displayName","hosts":[...]}]}
		// Vue lobby browser keys off this exact shape.
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
					if (h.game != g.slug) continue;
					// camelCase to match the web LobbyHost type (snake_case here rendered
					// every host as "Unnamed Server 0/0" — the Vue card reads
					// serverName/maxPlayers). hostIp/hostPort surface the join
					// address so a host is identifiable, not just named.
					crow::json::wvalue hj;
					hj["id"]          = h.rid;
					hj["serverName"]  = h.server_name;
					hj["hostIp"]      = h.host_ip;
					hj["hostPort"]    = h.host_port;
					hj["players"]     = h.player_count;
					hj["maxPlayers"]  = h.max_players;
					hj["region"]      = h.region;
					hosts.push_back(std::move(hj));
				}
				game["hosts"] = std::move(hosts);
				games_json.push_back(std::move(game));
			}
		} catch (const db::SqliteError &e) {
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
		} catch (const db::SqliteError &e) {
			out["error"] = e.what();
		}
		return out;
	});

	// POST /api/register — public self-signup. No admin token needed.
	// Server auto-generates a unique 8-hex-char PCID. nwhandle defaults
	// to username when omitted. Returns the new user's public record on
	// success; 400 on conflict / missing fields, 500 on DB error.
	CROW_ROUTE(app, "/api/register").methods("POST"_method)(
	    [this](const crow::request &req) {
		auto body = crow::json::load(req.body);
		if (!body) {
			crow::response res(400);
			res.body = "{\"error\":\"invalid_json\"}";
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto js = [&](const char *k, const char *fallback) {
			return body.has(k) ? std::string(body[k].s())
			                   : std::string(fallback);
		};
		const std::string username = js("username", "");
		const std::string password = js("password", "");
		const std::string nwhandle = js("nwhandle", username.c_str());
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
			out["message"] = result.error_message;
			crow::response res(result.error_code == "db_error" ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto created = get_user_by_id(*db_conn, result.id);
		out["user"] = created ? user_to_json(*created) : crow::json::wvalue{};
		std::printf("[http] /api/register -> created user '%s' (id=%lld)\n",
		            username.c_str(), static_cast<long long>(result.id));
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
				e["ver1"]        = g.ver1;
				e["ver2"]        = g.ver2;
				arr.push_back(std::move(e));
			}
			out["games"] = std::move(arr);
		} catch (const db::SqliteError &e) {
			out["error"] = e.what();
		}
		return out;
	});

	CROW_ROUTE(app, "/api/hosts")([this]() {
		// Phase I.2/I.3: backed by active_hosts + host_players.
		std::vector<crow::json::wvalue> entries;
		try {
			auto db_conn = db_pool_.acquire();
			// One snapshot for the rows and every roster and player list read
			// below, so no host goes out beside a roster from another commit.
			db::ReadSnapshot snapshot(*db_conn);
			auto rows = hostdb::list_hosts(*db_conn);
			entries.reserve(rows.size());
			for (const auto &h : rows) {
				crow::json::wvalue e;
				e["rid"]          = h.rid;
				e["gsid"]         = h.gsid;
				e["game"]         = h.game;
				e["app_id"]       = h.app_id;
				e["server_name"]  = h.server_name;
				e["host_ip"]      = h.host_ip;
				e["host_port"]    = h.host_port;
				e["players"]      = h.player_count;
				e["max_players"]  = h.max_players;
				e["region"]       = h.region;
				e["game_type"]    = h.game_type;
				e["mission_name"] = h.mission_name;
				e["country"]      = h.country;
				e["exp"]          = h.exp;
				e["exp_bits"]     = h.exp_bits;
				e["ver1"]         = h.ver1;
				e["time_left"]    = h.time_left;
				e["time_of_day"]  = h.time_of_day;
				e["msg"]          = h.msg;
				e["mod"]          = h.mod;
				e["age"]          = h.age;
				e["pb_server"]    = h.pb_server;
				// The host-reported PlayerList (one entry per slot).
				std::vector<crow::json::wvalue> roster_entries;
				try {
					auto roster = hostdb::list_roster(*db_conn, h.rid);
					roster_entries.reserve(roster.size());
					for (const auto &s : roster) {
						crow::json::wvalue re;
						re["slot"]        = s.slot;
						re["player_name"] = s.player_name;
						re["team"]        = s.team;
						re["type"]        = s.type;
						roster_entries.push_back(std::move(re));
					}
				} catch (const db::SqliteError &) { /* ignore */ }
				e["roster"] = std::move(roster_entries);
				std::vector<crow::json::wvalue> player_entries;
				try {
					auto players = hostdb::list_players(*db_conn, h.rid);
					player_entries.reserve(players.size());
					for (const auto &p : players) {
						crow::json::wvalue pe;
						pe["nwhandle"] = p.nwhandle;
						if (p.user_id) pe["user_id"] = static_cast<int64_t>(*p.user_id);
						pe["peer_ip"]  = p.peer_ip;
						player_entries.push_back(std::move(pe));
					}
				} catch (const db::SqliteError &) { /* ignore */ }
				e["player_list"] = std::move(player_entries);
				entries.push_back(std::move(e));
			}
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[http] /api/hosts list failed: %s\n", e.what());
		}
		crow::json::wvalue out;
		out["count"] = entries.size();
		out["hosts"] = std::move(entries);
		std::printf("[http] /api/hosts -> %zu host(s)\n", entries.size());
		return out;
	});

	CROW_ROUTE(app, "/api/health")([]() {
		crow::json::wvalue out;
		out["status"] = "ok";
		return out;
	});

	// Live unknown-message snapshot (in-memory, reflects THIS run — the
	// durable cross-run record is the unknown_messages table). Optional
	// ?channel= filter. Each entry carries the first captured sample as
	// lowercase hex so reverse-engineering can eyeball the bytes.
	CROW_ROUTE(app, "/api/unknowns")([this](const crow::request &req) {
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

}

// Retail NW*.dll login/session chain: prepare, start, the EPASK login
// POST + relay GET, logout, and the character/account template pages.
void HttpListener::register_legacy_login_routes(const std::string &templates_dir) {
	auto &app = impl_->app;

	// ----- Phase E.1: Legacy NW*.dll login chain ---------------------------
	// Routes registered for both lowercase (retail capture) and Title-case
	// spellings since Crow routes are case-sensitive. Behavior mirrors
	// onnet's onnw/controllers/nova_world/{prepare,login}.py.

	// GET /nwprepare.dll
	auto handle_prepare = [this, templates_dir](const crow::request &req) {
		const std::string url   = req.url_params.get("url")  ? req.url_params.get("url")  : "jop_2_start.htm";
		const std::string ver1  = req.url_params.get("ver1") ? req.url_params.get("ver1") : "";
		const std::string ver2  = req.url_params.get("ver2") ? req.url_params.get("ver2") : "";
		const std::string gt    = req.url_params.get("gt")   ? req.url_params.get("gt")   : "";
		const std::string cc    = req.url_params.get("cc")   ? req.url_params.get("cc")   : "";

		const std::string epask = opennova::epask_to_string(epask_params_);
		std::printf("[http] GET %s url=%s gt=%s ver=%s/%s cookies=[%s]\n",
		            req.url.c_str(), url.c_str(), gt.c_str(), ver1.c_str(), ver2.c_str(),
		            cookie_summary(request_cookie_header(req)).c_str());

		TemplateVars vars{
			{"HOST_URL",     host_url()},
			{"GSB_SERVER",   gsb_url()},
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
		const std::string in_p   = req.url_params.get("IN")      ? req.url_params.get("IN")      : "jop_2_main.htm";
		const std::string out_p  = req.url_params.get("OUT")     ? req.url_params.get("OUT")     : "jop_2_login.htm";
		const std::string msgbase = req.url_params.get("MSGBASE")? req.url_params.get("MSGBASE") : "jop_2_msg.htm";
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		const auto epask = cookies.count("EPASK") ? cookies.at("EPASK") : opennova::epask_to_string(epask_params_);

		std::printf("[http] GET %s IN=%s OUT=%s MSGBASE=%s cookies=[%s]\n",
		            req.url.c_str(), in_p.c_str(), out_p.c_str(), msgbase.c_str(),
		            cookie_summary(request_cookie_header(req)).c_str());

		// Hardcoded version-OK; render OUT (the login page).
		TemplateVars vars{
			{"IN",          in_p},
			{"OUT",         out_p},
			{"MSGBASE",     msgbase},
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url()},
			{"JOINLAN_URL", ""},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, out_p, vars);
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
			} catch (const db::SqliteError &e) {
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
				std::fprintf(stderr, "[http] WARN EPASK form field bad: %s\n", why.c_str());
			}
		}
		auto epask_decode = [&](const std::string &name) -> std::string {
			const auto v = pick(name.c_str());
			if (v.empty() || !epask_in) return v;
			std::string plain;
			std::string why;
			if (!opennova::epask_decrypt(v, *epask_in, plain, &why)) {
				std::fprintf(stderr, "[http] WARN EPASK decrypt(%s) failed: %s\n",
				             name.c_str(), why.c_str());
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
		const std::string pfid_hint      = field_or("pfid", "28");
		const std::string success_hint   = field_or("success", "jop_2_main.htm");
		const std::string game_slug      = legacy_game_slug(pfid_hint, success_hint);
		auto render_login_message = [&](const std::string &message) {
			const std::string fail_tpl = field_or("failure", std::string("jop_2_main.htm"));
			const std::string msg_tpl  = field_or("msgbase", std::string("jop_2_msg.htm"));
			return render_legacy_message(templates_dir, message, fail_tpl, msg_tpl,
			                             req.remote_ip_address, host_url(), gsb_url());
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
			user = authenticate_user(*db_conn, login_name, login_password);
			if (user) {
				resolution = "epask-auth";
			} else {
				// Bad credentials → render the failure template (jop_2_main.htm
				// per retail's form `failure` arg) with a "Invalid username or
				// password" message and bail before storing a session.
				std::fprintf(stderr, "[http] POST /NWLogin.dll auth failed for user '%s'\n",
				             login_name.c_str());
				return render_login_message("Invalid username or password");
			}
		}
		if (!user && !persist_cookie.empty()) {
			int64_t pinned_id = 0;
			{
				std::lock_guard<std::mutex> lk(persistent_user_mu_);
				auto pin_it = persistent_to_user_id_.find(persist_cookie);
				if (pin_it != persistent_to_user_id_.end()) pinned_id = pin_it->second;
			}
			if (pinned_id != 0) {
				user = get_user_by_id(*db_conn, pinned_id);
				if (user) resolution = "persist-pin";
			}
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
		std::string effective_exp_bits = (game_slug == "dfx2_consumer") ? "1" : "3";
		if (user->id != 0) {
			const std::string account_status =
				user->account_status.empty() ? std::string("active") : user->account_status;
			if (account_status == "banned") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld banned\n",
				            static_cast<long long>(user->id));
				return render_login_message("This NovaWorld account has been banned. NWEC11");
			}
			if (account_status != "active") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld status=%s\n",
				            static_cast<long long>(user->id), account_status.c_str());
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
				            access->status.c_str());
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
			// is INSERT OR REPLACE on the user_id PK, so the stale row is simply
			// overwritten by this fresh login.
		}
		s.user_id  = user->id;
		s.username = user->username;
		s.pcid     = user->pcid;
		s.nwh      = user->nwh;
		s.nwhandle = user->nwhandle;
		s.exp_bits = effective_exp_bits;
		if (!persist_cookie.empty() && user->id != 0) {
			std::lock_guard<std::mutex> lk(persistent_user_mu_);
			persistent_to_user_id_[persist_cookie] = user->id;
		}
		std::printf("[http]   user resolved (%s): id=%lld username=%s pcid=%s nwhandle=%s persist=%.16s%s\n",
		            resolution ? resolution : "?",
		            static_cast<long long>(s.user_id), s.username.c_str(),
		            s.pcid.c_str(), s.nwhandle.c_str(),
		            persist_cookie.empty() ? "(none)" : persist_cookie.c_str(),
		            persist_cookie.size() > 16 ? ".." : "");

		// The relay template renders this response only; nothing reads it back
		// from the stored session.
		const std::string relay_template = field_or("relay", "jop_2_relay.htm");
		s.msgbase      = field_or("msgbase",     "jop_2_msg.htm");
		s.success      = field_or("success",     "jop_2_main.htm");
		s.failure      = field_or("failure",     "jop_2_main.htm");

		// Capture everything we need from `s` BEFORE moving it into the
		// session map — a use-after-move here once returned empty strings →
		// empty response bodies → retail re-POST'd until GOODBYE.
		const std::string tag            = s.session_tag;
		const int64_t user_id_for_active = s.user_id;
		const std::string username_for_active = s.username;

		sessions_.put_login(tag, std::move(s));
		if (user_id_for_active != 0) {
			try {
				register_active_user_session(*db_conn, user_id_for_active, username_for_active, tag,
				                             persist_cookie, req.remote_ip_address,
				                             req.get_header_value("User-Agent"));
			} catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN active_user_sessions register failed: %s\n",
				             e.what());
			}
		}

		std::printf("[http] POST %s body=%zuB user=%s relay->%s success->%s -> tag %s\n",
		            req.url.c_str(), req.body.size(),
		            sessions_.get_login(tag).value().username.c_str(),
		            relay_template.c_str(),
		            sessions_.get_login(tag).value().success.c_str(),
		            tag.c_str());

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
			{"GSB_SERVER",       gsb_url()},
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
		std::string tag;
		if (req.url_params.get("tag")) {
			tag = req.url_params.get("tag");
		} else {
			const auto cookies = parse_cookie_header(request_cookie_header(req));
			auto tag_it = cookies.find("LOGINSESSIONTAG");
			if (tag_it != cookies.end()) tag = tag_it->second;
		}
		if (tag.empty()) {
			std::printf("[http] GET %s -> 400 (no tag in query or cookie)\n", req.url.c_str());
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
			            req.url.c_str(), tag.c_str());
			// Mint a fresh, UNAUTHENTICATED session so retail doesn't hit a
			// blank screen on a cross-restart tag — but it carries no user.
			// The POST /NWLogin.dll path is the only auth gate; a stale tag
			// must re-authenticate there rather than be silently re-bound to
			// an account here. (user_id stays 0 → no active session, and any
			// authenticated action downstream still requires a real login.)
			LoginSession fresh;
			fresh.session_tag = sessions_.generate_tag("NWLogin.dll");
			fresh.success = "jop_2_main.htm";
			fresh.failure = "jop_2_main.htm";
			fresh.msgbase = "jop_2_msg.htm";
			tag = fresh.session_tag;
			sessions_.put_login(tag, std::move(fresh));
			session = sessions_.get_login(tag);
		}

		std::printf("[http] GET %s tag=%s -> auth ok (user=%s pcid=%s)\n",
		            req.url.c_str(), tag.c_str(),
		            session->username.c_str(), session->pcid.c_str());
		if (session->user_id != 0) {
			try { touch_active_user_session(*db_pool_.acquire(), session->user_id); }
			catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN active_user_sessions touch failed: %s\n",
				             e.what());
			}
		}

		TemplateVars vars{
			{"IN",           session->success},
			{"OUT",          session->failure},
			{"MSGBASE",      session->msgbase},
			{"HOST_URL",     host_url()},
			{"GSB_SERVER",   gsb_url()},
			{"JOINLAN_URL",  ""},
		};
		const std::string success_template = session->success.empty() ? "jop_2_main.htm" : session->success;
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
			auto pin_it = persistent_to_user_id_.find(persist_it->second);
			if (pin_it != persistent_to_user_id_.end()) pinned_id = pin_it->second;
			persistent_to_user_id_.erase(persist_it->second);
			if (pinned_id != 0) {
				try { clear_active_user_session(*db_pool_.acquire(), pinned_id); }
				catch (const std::exception &e) {
					std::fprintf(stderr, "[http] WARN active_user_sessions clear failed: %s\n",
					             e.what());
				}
			}
		}

		const std::string success_tpl = req.url_params.get("success")
		                                  ? req.url_params.get("success")
		                                  : "jop_2_login.htm";
		std::printf("[http] /NWLogout.dll tag=%s persist_dropped=%d -> %s\n",
		            tag.c_str(),
		            persist_it != cookies.end() ? 1 : 0,
		            success_tpl.c_str());

		const std::filesystem::path tpl_path =
			std::filesystem::path(templates_dir) / success_tpl;
		TemplateVars vars{
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url()},
			{"JOINLAN_URL", ""},
		};
		crow::response res(200);
		if (std::filesystem::exists(tpl_path)) {
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
	// Identity resolution mirrors the auth path: NWHANDLE cookie → DB
	// lookup, fall back to PERSISTENTEXPRESSLOGINDATA pin.
	auto handle_generic = [this, templates_dir](const crow::request &req) {
		const std::string tpl = req.url_params.get("success")
		                          ? req.url_params.get("success")
		                          : "jop_2_main.htm";
		const auto tpl_path = std::filesystem::path(templates_dir) / tpl;
		const bool exists = std::filesystem::exists(tpl_path);

		// Resolve identity. Best-effort — if neither cookie resolves we
		// just render with empty identity vars (the template can still
		// be served for unauthenticated paths).
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		auto db_conn = db_pool_.acquire();
		std::optional<UserRecord> user;
		if (auto it = cookies.find("NWHANDLE"); it != cookies.end() && !it->second.empty()) {
			user = get_user_by_username(*db_conn, it->second);
		}
		if (!user) {
			if (auto it = cookies.find("PERSISTENTEXPRESSLOGINDATA"); it != cookies.end()) {
				int64_t pinned_id = 0;
				{
					std::lock_guard<std::mutex> lk(persistent_user_mu_);
					auto pin_it = persistent_to_user_id_.find(it->second);
					if (pin_it != persistent_to_user_id_.end()) pinned_id = pin_it->second;
				}
				if (pinned_id != 0) user = get_user_by_id(*db_conn, pinned_id);
			}
		}
		std::printf("[http] %s -> %s%s user=%s\n",
		            req.url.c_str(), tpl.c_str(),
		            exists ? "" : " (MISSING — 404)",
		            user ? user->username.c_str() : "(unknown)");
		if (!exists) {
			crow::response res(404);
			res.body = "template not found: " + tpl;
			res.set_header("Content-Type", "text/plain");
			return res;
		}
		TemplateVars vars{
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url()},
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
	auto handle_gsb = [this](const std::string &game_slug) {
		// Phase I.2: backed by active_hosts, filtered to the requested game
		// (onnet serves each *.gsb from a per-game query — without the filter
		// a DFX2 host would leak into the JO browser and vice versa).
		std::vector<opennova::GsbServerEntry> entries;
		try {
			auto db_conn = db_pool_.acquire();
			// One snapshot for the rows and their rosters: a row's player count
			// and the names in its tail come from the same commit.
			db::ReadSnapshot snapshot(*db_conn);
			auto rows = hostdb::list_hosts_by_game(*db_conn, game_slug);
			entries.reserve(rows.size());
			for (const auto &h : rows) {
				// Every FLDS column carries the host-reported value and the row
				// tail carries the host's roster names (hostdb::gsb_entry_from_host).
				entries.push_back(hostdb::gsb_entry_from_host(
						h, hostdb::list_roster(*db_conn, h.rid)));
			}
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[http] GSB list failed: %s\n", e.what());
		}
		const auto blob = opennova::gsb_build_response(entries);
		std::printf("[http] GET /*.gsb (%s) -> %zu hosts, %zu bytes\n",
		            game_slug.c_str(), entries.size(), blob.size());
		crow::response res(200);
		res.set_header("Content-Type", "application/octet-stream");
		res.body.assign(blob.begin(), blob.end());
		return res;
	};
	CROW_ROUTE(app, "/jop_2.gsb")([handle_gsb]() { return handle_gsb("jop_2_consumer"); });
	CROW_ROUTE(app, "/dfx2_0.gsb")([handle_gsb]() { return handle_gsb("dfx2_consumer"); });

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
			JoinSession s;
			s.session_tag = sessions_.generate_tag("NWJoin.dll");
			s.success     = req.url_params.get("success")    ? req.url_params.get("success")    : "jop_2_join.joi";
			s.failure     = req.url_params.get("failure")    ? req.url_params.get("failure")    : "jop_2_main.htm";
			const std::string relay_template =
					req.url_params.get("relay") ? req.url_params.get("relay") : "jop_2_relay.htm";
			s.msgbase     = req.url_params.get("msgbase")    ? req.url_params.get("msgbase")    : "jop_2_msg.htm";
			s.needexpkey  = req.url_params.get("needexpkey") ? req.url_params.get("needexpkey") : "";
			s.pfid        = req.url_params.get("pfid")       ? req.url_params.get("pfid")       : "";
			s.rid         = req.url_params.get("rid")        ? req.url_params.get("rid")        : "";
			const std::string tag = s.session_tag;
			sessions_.put_join(tag, std::move(s));

			std::printf("[http] /NWJoin.dll (first call) rid=%s success=%s -> tag %s cookies=[%s]\n",
			            req.url_params.get("rid") ? req.url_params.get("rid") : "(none)",
			            req.url_params.get("success") ? req.url_params.get("success") : "(default)",
			            tag.c_str(),
			            cookie_summary(request_cookie_header(req)).c_str());

			// Bare "NWJoin.dll" refresh — matches onnet and the genuine server;
			// the NWJOINSESSIONTAG cookie carries the tag (see the /NWLogin.dll
			// relay note above re: dropping the divergent `?tag=` workaround).
			TemplateVars vars{
				{"MESSAGE",          "Contacting game server...."},
				{"REFRESH_ENDPOINT", "NWJoin.dll"},
				{"HOST_URL",         host_url()},
				{"GSB_SERVER",       gsb_url()},
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
		// full inbound cookie set — NWHANDLE / PERSISTENTEXPRESSLOGINDATA decide
		// the joiner identity that PUBPCID is encoded from.
		std::printf("[http] /NWJoin.dll (second call) tag=%s (from %s) cookies=[%s]\n",
		            tag_from_request.c_str(),
		            req.url_params.get("tag") ? "query" : "cookie",
		            cookie_summary(request_cookie_header(req)).c_str());
		const auto session = sessions_.get_join(tag_from_request);
		if (!session) {
			std::printf("[http] /NWJoin.dll (second call) WARN unknown tag '%s'\n",
			            tag_from_request.c_str());
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
		            static_cast<unsigned>(rid_value),
		            host.host_ip.c_str(), host.host_port,
		            nk_token.c_str(), ck_token.c_str());

		TemplateVars vars{
			{"NK",          nk_token},
			{"CK",          ck_token},
			{"NI",          host.host_ip},
			{"NP",          std::to_string(host.host_port)},
			{"BK",          BK_VALUE},
			{"SERVER_NAME", host.server_name.empty() ? std::string("OpenNova Server") : host.server_name},
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url()},
			{"JOINLAN_URL", ""},
		};
		const std::string success_template = session->success.empty() ? "jop_2_join.joi" : session->success;
		crow::response res(200);
		res.body = render_template_file(templates_dir, success_template, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",           req.remote_ip_address);
		add_cookie(res, "NWJOINSESSIONTAG", tag_from_request);

		// PUBcrypto-encoded join cookies (Phase E.4). Retail host-side join
		// reads CD/<localaddr>{PCID,NAMEINFO,SQUADINFO}; SQUADINFO supplies
		// the remote player's display/short names before validation.
		// Joiner identity comes from the inbound NWHANDLE cookie which
		// retail set during their /NWLogin.dll completion. Look the user
		// up in `players` to get their PCID.
		// Resolve joiner identity. Try NWHANDLE cookie first; if IB3
		// dropped it (HTTP/1.0 unreliability), fall back to the
		// PERSISTENTEXPRESSLOGINDATA pin we set during /NWLogin.dll (G.8).
		std::string pub_pcid;
		std::string pub_nameinfo;
		std::string pub_squadinfo;
		const std::string host_pcid_key = host.pcid_key;
		std::string joiner_pcid;
		std::string joiner_nwhandle;
		int64_t     joiner_user_id = 0;
		std::string joiner_label;  // for logging
		{
			const auto nwhandle_it = cookies.find("NWHANDLE");
			const std::string joiner_handle = nwhandle_it == cookies.end()
			                                    ? std::string()
			                                    : nwhandle_it->second;
			if (!joiner_handle.empty()) {
				if (auto u = get_user_by_username(*db_conn, joiner_handle)) {
					joiner_user_id  = u->id;
					joiner_pcid     = u->pcid;
					joiner_nwhandle = u->nwhandle;
					joiner_label    = "nwhandle=" + joiner_handle;
				}
			}
			if (joiner_pcid.empty()) {
				const auto persist_it = cookies.find("PERSISTENTEXPRESSLOGINDATA");
				if (persist_it != cookies.end()) {
					int64_t pinned_id = 0;
					{
						std::lock_guard<std::mutex> lk(persistent_user_mu_);
						auto pin_it = persistent_to_user_id_.find(persist_it->second);
						if (pin_it != persistent_to_user_id_.end()) pinned_id = pin_it->second;
					}
					if (pinned_id != 0) {
						if (auto u = get_user_by_id(*db_conn, pinned_id)) {
							joiner_user_id  = u->id;
							joiner_pcid     = u->pcid;
							joiner_nwhandle = u->nwhandle;
							joiner_label    = "persist-pinned=" + u->nwhandle;
						}
					}
				}
			}
			if (joiner_user_id != 0) {
				const std::string host_game = host.game.empty()
					? legacy_game_slug(session->pfid, session->success)
					: host.game;
				const std::string required_exp_bits = host.exp_bits.empty()
					? (host_game == "dfx2_consumer" ? std::string("1") : std::string("3"))
					: host.exp_bits;
				const auto access = get_game_access(*db_conn, joiner_user_id, host_game);
				const bool access_ok = access && access->status == "active" &&
					expansion_bits_compatible(access->exp_bits, required_exp_bits);
				if (!access_ok) {
					std::printf("[http] /NWJoin.dll rejected: joiner=%lld game=%s owned_exp=%s required_exp=%s status=%s\n",
					            static_cast<long long>(joiner_user_id), host_game.c_str(),
					            access ? access->exp_bits.c_str() : "(none)",
					            required_exp_bits.c_str(),
					            access ? access->status.c_str() : "(none)");
					const std::string msg_tpl = session->msgbase.empty()
						? std::string("jop_2_msg.htm")
						: session->msgbase;
					const std::string fail_tpl = session->needexpkey.empty()
						? (session->failure.empty() ? std::string("jop_2_main.htm") : session->failure)
						: session->needexpkey;
					return render_legacy_message(templates_dir,
						"This NovaWorld account does not have the required expansion key.",
						fail_tpl, msg_tpl, req.remote_ip_address, host_url(), gsb_url());
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
						std::fprintf(stderr, "[http] /NWJoin.dll WARN no joiner display handle - PUBNAMEINFO/PUBSQUADINFO will be empty (joiner=%s cookies=[%s])\n",
						             joiner_label.c_str(),
						             cookie_summary(request_cookie_header(req)).c_str());
					}
					std::printf("[http] /NWJoin.dll PUB* encoded for joiner=%s pcid=%s nwhandle=%s host_key=%zuB name=%zuB squad=%zuB\n",
					            joiner_label.c_str(), joiner_pcid.c_str(),
					            joiner_nwhandle.c_str(), host_pcid_key.size(),
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
	//     - store HostSession with default templates (jop_2_host2.htm etc.)
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
		std::string tag;
		if (req.url_params.get("tag")) {
			tag = req.url_params.get("tag");
		} else {
			const auto cookies = parse_cookie_header(request_cookie_header(req));
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
			s.success    = field_or("success",    "jop_2_host2.htm");
			const std::string relay_tpl = field_or("relay", "jop_2_relay.htm");
			s.pfid       = field_or("pfid",       "28");

			const std::string new_tag    = s.session_tag;
			sessions_.put_host(new_tag, std::move(s));

			std::printf("[http] /NWHost.dll (first call) -> tag %s host_key=%.16s...\n",
			            new_tag.c_str(), sessions_.get_host(new_tag).value().host_key.c_str());

			TemplateVars vars{
				{"MESSAGE",          "Contacting NovaWorld...."},
				{"REFRESH_ENDPOINT", "NWHost.dll"},
				{"HOST_URL",         host_url()},
				{"GSB_SERVER",       gsb_url()},
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

		// Second call — look up session, render success with HOSTKEY.
		const auto session = sessions_.get_host(tag);
		if (!session) {
			std::printf("[http] /NWHost.dll (second call) -> 400 (unknown tag %s)\n",
			            tag.c_str());
			crow::response res(400);
			res.body = "unknown NWJOINSESSIONTAG (host)";
			return res;
		}

		std::printf("[http] /NWHost.dll (second call) tag=%s -> %s with HOSTKEY=%.16s...\n",
		            tag.c_str(), session->success.c_str(), session->host_key.c_str());

		TemplateVars vars{
			{"HOSTKEY",     session->host_key},
			{"HOST_URL",    host_url()},
			{"GSB_SERVER",  gsb_url()},
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

		// /static/* — game-client assets (.tga login backgrounds, etc.)
		// served from apps/novaworld_server/static/.
		if (subpath.rfind("static/", 0) == 0) {
			const auto path = std::filesystem::path(static_dir) / subpath.substr(7);
			if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
				res.code = 200;
				res.set_header("Content-Type", content_type_for(path));
				res.body = read_file_text(path);
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
			const auto path = std::filesystem::path(templates_dir) / subpath;
			if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
				res.code = 200;
				res.set_header("Content-Type", content_type_for(path));
				res.body = read_file_text(path);
				return res;
			}
		}

		// Fallback — Vue SPA assets from web/dist/.
		const auto path = web_dist / subpath;
		if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
			std::printf("[http] 404 /%s (no match in static/, templates/, web/dist/)\n",
			            subpath.c_str());
			record_http_404();
			res.code = 404;
			res.body = "not found: " + subpath;
			res.set_header("Content-Type", "text/plain");
			return res;
		}
		res.code = 200;
		res.set_header("Content-Type", content_type_for(path));
		res.body = read_file_text(path);
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
