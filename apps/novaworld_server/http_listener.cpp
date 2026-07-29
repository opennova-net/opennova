#include "http_listener.h"

#include "auth.h"
#include "catalog_repository.h"
#include "github_client.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#include "session_store.h"
#include "template_engine.h"

#include <novacrypto/pubcrypto.h>
#include <novaworld/connection/manager.h>
#include <novaworld/db/sqlite.h>
#include <novaworld/gsb.h>
#include <novaworld/host_repository.h>
#include <novaworld/join_identity.h>
#include <novaworld/unknown_tracker.h>

#include <crow.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <random>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace opennova::server {

namespace {

std::string read_file_text(const std::filesystem::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream os;
	os << in.rdbuf();
	return os.str();
}

// UTC "YYYY-MM-DD HH:MM:SS", matching SQLite's CURRENT_TIMESTAMP format so a
// server-generated published_at sorts/compares the same as DB-stamped rows.
// Mirrors onnet passing datetime.utcnow() (admin_internal.py:79).
std::string now_utc_timestamp() {
	std::time_t t = std::time(nullptr);
	std::tm tm_utc{};
#if defined(_WIN32)
	gmtime_s(&tm_utc, &t);
#else
	gmtime_r(&t, &tm_utc);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
	return std::string(buf);
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

// Synthesise a fake EPASK string. Retail's client only treats it as an
// opaque cookie value (it carries the e/n/key triple it'll use to encrypt
// the next form, which we then ignore — see Phase E.1 plan).
std::string fake_epask() {
	using namespace std::chrono;
	const auto t = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	char buf[64];
	std::snprintf(buf, sizeof(buf), "12345:250997:%lld%06d",
	              static_cast<long long>(t), static_cast<int>(t % 1000000));
	return buf;
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

crow::json::wvalue value_to_json(const opennova::db::Value &v) {
	if (std::holds_alternative<std::monostate>(v)) {
		return crow::json::wvalue();
	}
	if (auto *p = std::get_if<int64_t>(&v)) {
		return crow::json::wvalue(*p);
	}
	if (auto *p = std::get_if<double>(&v)) {
		return crow::json::wvalue(*p);
	}
	if (auto *p = std::get_if<std::string>(&v)) {
		return crow::json::wvalue(*p);
	}
	// BLOBs unsupported in JSON — encode as base64? for now, skip with empty.
	return crow::json::wvalue();
}

// Lowercase hex of a byte buffer (for /api/unknowns sample_hex). Empty in,
// empty out.
std::string bytes_to_hex(const std::vector<uint8_t> &bytes) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(bytes.size() * 2);
	for (uint8_t b : bytes) {
		out.push_back(kHex[(b >> 4) & 0xf]);
		out.push_back(kHex[b & 0xf]);
	}
	return out;
}

crow::json::wvalue rows_to_json(const std::vector<opennova::db::Row> &rows) {
	crow::json::wvalue out = crow::json::wvalue::list();
	for (size_t i = 0; i < rows.size(); ++i) {
		crow::json::wvalue obj;
		for (size_t c = 0; c < rows[i].columns.size(); ++c) {
			obj[rows[i].columns[c]] = value_to_json(rows[i].values[c]);
		}
		out[i] = std::move(obj);
	}
	return out;
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
};

HttpListener::HttpListener(ConnectionManager &manager, db::Database &db,
                           NwUdpListener &nw_udp, SessionStore &sessions)
	: impl_(std::make_unique<Impl>()), manager_(manager), db_(db),
	  nw_udp_(nw_udp), sessions_(sessions),
	  epask_params_(opennova::generate_epask()) {
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

	// Client-facing URLs injected into the menus. The retail client is REMOTE,
	// so HOST_URL/GSB_SERVER must advertise the public host:port (not 127.0.0.1,
	// which would point the client at its own machine — host registration + the
	// server browser would silently never reach us).
	const std::string http_base = "http://" + public_host + ":" +
	                              std::to_string(config.http_port);
	host_url_ = http_base + "/nwhost.dll";
	gsb_url_  = http_base + "/jop_2.gsb";
	std::printf("[http] HOST_URL=%s\n", host_url_.c_str());

	// Expansion-publish pipeline config (ported from onnet). Captured by the
	// /release + /admin/internal routes below.
	const std::string expansion_github_token  = config.expansion_github_token;
	const std::string expansion_publish_token = config.expansion_publish_token;
	// The slug->repo mapping is read per-release from the expansions table
	// (Terraform-managed catalogue), not from config.
	std::printf("[http] expansion github token %s, publish token %s\n",
	            expansion_github_token.empty()  ? "DISABLED" : "ENABLED",
	            expansion_publish_token.empty() ? "DISABLED" : "ENABLED");

	register_admin_api_routes(admin_token, public_host, expansion_github_token);
	register_publish_callback_routes(expansion_publish_token);
	register_public_api_routes(public_host);
	register_legacy_login_routes(templates_dir);
	register_legacy_host_join_routes(templates_dir);
	// The catch-all /<path> wildcard must register last: Crow rejects a
	// more-specific route registered after a wildcard with "handler
	// already exists", so the static family always closes registration.
	register_static_routes(web_dist, static_dir, templates_dir);

	const uint16_t port = config.http_port;
	worker_ = std::thread([this, port] {
		std::printf("[http] listening on :%u\n", static_cast<unsigned>(port));
		try {
			impl_->app.port(port).multithreaded().run();
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[http] crashed: %s\n", e.what());
		}
		running_.store(false);
		std::printf("[http] loop exiting\n");
	});
	running_.store(true);
	return true;
}

// Admin REST API (Bearer ADMIN_API_TOKEN): server status, dev host
// injection, connection dump, expansion catalogue/releases, user CRUD.
void HttpListener::register_admin_api_routes(const std::string &admin_token,
                                             const std::string &public_host,
                                             const std::string &expansion_github_token) {
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

	// Serialize a release row to JSON. Shared by GET /api/admin/releases and
	// the POST .../release response. camelCase keys, faithful to onnet's
	// admin.py:_serialize_release — the Vue admin UI (web/src/types/admin.ts
	// AdminRelease) reads these directly.
	auto release_to_json = [](const catalog::ReleaseRow &r) {
		crow::json::wvalue e;
		e["id"]           = r.id;
		e["slug"]         = r.slug;
		e["version"]      = r.version;
		e["status"]       = r.status;
		e["repoRef"]      = r.repo_ref;
		e["workflowUrl"]  = r.workflow_url;
		e["targetCommit"] = r.target_commit;
		e["createdAt"]    = r.created_at;
		e["updatedAt"]    = r.updated_at;
		if (r.notes)         e["notes"]        = *r.notes;
		if (r.published_at)  e["publishedAt"]  = *r.published_at;
		if (r.error_message) e["errorMessage"] = *r.error_message;
		return e;
	};

	CROW_ROUTE(app, "/api/admin/server-status").methods("GET"_method)(
	    [this, admin_authorized](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		auto status = get_server_status(db_);
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
		auto result = update_server_status(db_, maintenance, message);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"] = result.error_code;
			out["message"] = result.error_message;
			crow::response res(500);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto status = get_server_status(db_);
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
			hostdb::upsert_host(db_, row);
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

	// Admin: list every expansion + its files. Mirrors onnet's
	// admin.py:177-180 — unwrapped DB row dump.
	CROW_ROUTE(app, "/api/admin/expansions")([this, admin_authorized](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		crow::json::wvalue out;
		try {
			std::vector<crow::json::wvalue> arr;
			for (const auto &x : catalog::list_expansions(db_)) {
				// camelCase admin shape, faithful to onnet admin.py:
				// _serialize_expansion. The Vue Available Expansions table
				// (web/src/types/admin.ts AdminExpansion) reads install.subdir
				// + files[]; keep distinct from the public card's install.target.
				crow::json::wvalue e;
				e["id"]          = x.id;
				e["slug"]        = x.slug;
				e["displayName"] = x.display_name;
				e["summary"]     = x.summary;
				e["version"]     = x.version;
				e["packageType"] = x.package_type;
				e["featured"]    = x.featured;
				e["gameSlug"]    = x.game_slug;
				e["githubRepo"]  = x.github_repo;
				crow::json::wvalue install;
				install["subdir"] = x.install_subdir;
				e["install"] = std::move(install);
				std::vector<crow::json::wvalue> files;
				for (const auto &f : catalog::list_expansion_files(db_, x.id)) {
					crow::json::wvalue fj;
					fj["downloadUrl"] = f.download_url;
					fj["sha256"]      = f.sha256;
					if (f.size_bytes) fj["sizeBytes"] = *f.size_bytes;
					fj["fileType"]    = f.file_type;
					fj["orderIndex"]  = f.order_index;
					files.push_back(std::move(fj));
				}
				e["files"] = std::move(files);
				arr.push_back(std::move(e));
			}
			out["expansions"] = std::move(arr);
		} catch (const db::SqliteError &e) {
			out["error"] = e.what();
		}
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// Admin: list recent expansion-release rows. Mirrors admin.py:183-187.
	CROW_ROUTE(app, "/api/admin/releases")([this, admin_authorized, release_to_json](const crow::request &req) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}
		int limit = 20;
		if (auto v = req.url_params.get("limit")) {
			try { limit = std::stoi(v); } catch (...) {}
		}
		crow::json::wvalue out;
		try {
			std::vector<crow::json::wvalue> arr;
			for (const auto &r : catalog::list_recent_releases(db_, limit))
				arr.push_back(release_to_json(r));
			out["releases"] = std::move(arr);
		} catch (const db::SqliteError &e) {
			out["error"] = e.what();
		}
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// Admin: request an expansion release. Ports onnet admin.py:190-245.
	// Records (or resets) the release row, then pushes a git tag to the
	// mapped GitHub repo using EXPANSION_GITHUB_TOKEN. Status becomes
	// 'tagged' on success, 'failed' otherwise. Admin-token gated.
	CROW_ROUTE(app, "/api/admin/expansions/<string>/release").methods("POST"_method)(
	    [this, admin_authorized, release_to_json, expansion_github_token]
	    (const crow::request &req, const std::string &slug) {
		if (!admin_authorized(req)) {
			crow::response res(401);
			res.body = "unauthorized";
			res.set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
			return res;
		}

		auto fail = [](int code, const std::string &message) {
			crow::json::wvalue out;
			out["ok"]    = false;
			out["error"] = message;
			crow::response res(code);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		};

		auto body = crow::json::load(req.body);
		if (!body) return fail(400, "Expected JSON object payload");

		auto trim = [](std::string s) {
			size_t a = s.find_first_not_of(" \t\r\n");
			size_t b = s.find_last_not_of(" \t\r\n");
			return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
		};

		const std::string version = body.has("version")
			? trim(std::string(body["version"].s())) : std::string();
		if (version.empty()) return fail(400, "'version' is required");

		std::string repo_ref;
		if (body.has("repoRef"))       repo_ref = trim(std::string(body["repoRef"].s()));
		else if (body.has("repo_ref")) repo_ref = trim(std::string(body["repo_ref"].s()));
		if (repo_ref.empty()) repo_ref = slug + "-v" + version;

		std::optional<std::string> notes;
		if (body.has("notes")) notes = std::string(body["notes"].s());

		try {
			auto exp = catalog::find_expansion_by_slug(db_, slug);
			if (!exp.found) return fail(404, "Unknown expansion '" + slug + "'");

			catalog::set_expansion_version(db_, exp.id, version);
			catalog::create_or_reset_release(db_, slug, version, repo_ref, notes);

			// Push the git tag (onnet _create_git_tag). The owner/repo comes
			// from the expansion row (Terraform-managed catalogue), not a
			// hardcoded map. A missing token or unmapped repo short-circuits
			// to failure with onnet's message.
			github::TagResult tag;
			if (expansion_github_token.empty() || exp.github_repo.empty()) {
				tag.success = false;
				tag.message = "GitHub token or repository mapping missing";
			} else {
				tag = github::create_git_tag(exp.github_repo, repo_ref,
				                             expansion_github_token);
			}

			const std::string status = tag.success ? "tagged" : "failed";
			std::optional<std::string> error_message;
			if (!tag.success) error_message = tag.message;
			catalog::update_release_status(db_, slug, version, status,
			                               error_message,
			                               /*workflow_url*/ std::nullopt,
			                               tag.target_commit,
			                               /*published_at*/ std::nullopt);

			auto rel = catalog::get_release(db_, slug, version);
			crow::json::wvalue out;
			out["ok"]      = tag.success;
			out["message"] = tag.success ? tag.message
			                             : ("Failed to create tag: " + tag.message);
			if (rel) out["release"] = release_to_json(*rel);

			crow::response res(tag.success ? 200 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		} catch (const db::SqliteError &e) {
			return fail(500, e.what());
		}
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
		std::vector<crow::json::wvalue> arr;
		for (const auto &u : list_users(db_)) arr.push_back(user_to_json(u));
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
		auto result = create_user(db_, p);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"]   = result.error_code;
			out["message"] = result.error_message;
			crow::response res(result.error_code == "db_error" ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto created = get_user_by_id(db_, result.id);
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
		auto result = delete_user(db_, id);
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
		auto result = update_user(db_, id, p);
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
		auto updated = get_user_by_id(db_, id);
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
		auto result = update_game_access(db_, id, p);
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
		auto access = get_game_access(db_, id, p.game_slug);
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

// CI publish callbacks (Bearer EXPANSION_PUBLISH_TOKEN): the expansion
// repo's build workflow reports publish success/failure here.
void HttpListener::register_publish_callback_routes(
		const std::string &expansion_publish_token) {
	auto &app = impl_->app;

	// Bearer-token gate for the /admin/internal/* publish callback. Distinct
	// from admin_authorized — it checks EXPANSION_PUBLISH_TOKEN, not the admin
	// token. Returns the HTTP status to send (0 == authorized), preserving
	// onnet's distinct codes (admin_internal.py:17-27): 500 token unset, 401
	// malformed/absent header, 403 mismatch.
	auto publish_authorized = [expansion_publish_token](const crow::request &req) -> int {
		if (expansion_publish_token.empty()) return 500;
		std::string auth = req.get_header_value("Authorization");
		if (auth.rfind("Bearer ", 0) != 0) return 401;
		const std::string presented = auth.substr(7);
		if (presented.size() != expansion_publish_token.size()) return 403;
		unsigned diff = 0;
		for (size_t i = 0; i < presented.size(); ++i) {
			diff |= static_cast<unsigned>(presented[i])
			      ^ static_cast<unsigned>(expansion_publish_token[i]);
		}
		return diff == 0 ? 0 : 403;
	};

	// Internal: CI publish callback. The expansion repo's build workflow
	// calls this after uploading the package to S3. Bearer-gated by
	// EXPANSION_PUBLISH_TOKEN. Ports onnet admin_internal.py:37-82.
	CROW_ROUTE(app, "/admin/internal/expansions/<string>/publish").methods("POST"_method)(
	    [this, publish_authorized](const crow::request &req, const std::string &slug) {
		if (int code = publish_authorized(req); code != 0) {
			crow::response res(code);
			res.body = (code == 500) ? "EXPANSION_PUBLISH_TOKEN is not configured"
			                         : "unauthorized";
			return res;
		}

		auto bad = [](const std::string &message) {
			crow::response res(400);
			res.body = message;
			return res;
		};

		auto body = crow::json::load(req.body);
		if (!body) return bad("Expected JSON object payload");

		auto trim = [](std::string s) {
			size_t a = s.find_first_not_of(" \t\r\n");
			size_t b = s.find_last_not_of(" \t\r\n");
			return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
		};

		const std::string version = body.has("version")
			? trim(std::string(body["version"].s())) : std::string();
		const std::string download_url = body.has("download_url")
			? trim(std::string(body["download_url"].s())) : std::string();
		const std::string sha256 = body.has("sha256")
			? trim(std::string(body["sha256"].s())) : std::string();
		if (version.empty() || download_url.empty() || sha256.empty())
			return bad("'version', 'download_url', and 'sha256' are required");

		std::optional<int64_t> size_bytes;
		if (body.has("size_bytes")) {
			// onnet rejects a non-integer size_bytes with 400.
			const auto &sb = body["size_bytes"];
			if (sb.t() == crow::json::type::Number)
				size_bytes = static_cast<int64_t>(sb.i());
			else
				return bad("'size_bytes' must be an integer");
		}
		std::optional<std::string> workflow_url;
		if (body.has("workflow_url")) workflow_url = std::string(body["workflow_url"].s());
		std::optional<std::string> target_commit;
		if (body.has("target_commit")) target_commit = std::string(body["target_commit"].s());

		try {
			auto exp = catalog::find_expansion_by_slug(db_, slug);
			if (!exp.found) {
				crow::response res(404);
				res.body = "Expansion '" + slug + "' not found";
				return res;
			}
			catalog::upsert_expansion_file(db_, exp.id, download_url, sha256, size_bytes);
			catalog::update_release_status(db_, slug, version, "published",
			                               /*error_message*/ std::nullopt,
			                               workflow_url, target_commit,
			                               now_utc_timestamp());
			crow::json::wvalue out;
			out["status"] = "ok";
			crow::response res(200);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		} catch (const db::SqliteError &e) {
			crow::response res(500);
			res.body = e.what();
			return res;
		}
	});

	// Internal: CI publish-failure callback. Ports onnet admin_internal.py:85-108.
	CROW_ROUTE(app, "/admin/internal/expansions/<string>/fail").methods("POST"_method)(
	    [this, publish_authorized](const crow::request &req, const std::string &slug) {
		if (int code = publish_authorized(req); code != 0) {
			crow::response res(code);
			res.body = (code == 500) ? "EXPANSION_PUBLISH_TOKEN is not configured"
			                         : "unauthorized";
			return res;
		}

		auto body = crow::json::load(req.body);
		if (!body) {
			crow::response res(400);
			res.body = "Expected JSON object payload";
			return res;
		}

		auto trim = [](std::string s) {
			size_t a = s.find_first_not_of(" \t\r\n");
			size_t b = s.find_last_not_of(" \t\r\n");
			return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
		};

		const std::string version = body.has("version")
			? trim(std::string(body["version"].s())) : std::string();
		if (version.empty()) {
			crow::response res(400);
			res.body = "'version' is required";
			return res;
		}

		std::string error_message = "Unknown error";
		if (body.has("error"))             error_message = std::string(body["error"].s());
		else if (body.has("error_message")) error_message = std::string(body["error_message"].s());
		std::optional<std::string> workflow_url;
		if (body.has("workflow_url")) workflow_url = std::string(body["workflow_url"].s());
		std::optional<std::string> target_commit;
		if (body.has("target_commit")) target_commit = std::string(body["target_commit"].s());

		try {
			catalog::update_release_status(db_, slug, version, "failed",
			                               error_message, workflow_url, target_commit,
			                               /*published_at*/ std::nullopt);
			crow::json::wvalue out;
			out["status"] = "recorded";
			crow::response res(200);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		} catch (const db::SqliteError &e) {
			crow::response res(500);
			res.body = e.what();
			return res;
		}
	});
}

// Public JSON API consumed by the web portal and the launcher: lobbies,
// stats, self-signup, games/expansions/hosts, health, unknowns,
// server-info.
void HttpListener::register_public_api_routes(const std::string &public_host) {
	auto &app = impl_->app;

	// Public expansion serialization (camelCase + files[]), faithful to onnet's
	// onnw/api.py. The launcher's Expansion Manager consumes files[].downloadUrl
	// to stage the package. Shared by GET /api/games (embedded per game) and
	// GET /api/expansions.
	auto expansion_card_json = [this](const catalog::ExpansionRow &x) {
		crow::json::wvalue e;
		e["slug"]        = x.slug;
		e["displayName"] = x.display_name;
		e["summary"]     = x.summary;
		e["version"]     = x.version;
		e["packageType"] = x.package_type;
		e["featured"]    = x.featured;
		crow::json::wvalue install;
		install["target"] = x.install_subdir;
		e["install"] = std::move(install);
		std::vector<crow::json::wvalue> files;
		for (const auto &f : catalog::list_expansion_files(db_, x.id)) {
			crow::json::wvalue fj;
			fj["downloadUrl"] = f.download_url;
			fj["sha256"]      = f.sha256;
			if (f.size_bytes) fj["sizeBytes"] = *f.size_bytes;
			fj["fileType"]    = f.file_type;
			files.push_back(std::move(fj));
		}
		e["files"] = std::move(files);
		return e;
	};

	CROW_ROUTE(app, "/api/lobbies")([this]() {
		// Phase I.4: game-centric format mirroring onnet's api.py:21-49.
		// {"games":[{"slug","displayName","hosts":[...]}]}
		// Vue lobby browser keys off this exact shape.
		std::vector<crow::json::wvalue> games_json;
		try {
			auto games     = catalog::list_games(db_);
			auto host_rows = hostdb::list_hosts(db_);
			games_json.reserve(games.size());
			for (const auto &g : games) {
				crow::json::wvalue game;
				game["slug"]        = g.slug;
				game["displayName"] = g.display_name;
				std::vector<crow::json::wvalue> hosts;
				for (const auto &h : host_rows) {
					if (h.game != g.slug) continue;
					// camelCase to match the web LobbyHost type + the
					// /api/expansions convention (snake_case here rendered
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
			auto agg = hostdb::aggregate(db_);
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
		// Try up to 5 random PCIDs to avoid the rare collision.
		static thread_local std::mt19937 gen{std::random_device{}()};
		std::uniform_int_distribution<uint32_t> dist;
		MutationResult result;
		for (int attempt = 0; attempt < 5; ++attempt) {
			char pcid[16];
			std::snprintf(pcid, sizeof(pcid), "%08x", dist(gen));
			CreateUserParams p;
			p.username = username;
			p.password = password;
			p.pcid     = pcid;
			p.nwh      = "1";
			p.nwhandle = nwhandle;
			result = create_user(db_, p);
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
		auto created = get_user_by_id(db_, result.id);
		out["user"] = created ? user_to_json(*created) : crow::json::wvalue{};
		std::printf("[http] /api/register -> created user '%s' (id=%lld)\n",
		            username.c_str(), static_cast<long long>(result.id));
		crow::response res(201);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	CROW_ROUTE(app, "/api/games")([this, expansion_card_json]() {
		crow::json::wvalue out;
		try {
			// One expansions read, grouped per game below — onnet's
			// api.py:list_games embeds each game's expansions (with files) so
			// the Expansions page can render them under their title.
			const auto exps = catalog::list_expansions(db_);
			std::vector<crow::json::wvalue> arr;
			for (const auto &g : catalog::list_games(db_)) {
				crow::json::wvalue e;
				// camelCase to match the web GameSummary type (the Expansions
				// page reads game.displayName; snake_case here left it undefined
				// and crashed the group sort on localeCompare). Mirrors the
				// /api/expansions + /api/lobbies casing.
				e["slug"]           = g.slug;
				e["displayName"]    = g.display_name;
				e["lobbyName"]      = g.lobby_name;
				e["gateTag"]        = g.gate_tag;
				e["executableName"] = g.executable_name;
				e["ver1"]           = g.ver1;
				e["ver2"]           = g.ver2;
				std::vector<crow::json::wvalue> game_exps;
				for (const auto &x : exps)
					if (x.game_slug == g.slug)
						game_exps.push_back(expansion_card_json(x));
				e["expansions"] = std::move(game_exps);
				arr.push_back(std::move(e));
			}
			out["games"] = std::move(arr);
		} catch (const db::SqliteError &e) {
			out["error"] = e.what();
		}
		return out;
	});

	CROW_ROUTE(app, "/api/expansions")([this, expansion_card_json]() {
		crow::json::wvalue out;
		try {
			std::vector<crow::json::wvalue> arr;
			for (const auto &x : catalog::list_expansions(db_)) {
				// Faithful to onnet api.py: full card + files[]; gameSlug drives
				// the web's standalone-vs-grouped split.
				crow::json::wvalue e = expansion_card_json(x);
				e["gameSlug"] = x.game_slug;
				arr.push_back(std::move(e));
			}
			out["expansions"] = std::move(arr);
		} catch (const db::SqliteError &e) {
			out["error"] = e.what();
		}
		return out;
	});

	CROW_ROUTE(app, "/api/hosts")([this]() {
		// Phase I.2/I.3: backed by active_hosts + host_players.
		std::vector<crow::json::wvalue> entries;
		try {
			auto rows = hostdb::list_hosts(db_);
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
				std::vector<crow::json::wvalue> player_entries;
				try {
					auto players = hostdb::list_players(db_, h.rid);
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
				e["sample_hex"]    = bytes_to_hex(s.sample);
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

	// Server-info for the launcher's ServerEndpointResolver: the public host
	// the client should redirect NovaWorld traffic to, plus the legacy
	// hostnames its hosts-file shim rewrites.
	CROW_ROUTE(app, "/api/server-info")([public_host]() {
		crow::json::wvalue out;
		out["novaworld_ip"] = public_host;
		std::vector<crow::json::wvalue> hostnames;
		hostnames.push_back(std::string("gs.novaworld.net"));
		out["redirect_hostnames"] = std::move(hostnames);
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
			{"HOST_URL",     host_url_},
			{"GSB_SERVER",   gsb_url_},
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
			{"HOST_URL",    host_url_},
			{"GSB_SERVER",  gsb_url_},
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
					evict_active_user_sessions_older_than(db_, 2 * 60 * 60);
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
			try { epask_in = opennova::epask_from_string(e_str); }
			catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN EPASK form field bad: %s\n", e.what());
			}
		}
		auto epask_decode = [&](const std::string &name) -> std::string {
			const auto v = pick(name.c_str());
			if (v.empty() || !epask_in) return v;
			try {
				return opennova::epask_decrypt(v, *epask_in);
			} catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN EPASK decrypt(%s) failed: %s\n",
				             name.c_str(), e.what());
				return std::string();
			}
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
			                             req.remote_ip_address, host_url_, gsb_url_);
		};

		const auto server_status = get_server_status(db_);
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
			user = authenticate_user(db_, login_name, login_password);
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
				user = get_user_by_id(db_, pinned_id);
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

			const auto access = get_game_access(db_, user->id, game_slug);
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

		s.relay        = field_or("relay",       "jop_2_relay.htm");
		s.msgbase      = field_or("msgbase",     "jop_2_msg.htm");
		s.success      = field_or("success",     "jop_2_main.htm");
		s.failure      = field_or("failure",     "jop_2_main.htm");
		s.pfid         = field_or("pfid",        "28");
		s.nodb         = field_or("nodb",        "jop_2_nodb.htm");
		s.needtoagree  = field_or("needtoagree", "jop_2_needtoagree.htm");
		s.enterkey     = field_or("enterkey",    "jop_2_key.htm");

		// Capture everything we need from `s` BEFORE moving it into the
		// session map — use-after-move on s.relay was returning empty
		// strings → empty response bodies → retail re-POST'd until GOODBYE.
		const std::string tag            = s.session_tag;
		const std::string relay_template = s.relay;
		const int64_t user_id_for_active = s.user_id;
		const std::string username_for_active = s.username;

		sessions_.put_login(tag, std::move(s));
		if (user_id_for_active != 0) {
			try {
				register_active_user_session(db_, user_id_for_active, username_for_active, tag,
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
			{"HOST_URL",         host_url_},
			{"GSB_SERVER",       gsb_url_},
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
			try { touch_active_user_session(db_, session->user_id); }
			catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN active_user_sessions touch failed: %s\n",
				             e.what());
			}
		}

		TemplateVars vars{
			{"IN",           session->success},
			{"OUT",          session->failure},
			{"MSGBASE",      session->msgbase},
			{"HOST_URL",     host_url_},
			{"GSB_SERVER",   gsb_url_},
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
			try { clear_active_user_session_by_tag(db_, tag); }
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
				try { clear_active_user_session(db_, pinned_id); }
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
			{"HOST_URL",    host_url_},
			{"GSB_SERVER",  gsb_url_},
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
		std::optional<UserRecord> user;
		if (auto it = cookies.find("NWHANDLE"); it != cookies.end() && !it->second.empty()) {
			user = get_user_by_username(db_, it->second);
		}
		if (!user) {
			if (auto it = cookies.find("PERSISTENTEXPRESSLOGINDATA"); it != cookies.end()) {
				int64_t pinned_id = 0;
				{
					std::lock_guard<std::mutex> lk(persistent_user_mu_);
					auto pin_it = persistent_to_user_id_.find(it->second);
					if (pin_it != persistent_to_user_id_.end()) pinned_id = pin_it->second;
				}
				if (pinned_id != 0) user = get_user_by_id(db_, pinned_id);
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
			{"HOST_URL",    host_url_},
			{"GSB_SERVER",  gsb_url_},
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
	// format from libs/novaworld/include/novaworld/gsb.h is byte-exact with
	// onnet's onnw/gsb.py, so retail's IB3 browser parser accepts it.
	auto handle_gsb = [this](const std::string &game_slug) {
		// Phase I.2: backed by active_hosts, filtered to the requested game
		// (onnet serves each *.gsb from a per-game query — without the filter
		// a DFX2 host would leak into the JO browser and vice versa).
		std::vector<opennova::GsbServerEntry> entries;
		try {
			auto rows = hostdb::list_hosts_by_game(db_, game_slug);
			entries.reserve(rows.size());
			for (const auto &h : rows) {
				opennova::GsbServerEntry e;
				e.rid = h.rid;    // host id — the GSB row's first u32 (the join rid)
				e.ip  = h.host_ip;  // row dword1: the ping-target IPv4 retail's
				// browser formats from entry+4 on the XXXX finalize
				// [orig: NapiGameList_StartPingSweep @ 0x63BCF0]. The joiner still
				// resolves the connect address from the NK token (/NWJoin.dll?rid=).
				e.server_name  = h.server_name.empty() ? std::string("Unnamed Server") : h.server_name;
				e.players      = h.player_count;
				e.max_players  = h.max_players;
				e.region       = h.region;
				e.game_type    = h.game_type.empty() ? std::string("COOP") : h.game_type;
				e.mission_name = h.mission_name;
				e.country      = h.country.empty() ? h.region : h.country;
				e.password     = h.password.empty() ? std::string("N") : h.password;
				e.locked       = h.locked.empty() ? std::string("N") : h.locked;
				e.dedicated    = h.dedicated.empty() ? std::string("Y") : h.dedicated;
				e.stat         = h.stat.empty() ? std::string("N") : h.stat;
				e.exp          = h.exp;
				e.exp_bits     = h.exp_bits.empty()
				                   ? (h.game == "dfx2_consumer" ? std::string("1") : std::string("3"))
				                   : h.exp_bits;
				e.ver1         = h.ver1.empty()
				                   ? (h.game == "dfx2_consumer" ? std::string("1") : std::string("3"))
				                   : h.ver1;
				e.joicon2      = h.joicon2.empty() ? std::string("4000") : h.joicon2;
				entries.push_back(std::move(e));
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

		// Treat as first call when there's no tag AND we got a rid (the
		// first /NWJoin.dll request from retail's host browser carries
		// rid=NNN). Without these, we'd loop the relay forever.
		const bool first_call = tag_from_request.empty();

		if (first_call) {
			JoinSession s;
			s.session_tag = sessions_.generate_tag("NWJoin.dll");
			s.success     = req.url_params.get("success")    ? req.url_params.get("success")    : "jop_2_join.joi";
			s.failure     = req.url_params.get("failure")    ? req.url_params.get("failure")    : "jop_2_main.htm";
			s.relay       = req.url_params.get("relay")      ? req.url_params.get("relay")      : "jop_2_relay.htm";
			s.msgbase     = req.url_params.get("msgbase")    ? req.url_params.get("msgbase")    : "jop_2_msg.htm";
			s.nodb        = req.url_params.get("nodb")       ? req.url_params.get("nodb")       : "";
			s.needexpkey  = req.url_params.get("needexpkey") ? req.url_params.get("needexpkey") : "";
			s.pfid        = req.url_params.get("pfid")       ? req.url_params.get("pfid")       : "";
			s.mode        = req.url_params.get("mode")       ? req.url_params.get("mode")       : "";
			s.rid         = req.url_params.get("rid")        ? req.url_params.get("rid")        : "";
			const std::string tag = s.session_tag;
			const std::string relay_template = s.relay;
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
				{"HOST_URL",         host_url_},
				{"GSB_SERVER",       gsb_url_},
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
		uint32_t rid_value = 0;
		try { rid_value = static_cast<uint32_t>(std::stoul(rid_str)); }
		catch (...) {
			crow::response res(400);
			res.body = "bad RID";
			return res;
		}

		// Look up the hosted entry from active_hosts (Phase I.2 — DB-backed).
		auto host_row = hostdb::find_host_by_rid(db_, rid_value);
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
			{"HOST_URL",    host_url_},
			{"GSB_SERVER",  gsb_url_},
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
				if (auto u = get_user_by_username(db_, joiner_handle)) {
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
						if (auto u = get_user_by_id(db_, pinned_id)) {
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
				const auto access = get_game_access(db_, joiner_user_id, host_game);
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
						fail_tpl, msg_tpl, req.remote_ip_address, host_url_, gsb_url_);
				}
			}
			if (host_pcid_key.empty()) {
				std::fprintf(stderr, "[http] /NWJoin.dll WARN host pcid_key missing - PUB* cookies will be empty (joiner cookies=[%s])\n",
				             cookie_summary(request_cookie_header(req)).c_str());
			} else if (joiner_pcid.empty()) {
				std::fprintf(stderr, "[http] /NWJoin.dll WARN no joiner identity - PUB* cookies will be empty (joiner cookies=[%s])\n",
				             cookie_summary(request_cookie_header(req)).c_str());
			} else {
				try {
					const auto payloads =
						opennova::build_pub_join_identity_plaintexts(joiner_pcid, joiner_nwhandle);
					pub_pcid = opennova::encode_pub_value(payloads.pcid, host_pcid_key);
					if (!payloads.name_info.empty()) {
						pub_nameinfo = opennova::encode_pub_value(payloads.name_info, host_pcid_key);
					}
					if (!payloads.squad_info.empty()) {
						pub_squadinfo = opennova::encode_pub_value(payloads.squad_info, host_pcid_key);
					}
					if (payloads.name_info.empty() || payloads.squad_info.empty()) {
						std::fprintf(stderr, "[http] /NWJoin.dll WARN no joiner display handle - PUBNAMEINFO/PUBSQUADINFO will be empty (joiner=%s cookies=[%s])\n",
						             joiner_label.c_str(),
						             cookie_summary(request_cookie_header(req)).c_str());
					}
					std::printf("[http] /NWJoin.dll PUB* encoded for joiner=%s pcid=%s nwhandle=%s host_key=%zuB name=%zuB squad=%zuB\n",
					            joiner_label.c_str(), joiner_pcid.c_str(),
					            joiner_nwhandle.c_str(), host_pcid_key.size(),
					            payloads.name_info.size(), payloads.squad_info.size());
				} catch (const std::exception &e) {
					std::fprintf(stderr, "[http] /NWJoin.dll PUB* encode failed: %s\n", e.what());
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
					hostdb::add_player(db_, p);
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

		if (tag.empty()) {
			// First call — generate session.
			HostSession s;
			s.session_tag = sessions_.generate_tag("NWHost.dll");
			// 48-char A-P encoded host_key from 24 random bytes.
			static thread_local std::mt19937 gen{std::random_device{}()};
			std::uniform_int_distribution<int> rb(0, 255);
			std::string hk;
			hk.reserve(48);
			for (int i = 0; i < 24; ++i) {
				const int b = rb(gen);
				hk.push_back(static_cast<char>('A' + ((b >> 4) & 0x0F)));
				hk.push_back(static_cast<char>('A' + (b & 0x0F)));
			}
			s.host_key   = std::move(hk);
			s.success    = field_or("success",    "jop_2_host2.htm");
			s.failure    = field_or("failure",    "jop_2_main.htm");
			s.relay      = field_or("relay",      "jop_2_relay.htm");
			s.msgbase    = field_or("msgbase",    "jop_2_msg.htm");
			s.nodb       = field_or("nodb",       "jop_2_nodb.htm");
			s.needexpkey = field_or("needexpkey", "jop_2_key2err.htm");
			s.pfid       = field_or("pfid",       "28");

			const std::string new_tag    = s.session_tag;
			const std::string relay_tpl  = s.relay;
			sessions_.put_host(new_tag, std::move(s));

			std::printf("[http] /NWHost.dll (first call) -> tag %s host_key=%.16s...\n",
			            new_tag.c_str(), sessions_.get_host(new_tag).value().host_key.c_str());

			TemplateVars vars{
				{"MESSAGE",          "Contacting NovaWorld...."},
				{"REFRESH_ENDPOINT", "NWHost.dll"},
				{"HOST_URL",         host_url_},
				{"GSB_SERVER",       gsb_url_},
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
			{"HOST_URL",    host_url_},
			{"GSB_SERVER",  gsb_url_},
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
	if (running_.load()) {
		impl_->app.stop();
	}
	if (worker_.joinable()) {
		worker_.join();
	}
	running_.store(false);
}

} // namespace opennova::server
