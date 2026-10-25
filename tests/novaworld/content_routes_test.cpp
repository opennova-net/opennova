// The content store's routes (apps/novaworld_web/content/content_routes.h, ADR 0069), driven
// in-process: a database of the run's own (migrated with the games seed, so the five seeded
// projects are there), a Redis namespace, a local object store in a temp directory, the Crow
// HttpListener on a port the OS picks, and requests as the site's sessions (an admin's and a
// player's, logged in through POST /api/login) and as the Bearer admin. The cases share the
// listener and run in order: who reads what (the public projects anyone's, the private ones an
// admin's alone, 404 to anyone else); who may push (no one signed out, no player, not the Bearer
// token, no session without the CSRF header); a push of a private retail project from nothing
// (the files it lacks, each uploaded and checked by its SHA-256, the revision, a second push on
// the same parent told the head that won, one listing the head's files refused, one naming a file
// the store lacks refused with its hash, a manifest out of its canonical form refused), its
// history, manifest and files read back; a file the store got through no route of this program (as
// an upload through the store's own URL puts one) held once a push names it; and the admin's
// projects created, refused, changed and deleted with their objects.

#include "core/local_object_store.h"
#include "data_sets/content_repository.h"
#include "http_listener.h"
#include "net_http.h"
#include "net_sockets.h"
#include "program/server_config.h"
#include "accounts/auth.h"
#include "accounts/web_session.h"

#include <base/content/content_manifest.h>
#include <base/io/file_io.h>
#include <base/io/sha256.h>
#include <net/novaworld/db/database.h>

#include <crow/json.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace content = opennova::novaworld_server::content;
namespace io = opennova::io;
using opennova::content::ContentFile;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;

namespace {

constexpr const char *kAdminToken = "content-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer content-routes-admin-token";
constexpr const char *kJson = "Content-Type: application/json";
constexpr const char *kOctets = "Content-Type: application/octet-stream";
constexpr const char *kCsrf = "X-OpenNova-Request: 1";

struct Harness {
	ConnectionPool &pool;
	nws::LocalObjectStore &store;
	uint16_t port = 0;
	std::string admin; // the admin's session cookie, "Cookie: ..."
	std::string player; // a player's
	std::string own_game; // our own game's project's key (its updater login)

	net::HttpReply send(const char *method, const std::string &path,
			const std::vector<std::string> &headers = {}, const std::string &body = {}) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
		return net::http_exchange(
				method, url, headers, body, 30000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
	}
	// The admin's session, as the editor sends a request: the cookie and the CSRF header.
	net::HttpReply as_admin(
			const char *method, const std::string &path, const std::string &body = {}) const {
		return send(method, path, { kJson, kCsrf, admin }, body);
	}
	net::HttpReply upload(
			const std::string &key, const std::string &sha, const std::string &bytes) const {
		return send("PUT", "/api/content/projects/" + key + "/blobs/" + sha,
				{ kOctets, kCsrf, admin }, bytes);
	}
};

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

std::string describe(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "no reply (" + reply.error + ")";
	return "HTTP " + std::to_string(reply.code) + " " + body_text(reply).substr(0, 600);
}

// The reply's JSON when it came with `code`; an empty value (and the reply logged) otherwise.
crow::json::rvalue json_of(const net::HttpReply &reply, int code) {
	if (!reply.transport_ok || reply.code != code) {
		std::fprintf(stderr, "  %s, want %d\n", describe(reply).c_str(), code);
		return {};
	}
	return crow::json::load(body_text(reply));
}

// Whether the reply came with `code` and the error `error` (logged otherwise).
bool refused(const net::HttpReply &reply, int code, const char *error) {
	const auto json = crow::json::load(body_text(reply));
	const std::string got =
			json && json.has("error") ? std::string(json["error"].s()) : std::string();
	if (reply.transport_ok && reply.code == code && got == error)
		return true;
	std::fprintf(stderr, "  %s, want %d %s\n", describe(reply).c_str(), code, error);
	return false;
}

std::string header_of(const net::HttpReply &reply, const char *name) {
	for (const std::string &line : reply.headers) {
		const auto colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		std::string key = line.substr(0, colon);
		for (char &c : key)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		std::string want = name;
		for (char &c : want)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (key != want)
			continue;
		size_t start = colon + 1;
		while (start < line.size() && line[start] == ' ')
			++start;
		return line.substr(start);
	}
	return {};
}

std::string sha_of(const std::string &bytes) {
	return io::sha256_hex(bytes.data(), bytes.size());
}

ContentFile file_of(const std::string &path, const std::string &bytes) {
	return { path, sha_of(bytes), bytes.size() };
}

// A push's body: {parent, message, manifest} with the manifest's text a JSON string.
std::string push_body(int64_t parent, const std::string &message, const std::string &manifest) {
	crow::json::wvalue body;
	body["parent"] = parent;
	body["message"] = message;
	body["manifest"] = manifest;
	return body.dump();
}

// The missing request's body for `files`.
std::string missing_body(const std::vector<ContentFile> &files) {
	std::vector<crow::json::wvalue> blobs;
	for (const ContentFile &file : files) {
		crow::json::wvalue blob;
		blob["sha256"] = file.sha256;
		blob["size"] = static_cast<int64_t>(file.size);
		blobs.push_back(std::move(blob));
	}
	crow::json::wvalue body;
	body["blobs"] = std::move(blobs);
	return body.dump();
}

// An account with `role`, logged in through POST /api/login: its "Cookie: ..." header, "" when
// the login failed.
std::string signed_in(Harness &h, const std::string &username, const char *role) {
	static int next = 0;
	char pcid[16];
	std::snprintf(pcid, sizeof(pcid), "%08x", 0xc0de0000u + static_cast<unsigned>(next++));
	nws::CreateUserParams p;
	p.username = username;
	p.password = "content-routes-pw1";
	p.pcid = pcid;
	p.nwh = "1";
	p.nwhandle = username;
	const auto made = nws::create_user(*h.pool.acquire(), p);
	if (!made.ok)
		return {};
	h.pool.acquire()->exec(
			"UPDATE players SET role = $1 WHERE id = $2;", { BindValue(role), BindValue(made.id) });
	const auto reply = h.send("POST", "/api/login", { kJson, kCsrf },
			"{\"username\":\"" + username + "\",\"password\":\"content-routes-pw1\"}");
	if (!reply.transport_ok || reply.code != 200)
		return {};
	const std::string prefix = std::string(nws::kWebSessionCookie) + "=";
	for (const std::string &line : reply.headers) {
		const auto at = line.find(prefix);
		if (at == std::string::npos)
			continue;
		const auto value = at + prefix.size();
		return "Cookie: " + prefix + line.substr(value, line.find(';', value) - value);
	}
	return {};
}

// The keys a project list holds.
std::vector<std::string> keys_of(const crow::json::rvalue &json) {
	std::vector<std::string> keys;
	if (json && json.has("projects"))
		for (const auto &p : json["projects"])
			keys.emplace_back(p["key"].s());
	return keys;
}

int test_who_reads_what(Harness &h) {
	// Anyone, and a player: our own two projects, which no one but an admin may push.
	for (const std::vector<std::string> &headers :
			{ std::vector<std::string>{}, std::vector<std::string>{ h.player } }) {
		const auto json = json_of(h.send("GET", "/api/content/projects", headers), 200);
		const auto keys = keys_of(json);
		TEST_EXPECT(keys.size() == 2);
		for (const auto &p : json["projects"])
			TEST_EXPECT(std::string(p["visibility"].s()) == "public" && !p["can_push"].b() &&
					!p.has("storage"));
	}
	// An admin, by session or by token: every project, its storage beside it.
	for (const std::vector<std::string> &headers :
			{ std::vector<std::string>{ h.admin }, std::vector<std::string>{ kBearer } }) {
		const auto json = json_of(h.send("GET", "/api/content/projects", headers), 200);
		const auto keys = keys_of(json);
		const auto holds = [&keys](const char *key) {
			return std::find(keys.begin(), keys.end(), key) != keys.end();
		};
		TEST_EXPECT(keys.size() == 5 && holds("DFX2") && holds("JO") && holds("JOX01"));
		for (const auto &p : json["projects"])
			TEST_EXPECT(p["can_push"].b() && p.has("storage") && p["head"].i() == 0);
	}
	const auto jox01 = json_of(h.send("GET", "/api/content/projects/JOX01", { h.admin }), 200);
	TEST_EXPECT(jox01 && std::string(jox01["project"]["base"].s()) == "JO" &&
			jox01["project"]["retail"].b() && jox01["head_revision"].t() == crow::json::type::Null);
	const auto own = json_of(h.send("GET", "/api/content/projects/" + h.own_game), 200);
	TEST_EXPECT(own && own["project"]["base"].t() == crow::json::type::Null);
	// A private project is no one's but an admin's: the same 404 as no project.
	TEST_EXPECT(refused(h.send("GET", "/api/content/projects/JO"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/content/projects/JO", { h.player }), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/content/projects/JO/revisions"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/content/projects/NOPE"), 404, "not_found"));
	return 0;
}

int test_who_pushes(Harness &h) {
	const std::string body = missing_body({ file_of("a.def", "alpha") });
	const std::string path = "/api/content/projects/" + h.own_game + "/missing";
	TEST_EXPECT(refused(h.send("POST", path, { kJson, kCsrf }, body), 401, "unauthorized"));
	TEST_EXPECT(refused(h.send("POST", path, { kJson, kCsrf, h.player }, body), 403, "forbidden"));
	TEST_EXPECT(refused(h.send("POST", path, { kJson, kBearer }, body), 403, "session_required"));
	TEST_EXPECT(
			refused(h.send("POST", path, { kJson, h.admin }, body), 403, "csrf_header_required"));
	TEST_EXPECT(refused(h.send("POST", "/api/content/projects/" + h.own_game + "/revisions",
								{ kJson, kBearer }, push_body(0, "x", "{}")),
			403, "session_required"));
	TEST_EXPECT(
			refused(h.send("PUT", "/api/content/projects/" + h.own_game + "/blobs/" + sha_of("x"),
							{ kOctets, kBearer }, "x"),
					403, "session_required"));
	return 0;
}

int test_push(Harness &h) {
	const std::string alpha = "alpha bytes", bravo(70000, 'b');
	const std::vector<ContentFile> files = { file_of("defs/a.def", alpha),
		file_of("art/b.tga", bravo) };
	const std::string manifest = opennova::content::encode_manifest(files);

	// The files the store lacks, each with its upload.
	auto json = json_of(
			h.as_admin("POST", "/api/content/projects/JO/missing", missing_body(files)), 200);
	TEST_EXPECT(json && json["missing"].size() == 2);
	for (const auto &m : json["missing"])
		TEST_EXPECT(std::string(m["upload"]["method"].s()) == "PUT" &&
				std::string(m["upload"]["url"].s()) ==
						"/api/content/projects/JO/blobs/" + std::string(m["sha256"].s()));
	TEST_EXPECT(refused(h.as_admin("POST", "/api/content/projects/JO/missing",
								"{\"blobs\": [{\"sha256\": \"xyz\", \"size\": 1}]}"),
			400, "invalid_field"));
	TEST_EXPECT(refused(h.upload("JO", sha_of("other"), alpha), 400, "sha256_mismatch"));
	TEST_EXPECT(refused(h.upload("JO", "NOT-A-SHA", alpha), 400, "invalid_sha256"));
	TEST_EXPECT(json_of(h.upload("JO", files[0].sha256, alpha), 200));
	TEST_EXPECT(json_of(h.upload("JO", files[1].sha256, bravo), 200));
	json = json_of(
			h.as_admin("POST", "/api/content/projects/JO/missing", missing_body(files)), 200);
	TEST_EXPECT(json && json["missing"].size() == 0);

	// The first revision.
	json = json_of(h.as_admin("POST", "/api/content/projects/JO/revisions",
						   push_body(0, "Stock import", manifest)),
			201);
	TEST_EXPECT(json && json["head"].i() == 1 && json["revision"]["number"].i() == 1 &&
			std::string(json["revision"]["author"].s()) == "content_admin" &&
			json["revision"]["file_count"].i() == 2 &&
			json["revision"]["bytes"].i() == static_cast<int64_t>(alpha.size() + bravo.size()) &&
			std::string(json["revision"]["manifest_sha256"].s()) ==
					opennova::content::manifest_sha256(manifest));

	// A second push on the same parent: told the head that won.
	const auto stale = h.as_admin(
			"POST", "/api/content/projects/JO/revisions", push_body(0, "Also first", manifest));
	TEST_EXPECT(
			refused(stale, 409, "stale") && crow::json::load(body_text(stale))["head"].i() == 1);
	// The head's own files again; a file the store lacks; a manifest out of its form.
	TEST_EXPECT(refused(h.as_admin("POST", "/api/content/projects/JO/revisions",
								push_body(1, "Again", manifest)),
			400, "unchanged"));
	const std::vector<ContentFile> more = { files[0], files[1],
		file_of("missions/m.bms", "never sent") };
	const auto lacking = h.as_admin("POST", "/api/content/projects/JO/revisions",
			push_body(1, "More", opennova::content::encode_manifest(more)));
	TEST_EXPECT(refused(lacking, 400, "missing_blobs"));
	// The reply kept whole: a member read off a temporary document outlives what it points into.
	const auto lacked = crow::json::load(body_text(lacking));
	TEST_EXPECT(lacked && lacked["hashes"].size() == 1 &&
			std::string(lacked["hashes"][0].s()) == more[2].sha256);
	std::string spaced = manifest;
	spaced.insert(spaced.find('\n'), " ");
	TEST_EXPECT(refused(h.as_admin("POST", "/api/content/projects/JO/revisions",
								push_body(1, "Spaced", spaced)),
			400, "invalid_manifest"));
	TEST_EXPECT(refused(h.as_admin("POST", "/api/content/projects/JO/revisions",
								push_body(1, std::string("bad\x01", 4), manifest)),
			400, "invalid_field"));

	// Read back: the history, the manifest's very text, the files.
	json = json_of(h.send("GET", "/api/content/projects/JO/revisions", { h.admin }), 200);
	TEST_EXPECT(json && json["revisions"].size() == 1 &&
			std::string(json["revisions"][0]["message"].s()) == "Stock import");
	const auto text = h.send("GET", "/api/content/projects/JO/revisions/1/manifest", { h.admin });
	TEST_EXPECT(text.code == 200 && body_text(text) == manifest &&
			header_of(text, "Cache-Control").rfind("private", 0) == 0);
	TEST_EXPECT(refused(h.send("GET", "/api/content/projects/JO/revisions/2/manifest", { h.admin }),
			404, "not_found"));
	const auto got =
			h.send("GET", "/api/content/projects/JO/blobs/" + files[1].sha256, { h.admin });
	TEST_EXPECT(got.code == 200 && body_text(got) == bravo);
	TEST_EXPECT(refused(
			h.send("GET", "/api/content/projects/JO/blobs/" + files[1].sha256), 404, "not_found"));
	TEST_EXPECT(
			refused(h.send("GET", "/api/content/projects/JO/blobs/" + sha_of("none"), { h.admin }),
					404, "not_found"));
	json = json_of(h.send("GET", "/api/content/projects/JO", { h.admin }), 200);
	TEST_EXPECT(json && json["project"]["head"].i() == 1 &&
			json["head_revision"]["number"].i() == 1 &&
			json["project"]["storage"]["files"].i() == 2);

	// A file the store got through no route here, as an upload through its own URL puts one: held
	// once a push names it at its size.
	const std::string charlie = "charlie";
	{
		test_temp::TempDir scratch("content_routes_direct");
		const std::string path = scratch.file("c.bin");
		std::string error;
		TEST_EXPECT(io::write_file_atomic(path, charlie, error));
		TEST_EXPECT(h.store.put_file(content::blob_key("JO", sha_of(charlie)), path, error));
	}
	const std::vector<ContentFile> direct = { files[0], file_of("defs/c.def", charlie) };
	json = json_of(
			h.as_admin("POST", "/api/content/projects/JO/revisions",
					push_body(1, "Direct upload", opennova::content::encode_manifest(direct))),
			201);
	TEST_EXPECT(json && json["head"].i() == 2);
	json = json_of(
			h.as_admin("POST", "/api/content/projects/JO/missing", missing_body(direct)), 200);
	TEST_EXPECT(json && json["missing"].size() == 0);
	json = json_of(h.send("GET", "/api/content/projects/JO/revisions?limit=1", { h.admin }), 200);
	TEST_EXPECT(json && json["revisions"].size() == 1 && json["revisions"][0]["number"].i() == 2);
	json = json_of(h.send("GET", "/api/content/projects/JO/revisions?before=2", { h.admin }), 200);
	TEST_EXPECT(json && json["revisions"].size() == 1 && json["revisions"][0]["number"].i() == 1);
	TEST_EXPECT(refused(h.send("GET", "/api/content/projects/JO/revisions?limit=0", { h.admin }),
			400, "invalid_query"));

	// More such files than a push asks the store of one by one: held by one listing of the store.
	std::vector<ContentFile> many = { files[0] };
	{
		test_temp::TempDir scratch("content_routes_many");
		for (int i = 0; i < 6; ++i) {
			const std::string bytes = "many-" + std::to_string(i);
			const std::string path = scratch.file(("m" + std::to_string(i)).c_str());
			std::string error;
			TEST_EXPECT(io::write_file_atomic(path, bytes, error));
			TEST_EXPECT(h.store.put_file(content::blob_key("JO", sha_of(bytes)), path, error));
			many.push_back(file_of("many/" + std::to_string(i) + ".def", bytes));
		}
	}
	json = json_of(h.as_admin("POST", "/api/content/projects/JO/revisions",
						   push_body(2, "Many", opennova::content::encode_manifest(many))),
			201);
	TEST_EXPECT(json && json["head"].i() == 3);

	// Where each file is downloaded from: this program's own blob route (a directory's store signs
	// no URLs), an admin's alone for a private project.
	const std::string hashes =
			"{\"hashes\": [\"" + files[0].sha256 + "\", \"" + files[1].sha256 + "\"]}";
	json = json_of(h.as_admin("POST", "/api/content/projects/JO/downloads", hashes), 200);
	TEST_EXPECT(json && json["downloads"].size() == 2 &&
			std::string(json["downloads"][0]["url"].s()) ==
					"/api/content/projects/JO/blobs/" + files[0].sha256 &&
			std::string(json["downloads"][0]["method"].s()) == "GET");
	TEST_EXPECT(
			refused(h.send("POST", "/api/content/projects/JO/downloads", { kJson, kCsrf }, hashes),
					404, "not_found"));
	TEST_EXPECT(refused(h.as_admin("POST", "/api/content/projects/JO/downloads",
								"{\"hashes\": [\"not-a-sha\"]}"),
			400, "invalid_field"));
	return 0;
}

int test_admin_projects(Harness &h) {
	const auto admin = [&](const char *method, const std::string &path,
							   const std::string &body = {}) {
		return h.send(method, path, { kJson, kBearer }, body);
	};
	auto json =
			json_of(admin("POST", "/api/admin/content/projects",
							"{\"key\": \"MOD\", \"title\": \"A mod\", \"visibility\": \"private\", "
							"\"base\": \"JO\"}"),
					201);
	TEST_EXPECT(json && std::string(json["project"]["base"].s()) == "JO" &&
			std::string(json["project"]["visibility"].s()) == "private");
	TEST_EXPECT(
			refused(admin("POST", "/api/admin/content/projects",
							"{\"key\": \"MOD\", \"title\": \"Again\", \"visibility\": \"public\"}"),
					409, "duplicate_key"));
	TEST_EXPECT(
			refused(admin("POST", "/api/admin/content/projects",
							"{\"key\": \"lower\", \"title\": \"x\", \"visibility\": \"public\"}"),
					400, "invalid_field"));
	TEST_EXPECT(refused(admin("POST", "/api/admin/content/projects",
								"{\"key\": \"X\", \"title\": \"x\", \"visibility\": \"secret\"}"),
			400, "invalid_field"));
	TEST_EXPECT(refused(admin("POST", "/api/admin/content/projects",
								"{\"key\": \"X\", \"title\": \"x\", \"visibility\": \"public\", "
								"\"base\": \"NOPE\"}"),
			400, "no_such_base"));
	TEST_EXPECT(refused(admin("POST", "/api/admin/content/projects", "{\"key\": \"X\"}"), 400,
			"missing_field"));
	TEST_EXPECT(refused(h.send("POST", "/api/admin/content/projects", { kJson, kCsrf, h.player },
								"{\"key\": \"P\", \"title\": \"x\", \"visibility\": \"public\"}"),
			403, "forbidden"));

	// Made public, then read by anyone.
	TEST_EXPECT(refused(h.send("GET", "/api/content/projects/MOD"), 404, "not_found"));
	json = json_of(admin("PATCH", "/api/admin/content/projects/MOD",
						   "{\"visibility\": \"public\", \"title\": \"Renamed\"}"),
			200);
	TEST_EXPECT(json && std::string(json["project"]["title"].s()) == "Renamed");
	TEST_EXPECT(json_of(h.send("GET", "/api/content/projects/MOD"), 200));
	TEST_EXPECT(refused(admin("PATCH", "/api/admin/content/projects/NOPE", "{\"title\": \"x\"}"),
			404, "not_found"));

	// A project an expansion builds on stays; another goes with its objects.
	TEST_EXPECT(json_of(h.upload("MOD", sha_of("mod file"), "mod file"), 200));
	bool exists = false;
	uint64_t size = 0;
	std::string error;
	TEST_EXPECT(h.store.head(content::blob_key("MOD", sha_of("mod file")), exists, size, error) &&
			exists);
	TEST_EXPECT(refused(admin("DELETE", "/api/admin/content/projects/JO"), 409, "has_expansion"));
	const auto gone = admin("DELETE", "/api/admin/content/projects/MOD");
	TEST_EXPECT(gone.transport_ok && gone.code == 204);
	TEST_EXPECT(h.store.head(content::blob_key("MOD", sha_of("mod file")), exists, size, error) &&
			!exists);
	TEST_EXPECT(refused(admin("DELETE", "/api/admin/content/projects/MOD"), 404, "not_found"));
	return 0;
}

// A release built from a revision (data_sets/release_build.h): refused with no version, no data
// set or no such revision, a player refused; made building with its build queued, and kept from
// deletion while the data program is on it.
int test_release_route(Harness &h) {
	const auto admin = [&](const char *method, const std::string &path,
							   const std::string &body = {}) {
		return h.send(method, path, { kJson, kBearer }, body);
	};
	const std::string jo = "/api/admin/content/projects/JO/releases";
	TEST_EXPECT(refused(admin("POST", jo, "{\"version\": \"1.x\"}"), 400, "invalid_field"));
	TEST_EXPECT(refused(admin("POST", jo, "{}"), 400, "invalid_field"));
	TEST_EXPECT(refused(admin("POST", jo, "{\"version\": \"1.0\", \"revision\": 99}"), 400,
			"no_revision"));
	TEST_EXPECT(refused(admin("POST", "/api/admin/content/projects/JOX01/releases",
								"{\"version\": \"1.0\"}"),
			400, "no_revision"));
	TEST_EXPECT(json_of(admin("POST", "/api/admin/content/projects",
								"{\"key\": \"NOSET\", \"title\": \"x\", \"visibility\": \"public\"}"),
			201));
	TEST_EXPECT(refused(admin("POST", "/api/admin/content/projects/NOSET/releases",
								"{\"version\": \"1.0\"}"),
			400, "no_data_set"));
	TEST_EXPECT(refused(admin("POST", "/api/admin/content/projects/NOPE/releases",
								"{\"version\": \"1.0\"}"),
			404, "not_found"));
	TEST_EXPECT(refused(h.send("POST", jo, { kJson, kCsrf, h.player }, "{\"version\": \"1.0\"}"),
			403, "forbidden"));

	const auto head = json_of(h.send("GET", "/api/content/projects/JO", { h.admin }), 200);
	TEST_EXPECT(head && head["project"]["head"].i() >= 1);
	const int64_t number = head ? head["project"]["head"].i() : 0;
	const auto made = json_of(admin("POST", jo, "{\"version\": \"1.0.1\"}"), 201);
	TEST_EXPECT(made && std::string(made["release"]["status"].s()) == "building" &&
			std::string(made["release"]["source"]["project"].s()) == "JO" &&
			made["release"]["source"]["revision"].i() == number &&
			std::string(made["release"]["version"].s()) == "1.0.1" &&
			std::string(made["release"]["name"].s()) == "JO 1.0.1");
	const int64_t id = made ? made["release"]["id"].i() : 0;
	const auto queued = h.pool.acquire()->query(
			"SELECT count(*) FROM jobs WHERE kind = 'release.build' AND "
			"(payload->>'release_id')::bigint = $1;",
			{ opennova::db::BindValue(id) });
	TEST_EXPECT(!queued.empty() && queued.front().as_int(0).value_or(0) == 1);
	TEST_EXPECT(
			refused(admin("DELETE", "/api/admin/releases/" + std::to_string(id)), 409, "building"));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("content_routes");
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

		Harness harness{ pool, store, http.bound_port() };
		harness.admin = signed_in(harness, "content_admin", "admin");
		harness.player = signed_in(harness, "content_player", "player");
		TEST_EXPECT(!harness.admin.empty() && !harness.player.empty());
		{
			const auto rows = pool.acquire()->query("SELECT updater_login FROM data_sets WHERE "
													"kind = 'base' AND updater_login <> '' "
													"AND NOT retail_updater;");
			TEST_EXPECT(rows.size() == 1);
			harness.own_game = rows.front().as_text(0).value_or("");
		}

		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "who_reads_what", test_who_reads_what },
			{ "who_pushes", test_who_pushes },
			{ "push", test_push },
			{ "admin_projects", test_admin_projects },
			{ "release_route", test_release_route },
		};
		rc = 0;
		for (const Case &c : cases) {
			const int result = c.fn(harness);
			std::printf("content_routes: %s %s\n", c.name, result == 0 ? "ok" : "FAILED");
			rc |= result;
		}
		http.stop();
	}
	net::shutdown();
	return rc;
}
