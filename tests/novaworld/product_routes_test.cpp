// The games' and the update releases' admin routes (apps/novaworld_web/products/product_routes.h,
// release_routes.h; ADR 0064), driven in-process: a database of the run's own
// (common/test_database.h, migrated with the games seed, so the seeded games are there), a Redis
// namespace of its own (common/test_redis.h), a local object store in a temp directory
// (core/local_object_store.h), the Crow HttpListener from opennova_novaworld_web_core on a port
// the OS picks, and requests through apps/common's http_exchange as the Bearer admin. The cases
// share the listener and run in order: every route refused without the admin token; the list's
// shapes over the seeded rows (every product, the apps among them, each with its key and a game's
// project's visibility and head); a game and an expansion created with every refusal (the
// shapes, a duplicate login, a missing, malformed or duplicate key, a missing visibility, a
// duplicate expansion, no game parent), patched (renamed, made public, an app given a project
// refused) and deleted (a game with an expansion refused has_expansion); an account's own game
// listed with its owner, given a lobby, never made retail, and deleted with its purge queued; a
// game's trees as
// release.apply leaves them (current, pinned by a live server and counted as outdated, the game
// then in use), no tree upload route left; an app's release created (a game's refused
// built_from_project, no app or version refused), its build uploaded in parts through the route
// and completed (building, a release.build job); a game's draft through the repository,
// published (one release.apply per section), retired, deleted with its package object (a
// published one refused, an abandoned upload aborted); the runtime's release published to the
// updater and built into no tree; a download-only app's release, no section, published to its
// own product alone.

#include "core/local_object_store.h"
#include "http_listener.h"
#include "jobs/job_queue.h"
#include "net_http.h"
#include "net_sockets.h"
#include "products/product_repository.h"
#include "products/product_routes.h"
#include "products/release_routes.h"
#include "program/server_config.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <net/novaworld/db/database.h>

#include <crow/json.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
namespace io = opennova::io;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

constexpr const char *kAdminToken = "product-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer product-routes-admin-token";
constexpr const char *kJson = "Content-Type: application/json";
constexpr const char *kOctets = "Content-Type: application/octet-stream";

struct Harness {
	ConnectionPool &pool;
	nws::LocalObjectStore &store;
	uint16_t port = 0;
	int64_t jo = 0; // the seeded Joint Operations and Escalation rows
	int64_t jox = 0;
	int64_t runtime = 0; // the seeded apps': the runtime, the editor, the tools
	int64_t editor = 0;
	int64_t tools = 0;

	net::HttpReply send(const char *method, const std::string &path,
			const std::vector<std::string> &headers = {}, const std::string &body = {}) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
		return net::http_exchange(
				method, url, headers, body, 30000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
	}
	// An admin's request with the Bearer token.
	net::HttpReply admin(
			const char *method, const std::string &path, const std::string &body = {}) const {
		return send(method, path, { kJson, kBearer }, body);
	}
};

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

std::string describe(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "no reply (" + reply.error + ")";
	return "HTTP " + std::to_string(reply.code) + " " + body_text(reply);
}

std::string str(const crow::json::rvalue &v) {
	return std::string(v.s());
}

bool is_null(const crow::json::rvalue &v) {
	return v.t() == crow::json::type::Null;
}

// The reply's JSON when it came with `code`; an empty value (and the reply logged)
// otherwise.
crow::json::rvalue json_of(const net::HttpReply &reply, int code) {
	if (!reply.transport_ok || reply.code != code) {
		std::fprintf(stderr, "  %s, want %d\n", describe(reply).c_str(), code);
		return {};
	}
	return crow::json::load(body_text(reply));
}

std::string error_of(const net::HttpReply &reply) {
	const auto json = crow::json::load(body_text(reply));
	return json && json.has("error") ? str(json["error"]) : std::string();
}

std::string field_of(const net::HttpReply &reply) {
	const auto json = crow::json::load(body_text(reply));
	return json && json.has("field") ? str(json["field"]) : std::string();
}

// Whether the reply came with `code` and the error `error` (logged otherwise).
bool refused(const net::HttpReply &reply, int code, const char *error) {
	if (reply.transport_ok && reply.code == code && error_of(reply) == error)
		return true;
	std::fprintf(stderr, "  %s, want %d %s\n", describe(reply).c_str(), code, error);
	return false;
}

// "YYYY-MM-DD HH:MM:SS".
bool is_db_time(const std::string &text) {
	if (text.size() != 19)
		return false;
	for (size_t i = 0; i < text.size(); ++i) {
		const char c = text[i];
		if (i == 4 || i == 7) {
			if (c != '-')
				return false;
		} else if (i == 10) {
			if (c != ' ')
				return false;
		} else if (i == 13 || i == 16) {
			if (c != ':')
				return false;
		} else if (!std::isdigit(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	return true;
}

// The queued job holding `dedupe_key`: its kind and payload text, or nullopt.
struct QueuedJob {
	std::string kind;
	std::string payload;
};
std::optional<QueuedJob> queued_job(Database &db, const std::string &dedupe_key) {
	const auto rows = db.query(
			"SELECT kind, payload::text FROM jobs WHERE dedupe_key = $1 AND state = 'queued';",
			{ BindValue(dedupe_key) });
	if (rows.empty())
		return std::nullopt;
	return QueuedJob{ rows.front().as_text(0).value_or(""), rows.front().as_text(1).value_or("") };
}

int64_t seeded(Database &db, const char *display_name) {
	const auto rows = db.query(
			"SELECT id FROM products WHERE display_name = $1;", { BindValue(display_name) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

// A hosted server row as a start writes it, pinned to `base_tree`.
int64_t insert_server(Database &db, int64_t product_id, int64_t base_tree) {
	const auto rows = db.query(
			"INSERT INTO hosted_servers (state, game_name, max_players, time_limit, kill_limit, "
			"max_score, respawn, start_delay, missions, product_id, base_tree_id) "
			"VALUES ('running', 'srv', 32, 30, 50, 5, 5, 0, 'a.bms', $1, $2) RETURNING id;",
			{ BindValue(product_id), BindValue(base_tree) });
	return rows.front().as_int(0).value_or(0);
}

// A ready tree of `product_id` with `version`, as release.apply leaves one: its id.
int64_t ready_tree(Database &db, int64_t product_id, const char *version) {
	const int64_t id = hosted::create_tree(db, product_id, std::nullopt, "worker");
	hosted::TreeReady ready;
	ready.files = { { "resource.pff", 35 }, { "localres.pff", 8 }, { "language.pff", 8 },
		{ "version.txt", 50 } };
	ready.missions = { { "a.bms", "Alpha" }, { "b.bms", "Bravo" } };
	ready.version_text = version;
	ready.bytes = 101;
	return hosted::finish_tree(db, id, ready) ? id : 0;
}

// The product `id` from the admin list, or an empty value.
crow::json::rvalue listed_product(const Harness &h, int64_t id) {
	const auto json = json_of(h.admin("GET", "/api/admin/products"), 200);
	if (!json)
		return {};
	// Read again into a value of its own: an element's copy still points into the listing's
	// text, which goes with `json`.
	for (const auto &d : json["products"])
		if (d["id"].i() == id)
			return crow::json::load(crow::json::wvalue(d).dump());
	return {};
}

// --- cases ---

int test_refused(Harness &h) {
	net::HttpReply reply = h.send("GET", "/api/admin/products");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("GET", "/api/admin/products", { "Authorization: Bearer wrong" });
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("POST", "/api/admin/products", { kJson }, "{}");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("PATCH", "/api/admin/products/1", { kJson }, "{}");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("GET", "/api/admin/releases", { "Authorization: Bearer wrong" });
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("POST", "/api/admin/releases", { kJson }, "{}");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("PUT", "/api/admin/releases/1/parts/1?upload_id=x", { kOctets }, "x");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	return 0;
}

// GET /api/admin/products over the seeded rows: the local store, the titles, the three
// lobbies, every product (games, expansions, then apps) with its key and a game's project's
// visibility and head, Joint Operations' row with no tree yet.
int test_list(Harness &h) {
	const auto json = json_of(h.admin("GET", "/api/admin/products"), 200);
	TEST_EXPECT(json);
	TEST_EXPECT(str(json["store"]["kind"]) == "local");
	TEST_EXPECT(json["titles"].size() == 2 && str(json["titles"][0]) == "jo" &&
			str(json["titles"][1]) == "dfx2");
	TEST_EXPECT(json["games"].size() == 3 && str(json["games"][2]["slug"]) == "jop_2_consumer" &&
			!str(json["games"][2]["display_name"]).empty());
	TEST_EXPECT(json["products"].size() == 10);
	TEST_EXPECT(str(json["products"][0]["kind"]) == "game" &&
			str(json["products"][3]["kind"]) == "expansion" &&
			str(json["products"][5]["kind"]) == "app" && str(json["products"][9]["kind"]) == "app");
	const auto jo = listed_product(h, h.jo);
	TEST_EXPECT(jo);
	TEST_EXPECT(str(jo["display_name"]) == "Joint Operations" && str(jo["title"]) == "jo" &&
			str(jo["lobby"]) == "jop_2_consumer" && str(jo["updater_login"]) == "JOTRus" &&
			str(jo["command_file"]) == "JOPTR.CMD" && jo["retail"].b() &&
			str(jo["wizard_login"]) == "JOTRqq" &&
			str(jo["download"]) == "jointops-game-windows.zip" && !str(jo["description"]).empty());
	TEST_EXPECT(str(jo["key"]) == "JO" && str(jo["visibility"]) == "private" &&
			jo["head"].i() == 0 && !jo.has("baseline_tree_id"));
	// The service's own: no owner.
	TEST_EXPECT(is_null(jo["owner"]) && is_null(jo["owner_id"]));
	TEST_EXPECT(is_null(jo["parent_id"]) && is_null(jo["current_tree"]) &&
			is_null(jo["published_release"]));
	TEST_EXPECT(jo["servers_live"].i() == 0 && jo["servers_outdated"].i() == 0 &&
			jo["trees"].size() == 0);
	const auto jox = listed_product(h, h.jox);
	TEST_EXPECT(jox && jox["parent_id"].i() == h.jo && str(jox["expansion_code"]) == "jox01" &&
			str(jox["key"]) == "JOX01");
	// The runtime: an app, keyed, with no project, its releases from CI.
	const auto runtime = listed_product(h, h.runtime);
	TEST_EXPECT(runtime && str(runtime["kind"]) == "app" &&
			str(runtime["updater_login"]) == "ONRUNTIME" && str(runtime["key"]) == "ONRUNTIME" &&
			is_null(runtime["visibility"]) && runtime["head"].i() == 0 &&
			!runtime["retail"].b() && str(runtime["download"]).empty());
	return 0;
}

// POST, PATCH and DELETE /api/admin/products: a base and its expansion, and the refusals.
int test_create_patch_delete(Harness &h) {
	// The shapes.
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/products", "nope"), 400, "invalid_json"));
	net::HttpReply reply = h.admin("POST", "/api/admin/products",
			R"({"kind":"game","title":"jo","lobby":"jop_2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "display_name");
	reply = h.admin("POST", "/api/admin/products", R"({"display_name":"X","kind":"other"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "kind");
	reply = h.admin("POST", "/api/admin/products", R"({"display_name":"X","kind":"app"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "kind");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"bhd","lobby":"jop_2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "title");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"no_such_lobby","key":"X","visibility":"public"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "lobby");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","expansion_code":"x"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "expansion_code");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":1,"kind":"game","title":"jo","lobby":"jop_2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "display_name");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","updater_login":"BHDus"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "command_file");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","updater_login":"BHD us","command_file":"BHDPTR.CMD"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "updater_login");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","updater_login":"BHDus","command_file":"BHDPTR.TXT"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "command_file");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","retail":"yes"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "retail");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"expansion","expansion_code":"bhx01"})");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "parent_id");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"expansion","parent_id":1,"expansion_code":"BHX 1"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "expansion_code");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"expansion","parent_id":1,"expansion_code":"bhx01","title":"jo"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "title");
	// Every product has a key, and a game its project's visibility, each of its shape.
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","key":"BHD"})");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "visibility");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","visibility":"public"})");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "key");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","key":"bhd","visibility":"public"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "key");
	reply = h.admin("POST", "/api/admin/products",
			R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","key":"BHD","visibility":"secret"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "visibility");
	// A duplicate login (without case) and a key another product has (an app's among them) are
	// the database's refusals.
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/products",
					R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","updater_login":"jotrus","command_file":"X.CMD","key":"X","visibility":"public"})"),
			409, "duplicate_login"));
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/products",
					R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","key":"JO","visibility":"public"})"),
			409, "duplicate_key"));
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/products",
					R"({"display_name":"X","kind":"game","title":"jo","lobby":"jop_2_consumer","key":"ONEDITOR","visibility":"public"})"),
			409, "duplicate_key"));

	// A base with its project.
	auto json = json_of(
			h.admin("POST", "/api/admin/products",
					R"({"display_name":"Black Hawk Down","kind":"game","title":"dfx2","lobby":"dfx2_consumer","updater_login":"BHDus","command_file":"BHDPTR.CMD","retail":true,"key":"BHD","visibility":"private"})"),
			201);
	TEST_EXPECT(json);
	const int64_t bhd = json["product"]["id"].i();
	TEST_EXPECT(bhd != 0 && str(json["product"]["display_name"]) == "Black Hawk Down" &&
			str(json["product"]["title"]) == "dfx2" &&
			str(json["product"]["updater_login"]) == "BHDus" &&
			json["product"]["retail"].b() && json["product"]["trees"].size() == 0 &&
			is_null(json["product"]["current_tree"]) && json["product"]["servers_live"].i() == 0);
	TEST_EXPECT(str(json["product"]["key"]) == "BHD" &&
			str(json["product"]["visibility"]) == "private" && json["product"]["head"].i() == 0);
	// Its expansion, once, with its project.
	const std::string expansion =
			R"({"display_name":"Team Sabre","kind":"expansion","parent_id":)" +
			std::to_string(bhd) + R"(,"expansion_code":"bhx01","key":"SABRE","visibility":"private"})";
	json = json_of(h.admin("POST", "/api/admin/products", expansion), 201);
	TEST_EXPECT(json);
	const int64_t ts = json["product"]["id"].i();
	TEST_EXPECT(ts != 0 && str(json["product"]["kind"]) == "expansion" &&
			json["product"]["parent_id"].i() == bhd &&
			str(json["product"]["expansion_code"]) == "bhx01" &&
			str(json["product"]["title"]).empty() &&
			str(json["product"]["updater_login"]).empty() && str(json["product"]["key"]) == "SABRE" &&
			str(json["product"]["visibility"]) == "private");
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/products",
								R"({"display_name":"Team Sabre","kind":"expansion","parent_id":)" +
										std::to_string(bhd) +
										R"(,"expansion_code":"bhx01","key":"SABRE2","visibility":"private"})"),
			409, "duplicate_expansion"));
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/products",
					R"({"display_name":"Orphan","kind":"expansion","parent_id":99999,"expansion_code":"bhx02","key":"ORPHAN","visibility":"private"})"),
			409, "no_such_parent"));
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/products",
								R"({"display_name":"Orphan","kind":"expansion","parent_id":)" +
										std::to_string(ts) +
										R"(,"expansion_code":"bhx02","key":"ORPHAN","visibility":"private"})"),
			409, "no_such_parent"));
	TEST_EXPECT(listed_product(h, bhd) && listed_product(h, ts));

	// PATCH.
	const std::string base = "/api/admin/products/" + std::to_string(bhd);
	json = json_of(h.admin("PATCH", base, R"({"display_name":"BHD"})"), 200);
	TEST_EXPECT(json && str(json["product"]["display_name"]) == "BHD");
	reply = h.admin("PATCH", base, R"({"display_name":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "display_name");
	// The updater product: a flag, a login another holds, a command file without its login,
	// both cleared.
	json = json_of(h.admin("PATCH", base, R"({"retail":false})"), 200);
	TEST_EXPECT(json && !json["product"]["retail"].b());
	TEST_EXPECT(refused(h.admin("PATCH", base, R"({"updater_login":"JOTRUS"})"), 409,
			"duplicate_login"));
	reply = h.admin("PATCH", base, R"({"command_file":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "command_file");
	json = json_of(h.admin("PATCH", base, R"({"updater_login":"","command_file":""})"), 200);
	TEST_EXPECT(json && str(json["product"]["updater_login"]).empty());
	reply = h.admin(
			"PATCH", "/api/admin/products/" + std::to_string(ts), R"({"lobby":"dfx2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "lobby");
	json = json_of(h.admin("PATCH", base, R"({"lobby":"acidgambit_0_consumer"})"), 200);
	TEST_EXPECT(json && str(json["product"]["lobby"]) == "acidgambit_0_consumer");
	TEST_EXPECT(refused(h.admin("PATCH", "/api/admin/products/99999", R"({"display_name":"X"})"),
			404, "not_found"));
	// Renamed and made public; a key in use, a malformed one, a visibility cleared, and an app
	// given a project, refused.
	json = json_of(h.admin("PATCH", base, R"({"key":"BHAWK","visibility":"public"})"), 200);
	TEST_EXPECT(json && str(json["product"]["key"]) == "BHAWK" &&
			str(json["product"]["visibility"]) == "public");
	TEST_EXPECT(refused(h.admin("PATCH", base, R"({"key":"JOX01"})"), 409, "duplicate_key"));
	reply = h.admin("PATCH", base, R"({"key":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "key");
	reply = h.admin("PATCH", "/api/admin/products/" + std::to_string(ts), R"({"visibility":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "visibility");
	reply = h.admin("PATCH", "/api/admin/products/" + std::to_string(h.runtime),
			R"({"visibility":"private"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "visibility");
	// An expansion's project made public.
	json = json_of(h.admin("PATCH", "/api/admin/products/" + std::to_string(ts),
						   R"({"visibility":"public"})"),
			200);
	TEST_EXPECT(json && str(json["product"]["key"]) == "SABRE" &&
			str(json["product"]["visibility"]) == "public");

	// DELETE: a base an expansion builds on is refused; the expansion goes, then the base.
	TEST_EXPECT(refused(h.admin("DELETE", base), 409, "has_expansion"));
	reply = h.admin("DELETE", "/api/admin/products/" + std::to_string(ts));
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	reply = h.admin("DELETE", base);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	TEST_EXPECT(refused(h.admin("DELETE", base), 404, "not_found"));
	TEST_EXPECT(!listed_product(h, bhd));
	return 0;
}

// An account's own game (made by its owner's route, products/project_routes.h; here through the
// repository) on the admin's routes: listed with its owner's id and name and no lobby; given a
// lobby by the admin; never made retail; deleted with its objects' purge queued, as any product.
int test_owned_product(Harness &h) {
	int64_t owner = 0, id = 0;
	{
		auto db = h.pool.acquire();
		db->exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
				 "VALUES ('modder', 'x', '0d0d0001', '1', 'Modder');");
		owner = db->query("SELECT id FROM players WHERE username = 'modder';")
						.front()
						.as_int(0)
						.value_or(0);
		hosted::NewProduct game;
		game.display_name = "Modder's game";
		game.kind = hosted::ProductKind::Game;
		game.title = "jo";
		game.key = "MODDERS";
		game.visibility = "private";
		game.created_by = "player modder";
		game.owner_id = owner;
		TEST_EXPECT(hosted::create_product(*db, game, id) == hosted::ProductOutcome::Ok);
	}
	const auto listed = listed_product(h, id);
	TEST_EXPECT(listed && listed["owner_id"].i() == owner && str(listed["owner"]) == "modder" &&
			str(listed["lobby"]).empty() && str(listed["kind"]) == "game" && !listed["retail"].b());
	const std::string path = "/api/admin/products/" + std::to_string(id);
	const net::HttpReply retail = h.admin("PATCH", path, R"({"retail":true})");
	TEST_EXPECT(refused(retail, 400, "invalid_field") && field_of(retail) == "retail");
	const auto lobbied = json_of(h.admin("PATCH", path, R"({"lobby":"jop_2_consumer"})"), 200);
	TEST_EXPECT(lobbied && str(lobbied["product"]["lobby"]) == "jop_2_consumer" &&
			str(lobbied["product"]["owner"]) == "modder");
	const net::HttpReply gone = h.admin("DELETE", path);
	TEST_EXPECT(gone.transport_ok && gone.code == 204 && !listed_product(h, id));
	{
		auto db = h.pool.acquire();
		const auto job = queued_job(*db, "product.purge:" + std::to_string(id));
		TEST_EXPECT(job && job->kind == "product.purge");
	}
	return 0;
}

// A game's trees as release.apply leaves them (products/product_jobs.h): the current one and the
// list's counts, a live server pinned to an older one counted as outdated and the game in use;
// and no route uploads a tree.
int test_trees(Harness &h) {
	const std::string base = "/api/admin/products/" + std::to_string(h.jo);
	const net::HttpReply upload =
			h.admin("POST", base + "/trees", R"({"files":[{"name":"resource.pff","size":1}]})");
	TEST_EXPECT(upload.transport_ok && (upload.code == 404 || upload.code == 405));
	int64_t t1 = 0, t2 = 0, server = 0;
	{
		auto db = h.pool.acquire();
		t1 = ready_tree(*db, h.jo, "JO V1.7.5.3");
		TEST_EXPECT(t1 != 0);
	}
	{
		const auto listed = listed_product(h, h.jo);
		TEST_EXPECT(listed && listed["current_tree"]["id"].i() == t1 &&
				str(listed["current_tree"]["version"]) == "JO V1.7.5.3" &&
				is_db_time(str(listed["current_tree"]["ready_at"])));
		TEST_EXPECT(listed["trees"].size() == 1 && listed["trees"][0]["id"].i() == t1 &&
				str(listed["trees"][0]["state"]) == "ready" && !listed["trees"][0].has("source") &&
				is_null(listed["trees"][0]["release_id"]) &&
				str(listed["trees"][0]["created_by"]) == "worker" &&
				listed["trees"][0]["files"].i() == 4 && listed["trees"][0]["missions"].i() == 2 &&
				listed["trees"][0]["bytes"].i() == 101 && listed["trees"][0]["pinned_by"].i() == 0);
	}
	// A live server pinned to it, then a newer tree current: the server is behind.
	{
		auto db = h.pool.acquire();
		server = insert_server(*db, h.jo, t1);
		t2 = ready_tree(*db, h.jo, "JO V1.7.5.4");
		TEST_EXPECT(t2 != 0);
	}
	{
		const auto listed = listed_product(h, h.jo);
		TEST_EXPECT(
				listed && listed["servers_live"].i() == 1 && listed["servers_outdated"].i() == 1);
		TEST_EXPECT(listed["trees"].size() == 2 && listed["trees"][0]["id"].i() == t2 &&
				listed["trees"][1]["id"].i() == t1 && listed["trees"][1]["pinned_by"].i() == 1);
	}
	// A game a live server runs is in use.
	TEST_EXPECT(refused(h.admin("DELETE", base), 409, "in_use"));
	{
		auto db = h.pool.acquire();
		db->exec("UPDATE hosted_servers SET state = 'stopped', ended_at = now() WHERE id = $1;",
				{ BindValue(server) });
	}
	return 0;
}

// The releases' routes: an app's build uploaded in parts through the route and completed
// (building, its release.build job) with every refusal (a game's, no app, no version); a game's
// release (built from its project, here a draft through the repository) published (one
// release.apply per section), retired and deleted with its package; an abandoned upload aborted.
int test_releases(Harness &h) {
	const std::string editor = std::to_string(h.editor);
	net::HttpReply reply = h.admin("POST", "/api/admin/releases", "{}");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "size");
	reply = h.admin("POST", "/api/admin/releases", R"({"size":0})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "size");
	reply = h.admin("POST", "/api/admin/releases", R"({"size":10,"version":"1.0"})");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "product_id");
	reply = h.admin("POST", "/api/admin/releases", R"({"size":10,"product_id":)" + editor + "}");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "version");
	reply = h.admin("POST", "/api/admin/releases",
			R"({"size":10,"version":"1.x","product_id":)" + editor + "}");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "version");
	reply = h.admin("POST", "/api/admin/releases",
			R"({"name":"bad\u0001name","size":10,"version":"1.0","product_id":)" + editor + "}");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "name");
	// A game has its releases built from its project: an upload naming it is refused.
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/releases",
								R"({"size":10,"version":"1.0","product_id":)" +
										std::to_string(h.jo) + "}"),
			409, "built_from_project"));
	reply = h.admin(
			"POST", "/api/admin/releases", R"({"size":10,"version":"1.0","product_id":99999})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "product_id");

	// Uploading: the editor's build begun under its source key, the part size the site slices
	// by; the release the editor's, of its version.
	auto json = json_of(h.admin("POST", "/api/admin/releases",
								R"({"name":"OpenNova Editor 0.0.22","size":28,"version":"0.0.22","product_id":)" +
										editor + "}"),
			201);
	TEST_EXPECT(json);
	const int64_t r1 = json["release"]["id"].i();
	TEST_EXPECT(r1 != 0 && str(json["release"]["status"]) == "uploading" &&
			str(json["release"]["name"]) == "OpenNova Editor 0.0.22" &&
			json["release"]["bytes"].i() == 28 && json["release"]["sections"].size() == 0 &&
			json["release"]["product_id"].i() == h.editor &&
			str(json["release"]["version"]) == "0.0.22" && is_null(json["release"]["source"]) &&
			str(json["release"]["published_at"]).empty() &&
			str(json["release"]["retired_at"]).empty());
	const std::string key = "updates/source-" + std::to_string(r1) + ".zip";
	TEST_EXPECT(str(json["upload"]["key"]) == key &&
			json["upload"]["part_size"].i() == hosted::ReleaseRoutes::kPartSize);
	const std::string upload_id = str(json["upload"]["upload_id"]);
	TEST_EXPECT(nws::LocalObjectStore::is_upload_id(upload_id));
	const std::string release = "/api/admin/releases/" + std::to_string(r1);

	// The build in one part through the route: the ETag back is the local store's (the part's
	// SHA-256 hex), and the reply is not cached.
	const std::string build = "not a zip, only test bytes";
	reply = h.send("PUT", release + "/parts/1?upload_id=" + upload_id, { kOctets, kBearer }, build);
	json = json_of(reply, 200);
	TEST_EXPECT(json);
	const std::string etag = str(json["etag"]);
	TEST_EXPECT(etag.size() == 64);
	bool no_store = false;
	for (const std::string &line : reply.headers)
		no_store = no_store || line == "Cache-Control: no-store";
	TEST_EXPECT(no_store);
	// A part the route refuses: over the cap, out of range, no upload id, a release that is
	// not there, an upload id the store does not know.
	reply = h.send("PUT", release + "/parts/1?upload_id=" + upload_id, { kOctets, kBearer },
			std::string(static_cast<size_t>(hosted::ReleaseRoutes::kPartSize) + 1, 'x'));
	TEST_EXPECT(refused(reply, 413, "too_large"));
	reply = h.send("PUT", release + "/parts/0?upload_id=" + upload_id, { kOctets, kBearer }, "x");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "part_number");
	reply = h.send("PUT", release + "/parts/10001?upload_id=" + upload_id, { kOctets, kBearer }, "x");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "part_number");
	reply = h.send("PUT", release + "/parts/1", { kOctets, kBearer }, "x");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "upload_id");
	reply = h.send("PUT", "/api/admin/releases/99999/parts/1?upload_id=" + upload_id,
			{ kOctets, kBearer }, "x");
	TEST_EXPECT(refused(reply, 404, "not_found"));
	reply = h.send("PUT", release + "/parts/1?upload_id=00000000-0000-4000-8000-000000000000",
			{ kOctets, kBearer }, "x");
	TEST_EXPECT(refused(reply, 502, "store_error"));
	// Published or retired before it is built: refused.
	TEST_EXPECT(refused(h.admin("POST", release + "/publish"), 409, "not_draft"));
	TEST_EXPECT(refused(h.admin("POST", release + "/retire"), 409, "not_published"));
	// Completed: a wrong ETag is the store's refusal; the right one makes it building, with its
	// build queued.
	TEST_EXPECT(refused(h.admin("POST", release + "/complete",
								R"({"upload_id":")" + upload_id +
										R"(","parts":[{"part_number":1,"etag":""}]})"),
			400, "invalid_field"));
	TEST_EXPECT(refused(
			h.admin("POST", release + "/complete",
					R"({"upload_id":")" + upload_id +
							R"(","parts":[{"part_number":1,"etag":"0000000000000000000000000000000000000000000000000000000000000000"}]})"),
			502, "store_error"));
	const std::string complete = R"({"upload_id":")" + upload_id +
			R"(","parts":[{"part_number":1,"etag":")" + etag + "\"}]}";
	json = json_of(h.admin("POST", release + "/complete", complete), 202);
	TEST_EXPECT(json && str(json["release"]["status"]) == "building");
	{
		auto db = h.pool.acquire();
		const auto job = queued_job(*db, "release.build:" + std::to_string(r1));
		TEST_EXPECT(job && job->kind == "release.build" &&
				job->payload.find("\"release_id\": " + std::to_string(r1)) != std::string::npos);
		std::string stored, error;
		TEST_EXPECT(io::read_file_text(h.store.path_of(key), stored, error) && stored == build);
	}
	TEST_EXPECT(refused(h.admin("POST", release + "/complete", complete), 409, "not_uploading"));
	reply = h.send("PUT", release + "/parts/2?upload_id=" + upload_id, { kOctets, kBearer }, "late");
	TEST_EXPECT(refused(reply, 409, "not_uploading"));
	TEST_EXPECT(refused(h.admin("DELETE", release), 409, "building"));

	// A game's release, built from its project (a draft through the repository, as the data
	// program's build and validate leave one), its package under its sha: two sections, one per
	// seeded JO product.
	const std::string sha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
	int64_t r2 = 0;
	{
		auto db = h.pool.acquire();
		hosted::NewBuiltRelease made;
		made.name = "JO 1.7.5.4";
		made.created_by = "admin root";
		made.product_id = h.jo;
		made.revision = 1;
		made.version = "1.7.5.4";
		r2 = hosted::create_built_release(*db, made);
		TEST_EXPECT(r2 != 0 && hosted::set_release_built(*db, r2, "$ opennova-project export\n"));
		TEST_EXPECT(hosted::set_release_validated(*db, r2, sha, 28, "Joint Operations",
				{ { h.jo, "", "JOPTR.CMD", "1.7.5.4", 3, 20 },
						{ h.jox, "jox01", "JOEPTR.CMD", "1.0.0.1", 1, 8 } }));
		std::string error;
		TEST_EXPECT(h.store.copy(key, "updates/" + sha + ".sfs", error));
	}
	const std::string game_release = "/api/admin/releases/" + std::to_string(r2);
	json = json_of(h.admin("GET", "/api/admin/releases"), 200);
	TEST_EXPECT(json && json["releases"].size() == 2 && json["releases"][0]["id"].i() == r2 &&
			json["releases"][1]["id"].i() == r1);
	{
		const auto &r = json["releases"][0];
		TEST_EXPECT(str(r["status"]) == "draft" && str(r["sha256"]) == sha &&
				str(r["package_product"]) == "Joint Operations" && r["product_id"].i() == h.jo &&
				str(r["source"]["project"]) == "JO" && r["source"]["revision"].i() == 1 &&
				is_null(r["source"]["base_revision"]) && str(r["version"]) == "1.7.5.4");
		TEST_EXPECT(r["sections"].size() == 2 && r["sections"][0]["product_id"].i() == h.jo &&
				str(r["sections"][0]["display_name"]) == "Joint Operations" &&
				str(r["sections"][0]["expansion"]).empty() &&
				str(r["sections"][0]["command_file"]) == "JOPTR.CMD" &&
				str(r["sections"][0]["version"]) == "1.7.5.4" && is_null(r["sections"][0]["tree"]));
		TEST_EXPECT(r["sections"][1]["product_id"].i() == h.jox &&
				str(r["sections"][1]["expansion"]) == "jox01" &&
				str(r["sections"][1]["display_name"]) == "Escalation");
	}

	// Published: one apply job per section; the products name it.
	json = json_of(h.admin("POST", game_release + "/publish"), 202);
	TEST_EXPECT(json && str(json["release"]["status"]) == "published" &&
			is_db_time(str(json["release"]["published_at"])));
	{
		auto db = h.pool.acquire();
		const auto jo_job =
				queued_job(*db, "release.apply:" + std::to_string(r2) + ":" + std::to_string(h.jo));
		const auto jox_job = queued_job(
				*db, "release.apply:" + std::to_string(r2) + ":" + std::to_string(h.jox));
		TEST_EXPECT(jo_job && jo_job->kind == "release.apply" &&
				jo_job->payload.find("\"product_id\": " + std::to_string(h.jo)) !=
						std::string::npos &&
				jo_job->payload.find("\"release_id\": " + std::to_string(r2)) != std::string::npos);
		TEST_EXPECT(jox_job && jox_job->kind == "release.apply");
		TEST_EXPECT(hosted::published_sections(*db).size() == 2);
		// The downloads host's render, queued with them; taken here, so the retire's shows.
		TEST_EXPECT(queued_job(*db, "downloads.publish"));
		db->exec("DELETE FROM jobs WHERE dedupe_key = 'downloads.publish';");
	}
	{
		const auto listed = listed_product(h, h.jo);
		TEST_EXPECT(listed && listed["published_release"]["id"].i() == r2 &&
				str(listed["published_release"]["name"]) == "JO 1.7.5.4" &&
				str(listed["published_release"]["sha256"]) == sha);
	}
	TEST_EXPECT(refused(h.admin("POST", game_release + "/publish"), 409, "not_draft"));
	TEST_EXPECT(refused(h.admin("DELETE", game_release), 409, "published"));

	// Retired, then deleted with its package.
	json = json_of(h.admin("POST", game_release + "/retire"), 202);
	TEST_EXPECT(json && str(json["release"]["status"]) == "retired" &&
			is_db_time(str(json["release"]["retired_at"])));
	TEST_EXPECT(refused(h.admin("POST", game_release + "/retire"), 409, "not_published"));
	TEST_EXPECT(is_null(listed_product(h, h.jo)["published_release"]));
	{
		auto db = h.pool.acquire();
		TEST_EXPECT(queued_job(*db, "downloads.publish"));
	}
	reply = h.admin("DELETE", game_release);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	{
		bool exists = true;
		uint64_t size = 0;
		std::string error;
		TEST_EXPECT(h.store.head("updates/" + sha + ".sfs", exists, size, error) && !exists);
		auto db = h.pool.acquire();
		TEST_EXPECT(!hosted::find_release(*db, r2));
	}
	TEST_EXPECT(refused(h.admin("DELETE", game_release), 404, "not_found"));
	TEST_EXPECT(json_of(h.admin("GET", "/api/admin/releases"), 200)["releases"].size() == 1);

	// An abandoned upload deleted: its upload aborted (a part after it refused by the store),
	// its row gone.
	json = json_of(h.admin("POST", "/api/admin/releases",
						   R"({"size":10,"version":"0.0.22","product_id":)" +
								   std::to_string(h.tools) + "}"),
			201);
	TEST_EXPECT(json);
	const int64_t r3 = json["release"]["id"].i();
	const std::string upload3 = str(json["upload"]["upload_id"]);
	const std::string release3 = "/api/admin/releases/" + std::to_string(r3);
	TEST_EXPECT(json_of(
			h.send("PUT", release3 + "/parts/1?upload_id=" + upload3, { kOctets, kBearer }, "abandoned"),
			200));
	reply = h.admin("DELETE", release3);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	{
		std::string etag, error;
		TEST_EXPECT(!h.store.upload_part(
				"updates/source-" + std::to_string(r3) + ".zip", upload3, 2, "more", etag, error));
	}
	return 0;
}

// The runtime's release (an app: its package uploaded by the Publish workflow, here a draft
// through the repository): published to the updater, no apply job, no tree.
int test_runtime_release(Harness &h) {
	int64_t release = 0;
	{
		auto db = h.pool.acquire();
		release = hosted::create_release(*db, "runtime test", 1, "test");
		TEST_EXPECT(hosted::set_release_status(*db, release, hosted::ReleaseStatus::Uploading,
				hosted::ReleaseStatus::Validating));
		TEST_EXPECT(hosted::set_release_validated(*db, release, std::string(64, 'c'), 1,
				"OpenNova runtime", { { h.runtime, "", "ONRUNTIME.CMD", "1.0", 1, 1 } }));
	}
	const std::string path = "/api/admin/releases/" + std::to_string(release);
	const auto json = json_of(h.admin("POST", path + "/publish"), 202);
	TEST_EXPECT(json && str(json["release"]["status"]) == "published" &&
			json["release"]["product_id"].i() == h.runtime &&
			str(json["release"]["sections"][0]["display_name"]) == "OpenNova runtime");
	{
		auto db = h.pool.acquire();
		TEST_EXPECT(!queued_job(*db, "release.apply:" + std::to_string(release) + ":" +
				std::to_string(h.runtime)));
		TEST_EXPECT(hosted::list_trees(*db, h.runtime).empty());
		const auto products = hosted::published_sections(*db);
		TEST_EXPECT(products.size() == 1 && products[0].updater_login == "ONRUNTIME" &&
				products[0].command_file == "ONRUNTIME.CMD" && !products[0].retail);
	}
	const auto listed = listed_product(h, h.runtime);
	TEST_EXPECT(listed && listed["published_release"]["id"].i() == release &&
			listed["trees"].size() == 0);
	return 0;
}

// A download-only app's release (the tools' build kept as uploaded, here a draft through the
// repository as the build leaves one: no section): published to its own product, no apply job,
// nothing for the updater.
int test_download_release(Harness &h) {
	int64_t release = 0;
	{
		auto db = h.pool.acquire();
		release = hosted::create_release(*db, "tools test", 1, "test", h.tools, "0.0.22");
		TEST_EXPECT(hosted::set_release_status(*db, release, hosted::ReleaseStatus::Uploading,
				hosted::ReleaseStatus::Building));
		TEST_EXPECT(hosted::set_release_stored(*db, release, std::string(64, 'd'), 1));
		TEST_EXPECT(!hosted::set_release_stored(*db, release, std::string(64, 'e'), 1));
	}
	const std::string path = "/api/admin/releases/" + std::to_string(release);
	const auto json = json_of(h.admin("POST", path + "/publish"), 202);
	TEST_EXPECT(json && str(json["release"]["status"]) == "published" &&
			json["release"]["product_id"].i() == h.tools &&
			json["release"]["sections"].size() == 0 && str(json["release"]["version"]) == "0.0.22");
	{
		auto db = h.pool.acquire();
		TEST_EXPECT(!queued_job(*db, "release.apply:" + std::to_string(release) + ":" +
				std::to_string(h.tools)));
		for (const hosted::PublishedSection &s : hosted::published_sections(*db))
			TEST_EXPECT(s.product_id != h.tools);
	}
	const auto listed = listed_product(h, h.tools);
	TEST_EXPECT(listed && listed["published_release"]["id"].i() == release &&
			str(listed["download"]) == "opennova-tools-windows.zip");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("product_routes");
		test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
		ConnectionPool pool(database.options());
		test_redis::TestRedis redis;
		nws::LocalObjectStore store(temp.file("store"));

		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.web_host = "127.0.0.1";
		config.admin_api_token = kAdminToken;
		config.http_port = 0;
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates";
		config.static_dir = temp.path / "static";
		std::filesystem::create_directories(config.templates_dir);

		nws::HttpListener http(pool, redis.pool());
		http.set_object_store(&store);
		TEST_EXPECT(http.start(config));
		TEST_EXPECT(http.bound_port() != 0);

		Harness harness{ pool, store, http.bound_port(), 0, 0, 0 };
		{
			auto db = pool.acquire();
			harness.jo = seeded(*db, "Joint Operations");
			harness.jox = seeded(*db, "Escalation");
			harness.runtime = seeded(*db, "OpenNova runtime");
			harness.editor = seeded(*db, "OpenNova Editor");
			harness.tools = seeded(*db, "Tools and dedicated server");
			TEST_EXPECT(harness.jo != 0 && harness.jox != 0 && harness.runtime != 0 &&
					harness.editor != 0 && harness.tools != 0);
		}

		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "refused", test_refused },
			{ "list", test_list },
			{ "create_patch_delete", test_create_patch_delete },
			{ "owned_product", test_owned_product },
			{ "trees", test_trees },
			{ "releases", test_releases },
			{ "runtime_release", test_runtime_release },
			{ "download_release", test_download_release },
		};
		rc = 0;
		for (const Case &c : cases) {
			const int result = c.fn(harness);
			std::printf("product_routes: %s %s\n", c.name, result == 0 ? "ok" : "FAILED");
			rc |= result;
		}
		http.stop();
	}
	net::shutdown();
	return rc;
}
