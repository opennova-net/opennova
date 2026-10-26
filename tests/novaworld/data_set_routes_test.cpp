// The games' and the update releases' admin routes (apps/novaworld_web/data_sets/data_set_routes.h,
// release_routes.h; ADR 0064), driven in-process: a database of the run's own
// (common/test_database.h, migrated with the games seed, so the seeded games are there), a Redis
// namespace of its own (common/test_redis.h), a local object store in a temp directory
// (core/local_object_store.h), the Crow HttpListener from opennova_novaworld_web_core on a port
// the OS picks, and requests through apps/common's http_exchange as the Bearer admin. The cases
// share the listener and run in order: every route refused without the admin token; the list's
// shapes over the seeded rows (every game, the runtime among them, each with its project's key,
// visibility and head); a base and an expansion created with every refusal (the shapes, a
// duplicate login, a project's key and visibility apart, a malformed or duplicate key, a
// duplicate expansion, no base parent), patched (its project renamed, made public, the runtime
// given none) and deleted (a base with an expansion refused has_expansion); a game's trees as
// release.apply leaves them (current, pinned by a live server and counted as outdated, the game
// then in use), no tree upload route left; releases created (naming a game with a project
// refused built_from_project), uploaded in parts through the route and completed (validating, a
// release.validate job), a part refused once the game it names has a project, a draft through
// the repository naming its game, published (one release.apply per section), retired, deleted
// with its package object (a published one refused, an abandoned upload aborted); the runtime's
// release published to the updater and built into no tree.

#include "core/local_object_store.h"
#include "data_sets/data_set_repository.h"
#include "data_sets/data_set_routes.h"
#include "data_sets/release_routes.h"
#include "http_listener.h"
#include "jobs/job_queue.h"
#include "net_http.h"
#include "net_sockets.h"
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

constexpr const char *kAdminToken = "data-set-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer data-set-routes-admin-token";
constexpr const char *kJson = "Content-Type: application/json";
constexpr const char *kOctets = "Content-Type: application/octet-stream";

struct Harness {
	ConnectionPool &pool;
	nws::LocalObjectStore &store;
	uint16_t port = 0;
	int64_t jo = 0; // the seeded Joint Operations and Escalation rows
	int64_t jox = 0;
	int64_t runtime = 0; // the seeded OpenNova runtime's

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
			"SELECT id FROM data_sets WHERE display_name = $1;", { BindValue(display_name) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

// A hosted server row as a start writes it, pinned to `base_tree`.
int64_t insert_server(Database &db, int64_t data_set_id, int64_t base_tree) {
	const auto rows = db.query(
			"INSERT INTO hosted_servers (state, game_name, max_players, time_limit, kill_limit, "
			"max_score, respawn, start_delay, missions, data_set_id, base_tree_id) "
			"VALUES ('running', 'srv', 32, 30, 50, 5, 5, 0, 'a.bms', $1, $2) RETURNING id;",
			{ BindValue(data_set_id), BindValue(base_tree) });
	return rows.front().as_int(0).value_or(0);
}

// A ready tree of `data_set_id` with `version`, as release.apply leaves one: its id.
int64_t ready_tree(Database &db, int64_t data_set_id, const char *version) {
	const int64_t id = hosted::create_tree(db, data_set_id, std::nullopt, "worker");
	hosted::TreeReady ready;
	ready.files = { { "resource.pff", 35 }, { "localres.pff", 8 }, { "language.pff", 8 },
		{ "version.txt", 50 } };
	ready.missions = { { "a.bms", "Alpha" }, { "b.bms", "Bravo" } };
	ready.version_text = version;
	ready.bytes = 101;
	return hosted::finish_tree(db, id, ready) ? id : 0;
}

// The data set `id` from the admin list, or an empty value.
crow::json::rvalue listed_data_set(const Harness &h, int64_t id) {
	const auto json = json_of(h.admin("GET", "/api/admin/data-sets"), 200);
	if (!json)
		return {};
	for (const auto &d : json["data_sets"])
		if (d["id"].i() == id)
			return d;
	return {};
}

// --- cases ---

int test_refused(Harness &h) {
	net::HttpReply reply = h.send("GET", "/api/admin/data-sets");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("GET", "/api/admin/data-sets", { "Authorization: Bearer wrong" });
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("POST", "/api/admin/data-sets", { kJson }, "{}");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("PATCH", "/api/admin/data-sets/1", { kJson }, "{}");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("GET", "/api/admin/releases", { "Authorization: Bearer wrong" });
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("POST", "/api/admin/releases", { kJson }, "{}");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("PUT", "/api/admin/releases/1/parts/1?upload_id=x", { kOctets }, "x");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	return 0;
}

// GET /api/admin/data-sets over the seeded rows: the local store, the titles, the three
// games, every game (bases, expansions, then the runtime) with its project's key, visibility
// and head, Joint Operations' row with no tree yet.
int test_list(Harness &h) {
	const auto json = json_of(h.admin("GET", "/api/admin/data-sets"), 200);
	TEST_EXPECT(json);
	TEST_EXPECT(str(json["store"]["kind"]) == "local");
	TEST_EXPECT(json["titles"].size() == 2 && str(json["titles"][0]) == "jo" &&
			str(json["titles"][1]) == "dfx2");
	TEST_EXPECT(json["games"].size() == 3 && str(json["games"][2]["slug"]) == "jop_2_consumer" &&
			!str(json["games"][2]["display_name"]).empty());
	TEST_EXPECT(json["data_sets"].size() == 6);
	TEST_EXPECT(str(json["data_sets"][0]["kind"]) == "base" &&
			str(json["data_sets"][3]["kind"]) == "expansion" &&
			str(json["data_sets"][5]["kind"]) == "update");
	const auto jo = listed_data_set(h, h.jo);
	TEST_EXPECT(jo);
	TEST_EXPECT(str(jo["display_name"]) == "Joint Operations" && str(jo["title"]) == "jo" &&
			str(jo["game"]) == "jop_2_consumer" && str(jo["updater_login"]) == "JOTRus" &&
			str(jo["command_file"]) == "JOPTR.CMD" && jo["retail_updater"].b());
	TEST_EXPECT(str(jo["key"]) == "JO" && str(jo["visibility"]) == "private" &&
			jo["head"].i() == 0 && !jo.has("baseline_tree_id"));
	TEST_EXPECT(is_null(jo["parent_id"]) && is_null(jo["current_tree"]) &&
			is_null(jo["published_release"]));
	TEST_EXPECT(jo["servers_live"].i() == 0 && jo["servers_outdated"].i() == 0 &&
			jo["trees"].size() == 0);
	const auto jox = listed_data_set(h, h.jox);
	TEST_EXPECT(jox && jox["parent_id"].i() == h.jo && str(jox["expansion_code"]) == "jox01" &&
			str(jox["key"]) == "JOX01");
	// The runtime: a game with no project, its releases from CI.
	const auto runtime = listed_data_set(h, h.runtime);
	TEST_EXPECT(runtime && str(runtime["kind"]) == "update" &&
			str(runtime["updater_login"]) == "ONRUNTIME" && is_null(runtime["key"]) &&
			is_null(runtime["visibility"]) && runtime["head"].i() == 0 &&
			!runtime["retail_updater"].b());
	return 0;
}

// POST, PATCH and DELETE /api/admin/data-sets: a base and its expansion, and the refusals.
int test_create_patch_delete(Harness &h) {
	// The shapes.
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/data-sets", "nope"), 400, "invalid_json"));
	net::HttpReply reply = h.admin("POST", "/api/admin/data-sets",
			R"({"kind":"base","title":"jo","game":"jop_2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "display_name");
	reply = h.admin("POST", "/api/admin/data-sets", R"({"display_name":"X","kind":"other"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "kind");
	reply = h.admin("POST", "/api/admin/data-sets", R"({"display_name":"X","kind":"update"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "kind");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"bhd","game":"jop_2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "title");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"no_such_lobby"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "game");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","expansion_code":"x"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "expansion_code");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":1,"kind":"base","title":"jo","game":"jop_2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "display_name");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","updater_login":"BHDus"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "command_file");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","updater_login":"BHD us","command_file":"BHDPTR.CMD"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "updater_login");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","updater_login":"BHDus","command_file":"BHDPTR.TXT"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "command_file");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","retail_updater":"yes"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "retail_updater");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"expansion","expansion_code":"bhx01"})");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "parent_id");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"expansion","parent_id":1,"expansion_code":"BHX 1"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "expansion_code");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"expansion","parent_id":1,"expansion_code":"bhx01","title":"jo"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "title");
	// A project's key and visibility come together, each of its shape.
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","key":"BHD"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "visibility");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","visibility":"public"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "key");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","key":"bhd","visibility":"public"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "key");
	reply = h.admin("POST", "/api/admin/data-sets",
			R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","key":"BHD","visibility":"secret"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "visibility");
	// A duplicate login (without case) and a key another game's project has are the database's
	// refusals.
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/data-sets",
					R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","updater_login":"jotrus","command_file":"X.CMD"})"),
			409, "duplicate_login"));
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/data-sets",
					R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","key":"JO","visibility":"public"})"),
			409, "duplicate_key"));

	// A base with its project.
	auto json = json_of(
			h.admin("POST", "/api/admin/data-sets",
					R"({"display_name":"Black Hawk Down","kind":"base","title":"dfx2","game":"dfx2_consumer","updater_login":"BHDus","command_file":"BHDPTR.CMD","retail_updater":true,"key":"BHD","visibility":"private"})"),
			201);
	TEST_EXPECT(json);
	const int64_t bhd = json["data_set"]["id"].i();
	TEST_EXPECT(bhd != 0 && str(json["data_set"]["display_name"]) == "Black Hawk Down" &&
			str(json["data_set"]["title"]) == "dfx2" &&
			str(json["data_set"]["updater_login"]) == "BHDus" &&
			json["data_set"]["retail_updater"].b() && json["data_set"]["trees"].size() == 0 &&
			is_null(json["data_set"]["current_tree"]) && json["data_set"]["servers_live"].i() == 0);
	TEST_EXPECT(str(json["data_set"]["key"]) == "BHD" &&
			str(json["data_set"]["visibility"]) == "private" && json["data_set"]["head"].i() == 0);
	// Its expansion, once, with no project.
	const std::string expansion =
			R"({"display_name":"Team Sabre","kind":"expansion","parent_id":)" +
			std::to_string(bhd) + R"(,"expansion_code":"bhx01"})";
	json = json_of(h.admin("POST", "/api/admin/data-sets", expansion), 201);
	TEST_EXPECT(json);
	const int64_t ts = json["data_set"]["id"].i();
	TEST_EXPECT(ts != 0 && str(json["data_set"]["kind"]) == "expansion" &&
			json["data_set"]["parent_id"].i() == bhd &&
			str(json["data_set"]["expansion_code"]) == "bhx01" &&
			str(json["data_set"]["title"]).empty() &&
			str(json["data_set"]["updater_login"]).empty() && is_null(json["data_set"]["key"]) &&
			is_null(json["data_set"]["visibility"]));
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/data-sets", expansion), 409, "duplicate_expansion"));
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/data-sets",
					R"({"display_name":"Orphan","kind":"expansion","parent_id":99999,"expansion_code":"bhx02"})"),
			409, "no_such_parent"));
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/data-sets",
								R"({"display_name":"Orphan","kind":"expansion","parent_id":)" +
										std::to_string(ts) + R"(,"expansion_code":"bhx02"})"),
			409, "no_such_parent"));
	TEST_EXPECT(listed_data_set(h, bhd) && listed_data_set(h, ts));

	// PATCH.
	const std::string base = "/api/admin/data-sets/" + std::to_string(bhd);
	json = json_of(h.admin("PATCH", base, R"({"display_name":"BHD"})"), 200);
	TEST_EXPECT(json && str(json["data_set"]["display_name"]) == "BHD");
	reply = h.admin("PATCH", base, R"({"display_name":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "display_name");
	// The updater product: a flag, a login another holds, a command file without its login,
	// both cleared.
	json = json_of(h.admin("PATCH", base, R"({"retail_updater":false})"), 200);
	TEST_EXPECT(json && !json["data_set"]["retail_updater"].b());
	TEST_EXPECT(refused(h.admin("PATCH", base, R"({"updater_login":"JOTRUS"})"), 409,
			"duplicate_login"));
	reply = h.admin("PATCH", base, R"({"command_file":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "command_file");
	json = json_of(h.admin("PATCH", base, R"({"updater_login":"","command_file":""})"), 200);
	TEST_EXPECT(json && str(json["data_set"]["updater_login"]).empty());
	reply = h.admin(
			"PATCH", "/api/admin/data-sets/" + std::to_string(ts), R"({"game":"dfx2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "game");
	json = json_of(h.admin("PATCH", base, R"({"game":"acidgambit_0_consumer"})"), 200);
	TEST_EXPECT(json && str(json["data_set"]["game"]) == "acidgambit_0_consumer");
	TEST_EXPECT(refused(h.admin("PATCH", "/api/admin/data-sets/99999", R"({"display_name":"X"})"),
			404, "not_found"));
	// The project: renamed and made public; a key in use, a malformed one, an expansion given a
	// visibility with no key, and the runtime given a project, refused.
	json = json_of(h.admin("PATCH", base, R"({"key":"BHAWK","visibility":"public"})"), 200);
	TEST_EXPECT(json && str(json["data_set"]["key"]) == "BHAWK" &&
			str(json["data_set"]["visibility"]) == "public");
	TEST_EXPECT(refused(h.admin("PATCH", base, R"({"key":"JOX01"})"), 409, "duplicate_key"));
	reply = h.admin("PATCH", base, R"({"key":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "key");
	reply = h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(ts),
			R"({"visibility":"public"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "key");
	reply = h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(h.runtime),
			R"({"key":"RUNTIME","visibility":"private"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "key");
	// An expansion given a project of its own.
	json = json_of(h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(ts),
						   R"({"key":"SABRE","visibility":"private"})"),
			200);
	TEST_EXPECT(json && str(json["data_set"]["key"]) == "SABRE");

	// DELETE: a base an expansion builds on is refused; the expansion goes, then the base.
	TEST_EXPECT(refused(h.admin("DELETE", base), 409, "has_expansion"));
	reply = h.admin("DELETE", "/api/admin/data-sets/" + std::to_string(ts));
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	reply = h.admin("DELETE", base);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	TEST_EXPECT(refused(h.admin("DELETE", base), 404, "not_found"));
	TEST_EXPECT(!listed_data_set(h, bhd));
	return 0;
}

// A game's trees as release.apply leaves them (data_sets/data_set_jobs.h): the current one and the
// list's counts, a live server pinned to an older one counted as outdated and the game in use;
// and no route uploads a tree.
int test_trees(Harness &h) {
	const std::string base = "/api/admin/data-sets/" + std::to_string(h.jo);
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
		const auto listed = listed_data_set(h, h.jo);
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
		const auto listed = listed_data_set(h, h.jo);
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

// The releases' routes.
int test_releases(Harness &h) {
	net::HttpReply reply = h.admin("POST", "/api/admin/releases", "{}");
	TEST_EXPECT(refused(reply, 400, "missing_field") && field_of(reply) == "size");
	reply = h.admin("POST", "/api/admin/releases", R"({"size":0})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "size");
	reply = h.admin("POST", "/api/admin/releases", R"({"name":"bad\u0001name","size":10})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "name");
	// A game with a project has its releases built: an upload naming it is refused.
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/releases",
								R"({"size":10,"data_set_id":)" + std::to_string(h.jo) + "}"),
			409, "built_from_project"));
	reply = h.admin("POST", "/api/admin/releases", R"({"size":10,"data_set_id":99999})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "data_set_id");

	// Uploading: the package's upload begun under its upload key, the part size the site
	// slices by; no game named yet.
	auto json =
			json_of(h.admin("POST", "/api/admin/releases", R"({"name":"1.7.5.4","size":28})"), 201);
	TEST_EXPECT(json);
	const int64_t r1 = json["release"]["id"].i();
	TEST_EXPECT(r1 != 0 && str(json["release"]["status"]) == "uploading" &&
			str(json["release"]["name"]) == "1.7.5.4" && json["release"]["bytes"].i() == 28 &&
			json["release"]["sections"].size() == 0 && is_null(json["release"]["data_set_id"]) &&
			is_null(json["release"]["source"]) && str(json["release"]["published_at"]).empty() &&
			str(json["release"]["retired_at"]).empty());
	const std::string key = "updates/upload-" + std::to_string(r1) + ".sfs";
	TEST_EXPECT(str(json["upload"]["key"]) == key &&
			json["upload"]["part_size"].i() == hosted::ReleaseRoutes::kPartSize);
	const std::string upload_id = str(json["upload"]["upload_id"]);
	TEST_EXPECT(nws::LocalObjectStore::is_upload_id(upload_id));
	const std::string release = "/api/admin/releases/" + std::to_string(r1);

	// The package in one part through the route: the ETag back is the local store's (the
	// part's SHA-256 hex), and the reply is not cached.
	const std::string package = "not a package, but 28 bytes.";
	reply = h.send("PUT", release + "/parts/1?upload_id=" + upload_id, { kOctets, kBearer }, package);
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
	// Published or retired before it is validated: refused.
	TEST_EXPECT(refused(h.admin("POST", release + "/publish"), 409, "not_draft"));
	TEST_EXPECT(refused(h.admin("POST", release + "/retire"), 409, "not_published"));
	// Completed: a wrong ETag is the store's refusal; the right one makes it validating,
	// with its job.
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
	TEST_EXPECT(json && str(json["release"]["status"]) == "validating");
	{
		auto db = h.pool.acquire();
		const auto job = queued_job(*db, "release.validate:" + std::to_string(r1));
		TEST_EXPECT(job && job->kind == "release.validate" &&
				job->payload.find("\"release_id\": " + std::to_string(r1)) != std::string::npos);
		std::string stored, error;
		TEST_EXPECT(io::read_file_text(h.store.path_of(key), stored, error) && stored == package);
	}
	TEST_EXPECT(refused(h.admin("POST", release + "/complete", complete), 409, "not_uploading"));
	reply = h.send("PUT", release + "/parts/2?upload_id=" + upload_id, { kOctets, kBearer }, "late");
	TEST_EXPECT(refused(reply, 409, "not_uploading"));
	TEST_EXPECT(refused(h.admin("DELETE", release), 409, "validating"));

	// A draft through the repository (what the worker's validate does), its package moved
	// under its sha: two sections, one per seeded JO data set; the release Joint Operations'.
	const std::string sha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
	{
		auto db = h.pool.acquire();
		TEST_EXPECT(hosted::set_release_validated(*db, r1, sha, 28, "Joint Operations",
				{ { h.jo, "", "JOPTR.CMD", "1.7.5.4", 3, 20 },
						{ h.jox, "jox01", "JOEPTR.CMD", "1.0.0.1", 1, 8 } }));
		std::string error;
		TEST_EXPECT(
				h.store.copy(key, "updates/" + sha + ".sfs", error) && h.store.remove(key, error));
	}
	json = json_of(h.admin("GET", "/api/admin/releases"), 200);
	TEST_EXPECT(json && json["releases"].size() == 1 && json["releases"][0]["id"].i() == r1);
	{
		const auto &r = json["releases"][0];
		TEST_EXPECT(str(r["status"]) == "draft" && str(r["sha256"]) == sha &&
				str(r["package_product"]) == "Joint Operations" && r["data_set_id"].i() == h.jo);
		TEST_EXPECT(r["sections"].size() == 2 && r["sections"][0]["data_set_id"].i() == h.jo &&
				str(r["sections"][0]["display_name"]) == "Joint Operations" &&
				str(r["sections"][0]["expansion"]).empty() &&
				str(r["sections"][0]["command_file"]) == "JOPTR.CMD" &&
				str(r["sections"][0]["version"]) == "1.7.5.4" && is_null(r["sections"][0]["tree"]));
		TEST_EXPECT(r["sections"][1]["data_set_id"].i() == h.jox &&
				str(r["sections"][1]["expansion"]) == "jox01" &&
				str(r["sections"][1]["display_name"]) == "Escalation");
	}

	// Published: one apply job per section; the data sets name it.
	json = json_of(h.admin("POST", release + "/publish"), 202);
	TEST_EXPECT(json && str(json["release"]["status"]) == "published" &&
			is_db_time(str(json["release"]["published_at"])));
	{
		auto db = h.pool.acquire();
		const auto jo_job =
				queued_job(*db, "release.apply:" + std::to_string(r1) + ":" + std::to_string(h.jo));
		const auto jox_job = queued_job(
				*db, "release.apply:" + std::to_string(r1) + ":" + std::to_string(h.jox));
		TEST_EXPECT(jo_job && jo_job->kind == "release.apply" &&
				jo_job->payload.find("\"data_set_id\": " + std::to_string(h.jo)) !=
						std::string::npos &&
				jo_job->payload.find("\"release_id\": " + std::to_string(r1)) != std::string::npos);
		TEST_EXPECT(jox_job && jox_job->kind == "release.apply");
		TEST_EXPECT(hosted::published_sections(*db).size() == 2);
	}
	{
		const auto listed = listed_data_set(h, h.jo);
		TEST_EXPECT(listed && listed["published_release"]["id"].i() == r1 &&
				str(listed["published_release"]["name"]) == "1.7.5.4" &&
				str(listed["published_release"]["sha256"]) == sha);
	}
	TEST_EXPECT(refused(h.admin("POST", release + "/publish"), 409, "not_draft"));
	TEST_EXPECT(refused(h.admin("DELETE", release), 409, "published"));

	// Retired, then deleted with its package.
	json = json_of(h.admin("POST", release + "/retire"), 202);
	TEST_EXPECT(json && str(json["release"]["status"]) == "retired" &&
			is_db_time(str(json["release"]["retired_at"])));
	TEST_EXPECT(refused(h.admin("POST", release + "/retire"), 409, "not_published"));
	TEST_EXPECT(is_null(listed_data_set(h, h.jo)["published_release"]));
	reply = h.admin("DELETE", release);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	{
		bool exists = true;
		uint64_t size = 0;
		std::string error;
		TEST_EXPECT(h.store.head("updates/" + sha + ".sfs", exists, size, error) && !exists);
		auto db = h.pool.acquire();
		TEST_EXPECT(!hosted::find_release(*db, r1));
	}
	TEST_EXPECT(refused(h.admin("DELETE", release), 404, "not_found"));
	TEST_EXPECT(json_of(h.admin("GET", "/api/admin/releases"), 200)["releases"].size() == 0);

	// An abandoned upload deleted: its upload aborted (a part after it refused by the
	// store), its row gone.
	json = json_of(h.admin("POST", "/api/admin/releases", R"({"size":10})"), 201);
	TEST_EXPECT(json);
	const int64_t r2 = json["release"]["id"].i();
	const std::string upload2 = str(json["upload"]["upload_id"]);
	const std::string release2 = "/api/admin/releases/" + std::to_string(r2);
	TEST_EXPECT(json_of(
			h.send("PUT", release2 + "/parts/1?upload_id=" + upload2, { kOctets, kBearer }, "abandoned"),
			200));
	reply = h.admin("DELETE", release2);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	{
		std::string etag, error;
		TEST_EXPECT(!h.store.upload_part(
				"updates/upload-" + std::to_string(r2) + ".sfs", upload2, 2, "more", etag, error));
	}

	// An upload naming a game with no project, which then gets one: its parts and its completion
	// refused (its releases are built now).
	json = json_of(h.admin("POST", "/api/admin/data-sets",
						   R"({"display_name":"Loose","kind":"base","title":"jo","game":"jop_2_consumer"})"),
			201);
	TEST_EXPECT(json);
	const int64_t loose = json ? json["data_set"]["id"].i() : 0;
	json = json_of(h.admin("POST", "/api/admin/releases",
						   R"({"size":10,"data_set_id":)" + std::to_string(loose) + "}"),
			201);
	TEST_EXPECT(json && json["release"]["data_set_id"].i() == loose);
	const int64_t r3 = json ? json["release"]["id"].i() : 0;
	const std::string upload3 = json ? str(json["upload"]["upload_id"]) : std::string();
	const std::string release3 = "/api/admin/releases/" + std::to_string(r3);
	TEST_EXPECT(json_of(h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(loose),
								R"({"key":"LOOSE","visibility":"private"})"),
			200));
	TEST_EXPECT(refused(
			h.send("PUT", release3 + "/parts/1?upload_id=" + upload3, { kOctets, kBearer }, "x"),
			409, "built_from_project"));
	TEST_EXPECT(refused(h.admin("POST", release3 + "/complete",
								R"({"upload_id":")" + upload3 +
										R"(","parts":[{"part_number":1,"etag":"x"}]})"),
			409, "built_from_project"));
	reply = h.admin("DELETE", release3);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	reply = h.admin("DELETE", "/api/admin/data-sets/" + std::to_string(loose));
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	return 0;
}

// The runtime's release (a game with no project: its package uploaded by the Publish workflow,
// here a draft through the repository): published to the updater, no apply job, no tree.
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
			json["release"]["data_set_id"].i() == h.runtime &&
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
	const auto listed = listed_data_set(h, h.runtime);
	TEST_EXPECT(listed && listed["published_release"]["id"].i() == release &&
			listed["trees"].size() == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("data_set_routes");
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
			TEST_EXPECT(harness.jo != 0 && harness.jox != 0 && harness.runtime != 0);
		}

		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "refused", test_refused },
			{ "list", test_list },
			{ "create_patch_delete", test_create_patch_delete },
			{ "trees", test_trees },
			{ "releases", test_releases },
			{ "runtime_release", test_runtime_release },
		};
		rc = 0;
		for (const Case &c : cases) {
			const int result = c.fn(harness);
			std::printf("data_set_routes: %s %s\n", c.name, result == 0 ? "ok" : "FAILED");
			rc |= result;
		}
		http.stop();
	}
	net::shutdown();
	return rc;
}
