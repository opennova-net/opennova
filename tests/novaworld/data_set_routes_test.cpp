// The hosted data sets' and the update releases' admin routes
// (apps/novaworld_web/data_sets/data_set_routes.h, release_routes.h; ADR 0064), driven
// in-process: a database of the run's own (common/test_database.h, migrated with the games
// seed, so the four seeded data sets are there), a Redis namespace of its own
// (common/test_redis.h), a local object store in a temp directory (core/local_object_store.h),
// the Crow HttpListener from opennova_novaworld_web_core on a port the OS picks, and
// requests through apps/common's http_exchange as the Bearer admin. The cases share the
// listener and run in order: every route refused without the admin token; the list's
// shapes over the seeded rows; a base and an expansion created with every refusal (the
// shapes, a duplicate login, a duplicate expansion, no base parent), patched and deleted (a
// base with an expansion is in use); a tree upload begun with the kind's files (a missing required one
// and an unexpected name refused), each part PUT through the route (the ETag back; a part
// over the cap, out of range, of a name the kind lacks or with no upload id refused), each
// file completed (a wrong ETag refused by the store), the tree finalized with its
// tree.finalize job under its dedupe key, then ready through the repository and current
// (its delete refused), an upload abandoned (aborted in the store, a part after it
// refused), a superseded tree pruned with a tree.prune job, a pinned one refused and
// counted as outdated; releases created, uploaded in parts through the route and completed
// (validating, a release.validate job), a draft through the repository published (one
// release.apply per section), retired, deleted with its package object (a published one
// refused, an abandoned upload aborted).

#include "core/local_object_store.h"
#include "data_sets/data_set_routes.h"
#include "hosted/data_set_files.h"
#include "hosted/data_set_repository.h"
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
	// A part's PUT, as the site sends it.
	net::HttpReply put_part(const std::string &tree, const std::string &name,
			const std::string &upload_id, int number, const std::string &bytes) const {
		return send("PUT",
				tree + "/files/" + name + "/parts/" + std::to_string(number) +
						"?upload_id=" + upload_id,
				{ kOctets, kBearer }, bytes);
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

// A tree upload begun for `names`: the tree id and the uploads' ids by name.
struct Upload {
	std::string name, key, upload_id;
};
struct BegunTree {
	int64_t id = 0;
	std::vector<Upload> uploads;
};
std::optional<BegunTree> begin_tree(
		const Harness &h, int64_t data_set, const std::vector<std::string> &names) {
	crow::json::wvalue body;
	std::vector<crow::json::wvalue> files;
	for (const std::string &name : names) {
		crow::json::wvalue f;
		f["name"] = name;
		f["size"] = 1000;
		files.push_back(std::move(f));
	}
	body["files"] = std::move(files);
	const auto json =
			json_of(h.admin("POST", "/api/admin/data-sets/" + std::to_string(data_set) + "/trees",
							body.dump()),
					201);
	if (!json)
		return std::nullopt;
	BegunTree tree;
	tree.id = json["tree"]["id"].i();
	for (const auto &u : json["uploads"])
		tree.uploads.push_back({ str(u["name"]), str(u["key"]), str(u["upload_id"]) });
	return tree;
}

// --- cases ---

int test_refused(Harness &h) {
	net::HttpReply reply = h.send("GET", "/api/admin/data-sets");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("GET", "/api/admin/data-sets", { "Authorization: Bearer wrong" });
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("POST", "/api/admin/data-sets", { kJson }, "{}");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("PUT", "/api/admin/data-sets/1/trees/1/files/resource.pff/parts/1?upload_id=x",
			{ kOctets }, "x");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("POST", "/api/admin/data-sets/1/trees/1/files/resource.pff/complete", { kJson },
			"{}");
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
// games, four data sets (bases first), Joint Operations' row with no tree yet.
int test_list(Harness &h) {
	const auto json = json_of(h.admin("GET", "/api/admin/data-sets"), 200);
	TEST_EXPECT(json);
	TEST_EXPECT(str(json["store"]["kind"]) == "local");
	TEST_EXPECT(json["titles"].size() == 2 && str(json["titles"][0]) == "jo" &&
			str(json["titles"][1]) == "dfx2");
	TEST_EXPECT(json["games"].size() == 3 && str(json["games"][1]["slug"]) == "jop_2_consumer" &&
			!str(json["games"][1]["display_name"]).empty());
	TEST_EXPECT(json["data_sets"].size() == 4);
	TEST_EXPECT(str(json["data_sets"][0]["kind"]) == "base" &&
			str(json["data_sets"][3]["kind"]) == "expansion");
	const auto jo = listed_data_set(h, h.jo);
	TEST_EXPECT(jo);
	TEST_EXPECT(str(jo["display_name"]) == "Joint Operations" && str(jo["title"]) == "jo" &&
			str(jo["game"]) == "jop_2_consumer" && str(jo["updater_login"]) == "JOTRus" &&
			str(jo["command_file"]) == "JOPTR.CMD" && jo["retail_updater"].b());
	TEST_EXPECT(is_null(jo["parent_id"]) && is_null(jo["current_tree"]) &&
			is_null(jo["baseline_tree_id"]) && is_null(jo["published_release"]));
	TEST_EXPECT(jo["servers_live"].i() == 0 && jo["servers_outdated"].i() == 0 &&
			jo["trees"].size() == 0);
	const auto jox = listed_data_set(h, h.jox);
	TEST_EXPECT(jox && jox["parent_id"].i() == h.jo && str(jox["expansion_code"]) == "jox01");
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
	// A duplicate login (without case) is the database's refusal.
	TEST_EXPECT(refused(
			h.admin("POST", "/api/admin/data-sets",
					R"({"display_name":"X","kind":"base","title":"jo","game":"jop_2_consumer","updater_login":"jotrus","command_file":"X.CMD"})"),
			409, "duplicate_login"));

	// A base.
	auto json = json_of(
			h.admin("POST", "/api/admin/data-sets",
					R"({"display_name":"Black Hawk Down","kind":"base","title":"dfx2","game":"dfx2_consumer","updater_login":"BHDus","command_file":"BHDPTR.CMD","retail_updater":true})"),
			201);
	TEST_EXPECT(json);
	const int64_t bhd = json["data_set"]["id"].i();
	TEST_EXPECT(bhd != 0 && str(json["data_set"]["display_name"]) == "Black Hawk Down" &&
			str(json["data_set"]["title"]) == "dfx2" &&
			str(json["data_set"]["updater_login"]) == "BHDus" &&
			json["data_set"]["retail_updater"].b() && json["data_set"]["trees"].size() == 0 &&
			is_null(json["data_set"]["current_tree"]) && json["data_set"]["servers_live"].i() == 0);
	// Its expansion, once.
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
			str(json["data_set"]["updater_login"]).empty());
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
	json = json_of(h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(bhd),
						   R"({"display_name":"BHD"})"),
			200);
	TEST_EXPECT(json && str(json["data_set"]["display_name"]) == "BHD");
	reply = h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(bhd), R"({"display_name":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "display_name");
	// The updater product: a flag, a login another holds, a command file without its login,
	// both cleared.
	json = json_of(h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(bhd),
						   R"({"retail_updater":false})"),
			200);
	TEST_EXPECT(json && !json["data_set"]["retail_updater"].b());
	TEST_EXPECT(refused(h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(bhd),
								R"({"updater_login":"JOTRUS"})"),
			409, "duplicate_login"));
	reply = h.admin(
			"PATCH", "/api/admin/data-sets/" + std::to_string(bhd), R"({"command_file":""})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "command_file");
	json = json_of(h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(bhd),
						   R"({"updater_login":"","command_file":""})"),
			200);
	TEST_EXPECT(json && str(json["data_set"]["updater_login"]).empty());
	reply = h.admin(
			"PATCH", "/api/admin/data-sets/" + std::to_string(ts), R"({"game":"dfx2_consumer"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "game");
	json = json_of(h.admin("PATCH", "/api/admin/data-sets/" + std::to_string(bhd),
						   R"({"game":"opennova_0_consumer"})"),
			200);
	TEST_EXPECT(json && str(json["data_set"]["game"]) == "opennova_0_consumer");
	TEST_EXPECT(refused(h.admin("PATCH", "/api/admin/data-sets/99999", R"({"display_name":"X"})"),
			404, "not_found"));

	// DELETE: a base with an expansion is in use; the expansion goes, then the base.
	TEST_EXPECT(refused(
			h.admin("DELETE", "/api/admin/data-sets/" + std::to_string(bhd)), 409, "in_use"));
	reply = h.admin("DELETE", "/api/admin/data-sets/" + std::to_string(ts));
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	reply = h.admin("DELETE", "/api/admin/data-sets/" + std::to_string(bhd));
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	TEST_EXPECT(refused(
			h.admin("DELETE", "/api/admin/data-sets/" + std::to_string(bhd)), 404, "not_found"));
	TEST_EXPECT(!listed_data_set(h, bhd));
	return 0;
}

// A tree of Joint Operations uploaded through the routes, each part through the part route.
int test_tree_upload(Harness &h) {
	const std::string base = "/api/admin/data-sets/" + std::to_string(h.jo);
	// The kind's files: a required one missing, a name the kind lacks.
	net::HttpReply reply = h.admin("POST", base + "/trees",
			R"({"files":[{"name":"resource.pff","size":10},{"name":"localres.pff","size":10}]})");
	TEST_EXPECT(refused(reply, 400, "invalid_files"));
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json["missing"].size() == 1 && str(json["missing"][0]) == "language.pff" &&
			json["unexpected"].size() == 0);
	reply = h.admin("POST", base + "/trees",
			R"({"files":[{"name":"resource.pff","size":10},{"name":"localres.pff","size":10},{"name":"language.pff","size":10},{"name":"readme.txt","size":1}]})");
	TEST_EXPECT(refused(reply, 400, "invalid_files"));
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json["missing"].size() == 0 && json["unexpected"].size() == 1 &&
			str(json["unexpected"][0]) == "readme.txt");
	reply = h.admin("POST", base + "/trees", R"({"files":[{"name":"resource.pff","size":0}]})");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "files");
	reply = h.admin("POST", base + "/trees", R"({"files":"resource.pff"})");
	TEST_EXPECT(refused(reply, 400, "invalid_field"));
	TEST_EXPECT(refused(h.admin("POST", "/api/admin/data-sets/99999/trees",
								R"({"files":[{"name":"resource.pff","size":1}]})"),
			404, "not_found"));

	// Begun: the names canonicalised, one upload per file, the keys under the tree, the
	// part size the site slices by.
	const auto begun =
			begin_tree(h, h.jo, { "RESOURCE.PFF", "localres.pff", "Language.pff", "version.txt" });
	TEST_EXPECT(begun && begun->id != 0 && begun->uploads.size() == 4);
	const int64_t t1 = begun->id;
	TEST_EXPECT(
			begun->uploads[0].name == "resource.pff" && begun->uploads[2].name == "language.pff");
	TEST_EXPECT(begun->uploads[0].key == "trees/" + std::to_string(t1) + "/resource.pff");
	TEST_EXPECT(nws::LocalObjectStore::is_upload_id(begun->uploads[0].upload_id));
	{
		const auto listed = listed_data_set(h, h.jo);
		TEST_EXPECT(listed && listed["trees"].size() == 1 && listed["trees"][0]["id"].i() == t1 &&
				str(listed["trees"][0]["state"]) == "uploading" &&
				str(listed["trees"][0]["source"]) == "baseline" &&
				is_null(listed["trees"][0]["release_id"]) &&
				str(listed["trees"][0]["created_by"]) == "token" &&
				str(listed["trees"][0]["ready_at"]).empty());
	}
	// One at a time.
	TEST_EXPECT(refused(
			h.admin("POST", base + "/trees",
					R"({"files":[{"name":"resource.pff","size":1},{"name":"localres.pff","size":1},{"name":"language.pff","size":1}]})"),
			409, "upload_in_flight"));

	// Each file in parts: PUT through the route, completed.
	const std::string tree = base + "/trees/" + std::to_string(t1);
	struct Content {
		std::string name;
		std::vector<std::string> parts;
	};
	const std::vector<Content> contents = {
		{ "resource.pff", { "resource part one;", "resource part two" } },
		{ "localres.pff", { "localres" } },
		{ "language.pff", { "language" } },
		{ "version.txt", { "Joint Operations: Typhoon Rising, V1.7.5.3; English" } },
	};
	for (const Content &c : contents) {
		const Upload *upload = nullptr;
		for (const Upload &u : begun->uploads)
			if (u.name == c.name)
				upload = &u;
		TEST_EXPECT(upload != nullptr);
		std::vector<crow::json::wvalue> completed;
		for (size_t i = 0; i < c.parts.size(); ++i) {
			const int number = static_cast<int>(i) + 1;
			// The part's bytes to the route: the ETag back is the local store's (the part's
			// SHA-256 hex), and the reply is not cached.
			reply = h.put_part(tree, c.name, upload->upload_id, number, c.parts[i]);
			json = json_of(reply, 200);
			TEST_EXPECT(json);
			const std::string etag = str(json["etag"]);
			TEST_EXPECT(etag.size() == 64);
			bool no_store = false;
			for (const std::string &line : reply.headers)
				no_store = no_store || line == "Cache-Control: no-store";
			TEST_EXPECT(no_store);
			crow::json::wvalue done;
			done["part_number"] = number;
			done["etag"] = etag;
			completed.push_back(std::move(done));
		}
		// A wrong ETag at completion is the store's refusal; the right ones complete it.
		crow::json::wvalue complete;
		complete["upload_id"] = upload->upload_id;
		if (c.name == "resource.pff") {
			std::vector<crow::json::wvalue> wrong;
			crow::json::wvalue w;
			w["part_number"] = 1;
			w["etag"] = "0000000000000000000000000000000000000000000000000000000000000000";
			wrong.push_back(std::move(w));
			complete["parts"] = std::move(wrong);
			TEST_EXPECT(refused(
					h.admin("POST", tree + "/files/" + c.name + "/complete", complete.dump()), 502,
					"store_error"));
		}
		complete["parts"] = std::move(completed);
		json = json_of(
				h.admin("POST", tree + "/files/" + c.name + "/complete", complete.dump()), 200);
		TEST_EXPECT(json && json["completed"].b());
		// The file is in the store, the parts joined.
		std::string stored, error, expected;
		for (const std::string &p : c.parts)
			expected += p;
		TEST_EXPECT(io::read_file_text(h.store.path_of(upload->key), stored, error) &&
				stored == expected);
	}
	// A part the route refuses: over the cap, out of range, no upload id, a name the kind
	// lacks, a tree that is not there, an upload id the store does not know.
	const std::string &upload = begun->uploads[0].upload_id;
	reply = h.put_part(tree, "resource.pff", upload, 1,
			std::string(static_cast<size_t>(hosted::DataSetRoutes::kPartSize) + 1, 'x'));
	TEST_EXPECT(refused(reply, 413, "too_large"));
	reply = h.put_part(tree, "resource.pff", upload, 0, "x");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "part_number");
	reply = h.put_part(tree, "resource.pff", upload, 10001, "x");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "part_number");
	reply = h.send("PUT", tree + "/files/resource.pff/parts/1", { kOctets, kBearer }, "x");
	TEST_EXPECT(refused(reply, 400, "invalid_field") && field_of(reply) == "upload_id");
	reply = h.put_part(tree, "readme.txt", upload, 1, "x");
	TEST_EXPECT(refused(reply, 404, "not_found"));
	reply = h.put_part(base + "/trees/99999", "resource.pff", upload, 1, "x");
	TEST_EXPECT(refused(reply, 404, "not_found"));
	reply = h.put_part(tree, "resource.pff", "00000000-0000-4000-8000-000000000000", 1, "x");
	TEST_EXPECT(refused(reply, 502, "store_error"));
	TEST_EXPECT(refused(
			h.admin("POST", base + "/trees/" + std::to_string(t1) + "/files/nope.txt/complete",
					R"({"upload_id":"x","parts":[]})"),
			400, "invalid_field"));
	TEST_EXPECT(refused(h.admin("POST", tree + "/files/resource.pff/complete",
								R"({"upload_id":")" + upload +
										R"(","parts":[{"part_number":1,"etag":""}]})"),
			400, "invalid_field"));
	// Finalize: the tree finalizing and its job queued under its dedupe key, once.
	json = json_of(h.admin("POST", tree + "/finalize"), 202);
	TEST_EXPECT(json && str(json["tree"]["state"]) == "finalizing");
	{
		auto db = h.pool.acquire();
		const auto job = queued_job(*db, "tree.finalize:" + std::to_string(t1));
		TEST_EXPECT(job && job->kind == "tree.finalize" &&
				job->payload.find("\"tree_id\": " + std::to_string(t1)) != std::string::npos);
	}
	TEST_EXPECT(refused(h.admin("POST", tree + "/finalize"), 409, "not_uploading"));
	TEST_EXPECT(refused(h.put_part(tree, "resource.pff", upload, 1, "late"), 409, "not_uploading"));
	TEST_EXPECT(refused(h.admin("DELETE", tree), 409, "in_progress"));

	// Ready through the repository (what the worker's finalize does): current and baseline.
	{
		auto db = h.pool.acquire();
		hosted::TreeReady ready;
		ready.files = { { "resource.pff", 35 }, { "localres.pff", 8 }, { "language.pff", 8 },
			{ "version.txt", 50 } };
		ready.missions = { { "a.bms", "Alpha" }, { "b.bms", "Bravo" } };
		ready.version_text = "JO V1.7.5.3";
		ready.bytes = 101;
		TEST_EXPECT(hosted::finish_tree(*db, t1, hosted::TreeState::Finalizing, ready));
	}
	{
		const auto listed = listed_data_set(h, h.jo);
		TEST_EXPECT(listed && listed["current_tree"]["id"].i() == t1 &&
				str(listed["current_tree"]["version"]) == "JO V1.7.5.3" &&
				is_db_time(str(listed["current_tree"]["ready_at"])) &&
				listed["baseline_tree_id"].i() == t1);
		TEST_EXPECT(listed["trees"].size() == 1 && str(listed["trees"][0]["state"]) == "ready" &&
				listed["trees"][0]["files"].i() == 4 && listed["trees"][0]["missions"].i() == 2 &&
				listed["trees"][0]["bytes"].i() == 101 && listed["trees"][0]["pinned_by"].i() == 0);
	}
	// What new starts take cannot go.
	TEST_EXPECT(refused(h.admin("DELETE", tree), 409, "current"));

	// An upload abandoned: aborted in the store, the row pruned, a part after it refused.
	const auto second = begin_tree(h, h.jo, { "resource.pff", "localres.pff", "language.pff" });
	TEST_EXPECT(second && second->id != 0);
	const std::string second_tree = base + "/trees/" + std::to_string(second->id);
	TEST_EXPECT(json_of(h.put_part(second_tree, "resource.pff", second->uploads[0].upload_id, 1,
							   "abandoned"),
			200));
	reply = h.admin("DELETE", second_tree);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	{
		std::string etag, error;
		TEST_EXPECT(!h.store.upload_part(second->uploads[0].key, second->uploads[0].upload_id, 2,
				"more", etag, error));
		auto db = h.pool.acquire();
		const auto t = hosted::find_tree(*db, second->id);
		TEST_EXPECT(t && t->state == hosted::TreeState::Pruned && !t->pruned_at.empty());
	}
	TEST_EXPECT(refused(h.put_part(second_tree, "resource.pff", second->uploads[0].upload_id, 2,
								"more"),
			409, "not_uploading"));
	reply = h.admin("DELETE", second_tree);
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	TEST_EXPECT(listed_data_set(h, h.jo)["trees"].size() == 1);

	// A newer baseline makes t1 deletable: pruned with a tree.prune job, its objects kept
	// for the job.
	int64_t t2 = 0;
	{
		auto db = h.pool.acquire();
		t2 = hosted::create_tree(*db, h.jo, hosted::TreeSource::Baseline, hosted::TreeState::Finalizing, std::nullopt, std::nullopt, "worker");
		hosted::TreeReady ready;
		ready.files = { { "resource.pff", 1 } };
		ready.version_text = "JO V1.7.5.4";
		ready.bytes = 1;
		TEST_EXPECT(hosted::finish_tree(*db, t2, hosted::TreeState::Finalizing, ready));
	}
	json = json_of(h.admin("DELETE", tree), 202);
	TEST_EXPECT(json && str(json["tree"]["state"]) == "pruned");
	{
		auto db = h.pool.acquire();
		const auto job = queued_job(*db, "tree.prune");
		TEST_EXPECT(job && job->kind == "tree.prune");
		const auto t = hosted::find_tree(*db, t1);
		TEST_EXPECT(t && t->state == hosted::TreeState::Pruned && t->pruned_at.empty());
		std::string stored, error;
		TEST_EXPECT(io::read_file_text(h.store.path_of(begun->uploads[1].key), stored, error) &&
				stored == "localres");
	}
	TEST_EXPECT(listed_data_set(h, h.jo)["trees"].size() == 1);

	// A tree a live server pins cannot go either, and the list counts that server as
	// outdated once a newer tree is current.
	int64_t t3 = 0, server = 0;
	{
		auto db = h.pool.acquire();
		server = insert_server(*db, h.jo, t2);
		t3 = hosted::create_tree(*db, h.jo, hosted::TreeSource::Baseline, hosted::TreeState::Finalizing, std::nullopt, std::nullopt, "worker");
		hosted::TreeReady ready;
		ready.files = { { "resource.pff", 1 } };
		ready.version_text = "JO V1.7.5.5";
		ready.bytes = 1;
		TEST_EXPECT(hosted::finish_tree(*db, t3, hosted::TreeState::Finalizing, ready));
	}
	reply = h.admin("DELETE", base + "/trees/" + std::to_string(t2));
	TEST_EXPECT(refused(reply, 409, "pinned"));
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json["servers"].i() == 1);
	{
		const auto listed = listed_data_set(h, h.jo);
		TEST_EXPECT(
				listed && listed["servers_live"].i() == 1 && listed["servers_outdated"].i() == 1);
		TEST_EXPECT(listed["trees"].size() == 2 && listed["trees"][0]["id"].i() == t3 &&
				listed["trees"][1]["id"].i() == t2 && listed["trees"][1]["pinned_by"].i() == 1);
	}
	// A data set a live server runs is in use.
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

	// Uploading: the package's upload begun under its upload key, the part size the site
	// slices by.
	auto json =
			json_of(h.admin("POST", "/api/admin/releases", R"({"name":"1.7.5.4","size":28})"), 201);
	TEST_EXPECT(json);
	const int64_t r1 = json["release"]["id"].i();
	TEST_EXPECT(r1 != 0 && str(json["release"]["status"]) == "uploading" &&
			str(json["release"]["name"]) == "1.7.5.4" && json["release"]["bytes"].i() == 28 &&
			json["release"]["sections"].size() == 0 &&
			str(json["release"]["published_at"]).empty() &&
			str(json["release"]["retired_at"]).empty());
	const std::string key = "updates/upload-" + std::to_string(r1) + ".sfs";
	TEST_EXPECT(str(json["upload"]["key"]) == key &&
			json["upload"]["part_size"].i() == hosted::DataSetRoutes::kPartSize);
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
			std::string(static_cast<size_t>(hosted::DataSetRoutes::kPartSize) + 1, 'x'));
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
	// under its sha: two sections, one per seeded JO data set.
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
				str(r["package_product"]) == "Joint Operations");
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

		Harness harness{ pool, store, http.bound_port(), 0, 0 };
		{
			auto db = pool.acquire();
			harness.jo = seeded(*db, "Joint Operations");
			harness.jox = seeded(*db, "Escalation");
			TEST_EXPECT(harness.jo != 0 && harness.jox != 0);
		}

		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "refused", test_refused },
			{ "list", test_list },
			{ "create_patch_delete", test_create_patch_delete },
			{ "tree_upload", test_tree_upload },
			{ "releases", test_releases },
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
