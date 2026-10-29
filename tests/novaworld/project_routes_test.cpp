// The project routes (apps/novaworld_web/products/project_routes.h, ADR 0069), driven
// in-process: a database of the run's own (migrated, so the lobbies and the five seeded
// games' projects are there), a Redis namespace, a local object store in a temp directory, the Crow
// HttpListener on a port the OS picks, and requests as the site's sessions (an admin's and a
// player's, logged in through POST /api/login) and as the Bearer admin. The cases share the
// listener and run in order: who reads what (the public projects anyone's, the private ones an
// admin's alone, 404 to anyone else; each reply's kind, game, owner and what the caller may do);
// who may push (no one signed out, no player, not the Bearer token, no session without the CSRF
// header); a push of a private retail project from nothing
// (the files it lacks, each uploaded and checked by its SHA-256, the revision, a second push on
// the same parent told the head that won, one listing the head's files refused, one naming a file
// the store lacks refused with its hash, a manifest out of its canonical form refused), its
// history, manifest and files read back; a file the store got through no route of this program (as
// an upload through the store's own URL puts one) held once a push names it; a file past the blob
// route's cap (the portal's /api/ body cap, web/nginx.conf's, which the test reads) refused 413;
// a game's project made, refused, made public and renamed (a row change alone: the store names its
// objects by the game's id, so a file uploaded under the old key serves a push under the new one)
// and deleted, the route answering at once with its objects' product.purge queued, through the
// games' admin routes (products/product_routes.h); a release built from a revision; an account's
// own projects: made (every refusal, the key's rules, its updater product and download, the rate
// limit, the cap), read, changed and pushed by the one rule (the matrix of no one, another
// player, the owner, an admin's session and the token over a private and a public one), lists of
// each caller's, the quota on their stores (an admin's push counted against the owner, a file the
// store got through its own URL counted before it is held, one of another size removed), the
// account's own page, a delete with its purge queued, and the account's delete taking them,
// refused while an expansion of the service's builds on one; and an account's own releases
// (ADR 0074): built, listed and published with its previously selected lobby,
// retired and deleted by their owner, one at a time, five a day and none past the quota (an
// admin exempt), and the mods a game's players may install.

#include "accounts/auth.h"
#include "accounts/web_session.h"
#include "core/local_object_store.h"
#include "http_listener.h"
#include "net_http.h"
#include "net_sockets.h"
#include "products/product_jobs.h"
#include "products/project_repository.h"
#include "products/project_routes.h"
#include "products/project_urls.h"
#include "program/server_config.h"

#include <base/io/base64.h>
#include <base/io/file_io.h>
#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <net/novaworld/db/database.h>

#include <crow/json.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include "products/product_repository.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
namespace projects = opennova::novaworld_server::projects;
namespace hosted = opennova::novaworld_server::hosted;
namespace io = opennova::io;
using opennova::project::ProjectFile;
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
	int64_t jo = 0; // the seeded Joint Operations game's id

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
		return send("PUT", "/api/products/" + key + "/blobs/" + sha,
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

ProjectFile file_of(const std::string &path, const std::string &bytes) {
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
std::string missing_body(const std::vector<ProjectFile> &files) {
	std::vector<crow::json::wvalue> blobs;
	for (const ProjectFile &file : files) {
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
	// Anyone, and a player: our own two projects, which no one but an admin may push or change.
	for (const std::vector<std::string> &headers :
			{ std::vector<std::string>{}, std::vector<std::string>{ h.player } }) {
		const auto json = json_of(h.send("GET", "/api/products", headers), 200);
		const auto keys = keys_of(json);
		TEST_EXPECT(keys.size() == 2);
		for (const auto &p : json["projects"])
			TEST_EXPECT(std::string(p["visibility"].s()) == "public" && !p["can_push"].b() &&
					!p["can_manage"].b() && !p.has("storage") &&
					p["owner"].t() == crow::json::type::Null);
	}
	// An admin, by session or by token: every project, its storage beside it; the token changes
	// each and pushes none (it names no author).
	for (const bool session : { true, false }) {
		const auto json =
				json_of(h.send("GET", "/api/products", { session ? h.admin : kBearer }), 200);
		const auto keys = keys_of(json);
		const auto holds = [&keys](const char *key) {
			return std::find(keys.begin(), keys.end(), key) != keys.end();
		};
		TEST_EXPECT(keys.size() == 5 && holds("DFX2") && holds("JO") && holds("JOX01"));
		for (const auto &p : json["projects"])
			TEST_EXPECT(p["can_push"].b() == session && p["can_manage"].b() && p.has("storage") &&
					p["head"].i() == 0);
	}
	// An app's key names no project, even to an admin.
	TEST_EXPECT(refused(h.send("GET", "/api/products/ONRUNTIME", { h.admin }), 404, "not_found"));
	const auto jox01 = json_of(h.send("GET", "/api/products/JOX01", { h.admin }), 200);
	TEST_EXPECT(jox01 && std::string(jox01["project"]["base"].s()) == "JO" &&
			jox01["project"]["retail"].b() && jox01["head_revision"].t() == crow::json::type::Null);
	TEST_EXPECT(jox01 && std::string(jox01["project"]["kind"].s()) == "expansion" &&
			std::string(jox01["project"]["game"].s()) == "jo" &&
			std::string(jox01["project"]["protocol"]["code"].s()) == "jo" &&
			std::string(jox01["project"]["protocol"]["name"].s()) == "JO" &&
			std::string(jox01["project"]["protocol"]["version"].s()) == "16" &&
			std::string(jox01["project"]["expansion_code"].s()) == "jox01" &&
			jox01["project"]["owner"].t() == crow::json::type::Null);
	// Escalation's version.txt, its row's retail line (0008_expansion_version_file.sql) with nothing
	// published: retail's 49 bytes, whose CRC-32/MPEG-2 a retail Escalation host compares.
	std::vector<uint8_t> version;
	TEST_EXPECT(jox01 && opennova::io::base64_decode_strict(
								 std::string(jox01["project"]["version_file"].s()), version) &&
			std::string(version.begin(), version.end()) ==
					"Joint Operations: Escalation, V1.3.0.0; English\r\n");
	const auto own = json_of(h.send("GET", "/api/products/" + h.own_game), 200);
	// Our own game runs as OP: Acid Gambit, which speaks Joint Operations' protocol.
	TEST_EXPECT(own && own["project"]["version_file"].t() == crow::json::type::Null);
	TEST_EXPECT(own && own["project"]["base"].t() == crow::json::type::Null &&
			std::string(own["project"]["kind"].s()) == "game" &&
			std::string(own["project"]["game"].s()) == "acidgambit" &&
			std::string(own["project"]["protocol"]["code"].s()) == "jo" &&
			std::string(own["project"]["expansion_code"].s()).empty());
	// A private project is no one's but an admin's: the same 404 as no project.
	TEST_EXPECT(refused(h.send("GET", "/api/products/JO"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/products/JO", { h.player }), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/products/JO/revisions"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/products/NOPE"), 404, "not_found"));
	return 0;
}

int test_who_pushes(Harness &h) {
	const std::string body = missing_body({ file_of("a.def", "alpha") });
	const std::string path = "/api/products/" + h.own_game + "/missing";
	TEST_EXPECT(refused(h.send("POST", path, { kJson, kCsrf }, body), 401, "unauthorized"));
	TEST_EXPECT(refused(h.send("POST", path, { kJson, kCsrf, h.player }, body), 403, "forbidden"));
	TEST_EXPECT(refused(h.send("POST", path, { kJson, kBearer }, body), 403, "session_required"));
	TEST_EXPECT(
			refused(h.send("POST", path, { kJson, h.admin }, body), 403, "csrf_header_required"));
	TEST_EXPECT(refused(h.send("POST", "/api/products/" + h.own_game + "/revisions",
								{ kJson, kBearer }, push_body(0, "x", "{}")),
			403, "session_required"));
	TEST_EXPECT(
			refused(h.send("PUT", "/api/products/" + h.own_game + "/blobs/" + sha_of("x"),
							{ kOctets, kBearer }, "x"),
					403, "session_required"));
	return 0;
}

int test_push(Harness &h) {
	const std::string alpha = "alpha bytes", bravo(70000, 'b');
	const std::vector<ProjectFile> files = { file_of("defs/a.def", alpha),
		file_of("art/b.tga", bravo) };
	const std::string manifest = opennova::project::encode_manifest(files);

	// The files the store lacks, each with its upload.
	auto json = json_of(
			h.as_admin("POST", "/api/products/JO/missing", missing_body(files)), 200);
	TEST_EXPECT(json && json["missing"].size() == 2);
	for (const auto &m : json["missing"])
		TEST_EXPECT(std::string(m["upload"]["method"].s()) == "PUT" &&
				std::string(m["upload"]["url"].s()) ==
						"/api/products/JO/blobs/" + std::string(m["sha256"].s()));
	TEST_EXPECT(refused(h.as_admin("POST", "/api/products/JO/missing",
								"{\"blobs\": [{\"sha256\": \"xyz\", \"size\": 1}]}"),
			400, "invalid_field"));
	TEST_EXPECT(refused(h.upload("JO", sha_of("other"), alpha), 400, "sha256_mismatch"));
	TEST_EXPECT(refused(h.upload("JO", "NOT-A-SHA", alpha), 400, "invalid_sha256"));
	TEST_EXPECT(json_of(h.upload("JO", files[0].sha256, alpha), 200));
	TEST_EXPECT(json_of(h.upload("JO", files[1].sha256, bravo), 200));
	json = json_of(
			h.as_admin("POST", "/api/products/JO/missing", missing_body(files)), 200);
	TEST_EXPECT(json && json["missing"].size() == 0);

	// The first revision.
	json = json_of(h.as_admin("POST", "/api/products/JO/revisions",
						   push_body(0, "Stock import", manifest)),
			201);
	TEST_EXPECT(json && json["head"].i() == 1 && json["revision"]["number"].i() == 1 &&
			std::string(json["revision"]["author"].s()) == "content_admin" &&
			json["revision"]["file_count"].i() == 2 &&
			json["revision"]["bytes"].i() == static_cast<int64_t>(alpha.size() + bravo.size()) &&
			std::string(json["revision"]["manifest_sha256"].s()) ==
					opennova::project::manifest_sha256(manifest));

	// A second push on the same parent: told the head that won.
	const auto stale = h.as_admin(
			"POST", "/api/products/JO/revisions", push_body(0, "Also first", manifest));
	TEST_EXPECT(
			refused(stale, 409, "stale") && crow::json::load(body_text(stale))["head"].i() == 1);
	// The head's own files again; a file the store lacks; a manifest out of its form.
	TEST_EXPECT(refused(h.as_admin("POST", "/api/products/JO/revisions",
								push_body(1, "Again", manifest)),
			400, "unchanged"));
	const std::vector<ProjectFile> more = { files[0], files[1],
		file_of("missions/m.bms", "never sent") };
	const auto lacking = h.as_admin("POST", "/api/products/JO/revisions",
			push_body(1, "More", opennova::project::encode_manifest(more)));
	TEST_EXPECT(refused(lacking, 400, "missing_blobs"));
	// The reply kept whole: a member read off a temporary document outlives what it points into.
	const auto lacked = crow::json::load(body_text(lacking));
	TEST_EXPECT(lacked && lacked["hashes"].size() == 1 &&
			std::string(lacked["hashes"][0].s()) == more[2].sha256);
	std::string spaced = manifest;
	spaced.insert(spaced.find('\n'), " ");
	TEST_EXPECT(refused(h.as_admin("POST", "/api/products/JO/revisions",
								push_body(1, "Spaced", spaced)),
			400, "invalid_manifest"));
	TEST_EXPECT(refused(h.as_admin("POST", "/api/products/JO/revisions",
								push_body(1, std::string("bad\x01", 4), manifest)),
			400, "invalid_field"));

	// Read back: the history, the manifest's very text, the files.
	json = json_of(h.send("GET", "/api/products/JO/revisions", { h.admin }), 200);
	TEST_EXPECT(json && json["revisions"].size() == 1 &&
			std::string(json["revisions"][0]["message"].s()) == "Stock import");
	const auto text = h.send("GET", "/api/products/JO/revisions/1/manifest", { h.admin });
	TEST_EXPECT(text.code == 200 && body_text(text) == manifest &&
			header_of(text, "Cache-Control").rfind("private", 0) == 0);
	TEST_EXPECT(refused(h.send("GET", "/api/products/JO/revisions/2/manifest", { h.admin }),
			404, "not_found"));
	const auto got =
			h.send("GET", "/api/products/JO/blobs/" + files[1].sha256, { h.admin });
	TEST_EXPECT(got.code == 200 && body_text(got) == bravo);
	TEST_EXPECT(refused(
			h.send("GET", "/api/products/JO/blobs/" + files[1].sha256), 404, "not_found"));
	TEST_EXPECT(
			refused(h.send("GET", "/api/products/JO/blobs/" + sha_of("none"), { h.admin }),
					404, "not_found"));
	json = json_of(h.send("GET", "/api/products/JO", { h.admin }), 200);
	TEST_EXPECT(json && json["project"]["head"].i() == 1 &&
			json["head_revision"]["number"].i() == 1 &&
			json["project"]["storage"]["files"].i() == 2);

	// A file the store got through no route here, as an upload through its own URL puts one: held
	// once a push names it at its size.
	const std::string charlie = "charlie";
	{
		test_temp::TempDir scratch("project_routes_direct");
		const std::string path = scratch.file("c.bin");
		std::string error;
		TEST_EXPECT(io::write_file_atomic(path, charlie, error));
		TEST_EXPECT(h.store.put_file(projects::blob_key(h.jo, sha_of(charlie)), path, error));
	}
	const std::vector<ProjectFile> direct = { files[0], file_of("defs/c.def", charlie) };
	json = json_of(
			h.as_admin("POST", "/api/products/JO/revisions",
					push_body(1, "Direct upload", opennova::project::encode_manifest(direct))),
			201);
	TEST_EXPECT(json && json["head"].i() == 2);
	json = json_of(
			h.as_admin("POST", "/api/products/JO/missing", missing_body(direct)), 200);
	TEST_EXPECT(json && json["missing"].size() == 0);
	json = json_of(h.send("GET", "/api/products/JO/revisions?limit=1", { h.admin }), 200);
	TEST_EXPECT(json && json["revisions"].size() == 1 && json["revisions"][0]["number"].i() == 2);
	json = json_of(h.send("GET", "/api/products/JO/revisions?before=2", { h.admin }), 200);
	TEST_EXPECT(json && json["revisions"].size() == 1 && json["revisions"][0]["number"].i() == 1);
	TEST_EXPECT(refused(h.send("GET", "/api/products/JO/revisions?limit=0", { h.admin }),
			400, "invalid_query"));

	// More such files than a push asks the store of one by one: held by one listing of the store.
	std::vector<ProjectFile> many = { files[0] };
	{
		test_temp::TempDir scratch("project_routes_many");
		for (int i = 0; i < 6; ++i) {
			const std::string bytes = "many-" + std::to_string(i);
			const std::string path = scratch.file(("m" + std::to_string(i)).c_str());
			std::string error;
			TEST_EXPECT(io::write_file_atomic(path, bytes, error));
			TEST_EXPECT(h.store.put_file(projects::blob_key(h.jo, sha_of(bytes)), path, error));
			many.push_back(file_of("many/" + std::to_string(i) + ".def", bytes));
		}
	}
	json = json_of(h.as_admin("POST", "/api/products/JO/revisions",
						   push_body(2, "Many", opennova::project::encode_manifest(many))),
			201);
	TEST_EXPECT(json && json["head"].i() == 3);

	// Where each file is downloaded from: this program's own blob route (a directory's store signs
	// no URLs), an admin's alone for a private project.
	const std::string hashes =
			"{\"hashes\": [\"" + files[0].sha256 + "\", \"" + files[1].sha256 + "\"]}";
	json = json_of(h.as_admin("POST", "/api/products/JO/downloads", hashes), 200);
	TEST_EXPECT(json && json["downloads"].size() == 2 &&
			std::string(json["downloads"][0]["url"].s()) ==
					"/api/products/JO/blobs/" + files[0].sha256 &&
			std::string(json["downloads"][0]["method"].s()) == "GET");
	TEST_EXPECT(
			refused(h.send("POST", "/api/products/JO/downloads", { kJson, kCsrf }, hashes),
					404, "not_found"));
	TEST_EXPECT(refused(h.as_admin("POST", "/api/products/JO/downloads",
								"{\"hashes\": [\"not-a-sha\"]}"),
			400, "invalid_field"));
	return 0;
}

// A deleted game's purge waits out every presigned transfer handed out before the delete.
static_assert(opennova::novaworld_server::hosted::kProductPurgeDelaySeconds >
		opennova::novaworld_server::projects::S3ProjectUrls::kExpiresS);

// The blob route's cap is the portal's: web/nginx.conf's client_max_body_size in its /api/
// location (the one proxy in front of web's routes), so the route refuses exactly what the proxy
// would; one byte past it is 413 too_large, its bytes never put in the store.
int test_body_cap(Harness &h) {
	std::string conf, error;
	TEST_EXPECT(io::read_file_text(
			std::string(OPENNOVA_SOURCE_DIR) + "/web/nginx.conf", conf, error));
	const std::string location = "location /api/ {";
	const size_t api = conf.find(location);
	TEST_EXPECT(api != std::string::npos);
	// The location's own closing brace, past the blocks nested in it (its `if`).
	size_t end = api + location.size();
	for (int depth = 1; end < conf.size() && depth > 0; ++end) {
		if (conf[end] == '{')
			++depth;
		else if (conf[end] == '}')
			--depth;
	}
	const std::string directive = "client_max_body_size ";
	const size_t at = conf.find(directive, api);
	TEST_EXPECT(at != std::string::npos && at < end);
	const size_t number = at + directive.size();
	const unsigned long megabytes = std::strtoul(conf.c_str() + number, nullptr, 10);
	TEST_EXPECT(conf.find("m;", number) == conf.find_first_not_of("0123456789", number));
	TEST_EXPECT((static_cast<uint64_t>(megabytes) << 20) == projects::ProjectRoutes::kBodyMax);

	const std::string over(static_cast<size_t>(projects::ProjectRoutes::kBodyMax) + 1, 'o');
	TEST_EXPECT(refused(h.upload("JO", sha_of(over), over), 413, "too_large"));
	bool exists = true;
	uint64_t size = 0;
	TEST_EXPECT(
			h.store.head(projects::blob_key(h.jo, sha_of(over)), exists, size, error) && !exists);
	return 0;
}

// A game's project through the games' admin routes (products/product_routes.h): an expansion
// of Joint Operations made with its project, which builds on JO's, a key in use refused; a file
// in its store, then the game made public and its key renamed, the file staying where it was (the
// store names it by the game's id) and still held, the old key no project's (its 404 naming a
// renamed key); a push begun under the old key (its file uploaded
// there) committed under the new one, its manifest read back; Joint Operations kept from deletion
// while an expansion builds on it; the mod deleted at once, its files left for the data program's
// product.purge, queued with its id.
int test_game_projects(Harness &h) {
	const auto admin = [&](const char *method, const std::string &path,
							   const std::string &body = {}) {
		return h.send(method, path, { kJson, kBearer }, body);
	};
	auto json = json_of(admin("POST", "/api/admin/products",
								"{\"display_name\": \"A mod\", \"kind\": \"expansion\", "
								"\"parent_id\": " + std::to_string(h.jo) +
										", \"expansion_code\": \"mod\", \"key\": \"MOD\", "
										"\"visibility\": \"private\"}"),
			201);
	TEST_EXPECT(json && std::string(json["product"]["key"].s()) == "MOD" &&
			std::string(json["product"]["visibility"].s()) == "private" &&
			json["product"]["head"].i() == 0);
	const int64_t mod = json ? json["product"]["id"].i() : 0;
	json = json_of(h.send("GET", "/api/products/MOD", { h.admin }), 200);
	TEST_EXPECT(json && std::string(json["project"]["base"].s()) == "JO" &&
			std::string(json["project"]["title"].s()) == "A mod" &&
			json["project"]["product_id"].i() == mod && !json["project"]["retail"].b());
	TEST_EXPECT(refused(h.send("GET", "/api/products/MOD"), 404, "not_found"));
	TEST_EXPECT(refused(admin("POST", "/api/admin/products",
								"{\"display_name\": \"Again\", \"kind\": \"game\", \"title\": "
								"\"jo\", \"lobby\": \"jop_2_consumer\", \"key\": \"MOD\", "
								"\"visibility\": \"public\"}"),
			409, "duplicate_key"));

	// A file in its store; made public and renamed, the file stays where it is.
	TEST_EXPECT(json_of(h.upload("MOD", sha_of("mod file"), "mod file"), 200));
	const std::string game = "/api/admin/products/" + std::to_string(mod);
	TEST_EXPECT(refused(admin("PATCH", game, "{\"key\": \"JO\"}"), 409, "duplicate_key"));
	json = json_of(admin("PATCH", game,
						   "{\"visibility\": \"public\", \"key\": \"MODDED\", \"display_name\": "
						   "\"Renamed\"}"),
			200);
	TEST_EXPECT(json && std::string(json["product"]["key"].s()) == "MODDED" &&
			std::string(json["product"]["visibility"].s()) == "public");
	bool exists = false;
	uint64_t size = 0;
	std::string error;
	const std::string mod_file = projects::blob_key(mod, sha_of("mod file"));
	TEST_EXPECT(h.store.head(mod_file, exists, size, error) && exists);
	const auto old_key = h.send("GET", "/api/products/MOD", { h.admin });
	TEST_EXPECT(refused(old_key, 404, "not_found") &&
			body_text(old_key).find("a renamed key") != std::string::npos);
	json = json_of(h.send("GET", "/api/products/MODDED"), 200);
	TEST_EXPECT(json && std::string(json["project"]["title"].s()) == "Renamed");
	json = json_of(h.as_admin("POST", "/api/products/MODDED/missing",
						   missing_body({ file_of("mod.def", "mod file") })),
			200);
	TEST_EXPECT(json && json["missing"].size() == 0);

	// A push across a rename: its file uploaded under MODDED, the key renamed MODA, its revision
	// pushed under MODA with nothing to upload again, its manifest read back.
	const std::string across = "uploaded before the rename";
	TEST_EXPECT(json_of(h.upload("MODDED", sha_of(across), across), 200));
	TEST_EXPECT(json_of(admin("PATCH", game, "{\"key\": \"MODA\"}"), 200));
	const std::vector<ProjectFile> pushed = { file_of("mod.def", "mod file"),
		file_of("across.def", across) };
	json = json_of(
			h.as_admin("POST", "/api/products/MODA/missing", missing_body(pushed)), 200);
	TEST_EXPECT(json && json["missing"].size() == 0);
	const std::string pushed_manifest = opennova::project::encode_manifest(pushed);
	json = json_of(h.as_admin("POST", "/api/products/MODA/revisions",
						   push_body(0, "Across a rename", pushed_manifest)),
			201);
	TEST_EXPECT(json && json["head"].i() == 1);
	const auto read_back =
			h.send("GET", "/api/products/MODA/revisions/1/manifest", { h.admin });
	TEST_EXPECT(read_back.code == 200 && body_text(read_back) == pushed_manifest);

	// Joint Operations stays while an expansion builds on it; the mod goes at once, its files
	// left for the data program's purge.
	TEST_EXPECT(refused(admin("DELETE", "/api/admin/products/" + std::to_string(h.jo)), 409,
			"has_expansion"));
	const auto gone = admin("DELETE", game);
	TEST_EXPECT(gone.transport_ok && gone.code == 204);
	TEST_EXPECT(h.store.head(mod_file, exists, size, error) && exists);
	const auto purge = h.pool.acquire()->query(
			"SELECT count(*) FROM jobs WHERE kind = 'product.purge' AND state = 'queued' AND "
			"(payload->>'product_id')::bigint = $1;",
			{ BindValue(mod) });
	TEST_EXPECT(!purge.empty() && purge.front().as_int(0).value_or(0) == 1);
	TEST_EXPECT(
			refused(h.send("GET", "/api/products/MODA", { h.admin }), 404, "not_found"));
	return 0;
}

// A release built from a revision (products/release_build.h), through the project's own route on
// an admin's session: refused with no version or no such revision, to a player (JO is private: no
// project to it), to the Bearer token (a release names its author) and with no CSRF header; made
// building with its build queued, its creator the admin, and kept from deletion while the data
// program is on it. The admin's route of old is gone.
int test_release_route(Harness &h) {
	const std::string jo = "/api/products/JO/releases";
	TEST_EXPECT(refused(h.as_admin("POST", jo, "{\"version\": \"1.x\"}"), 400, "invalid_field"));
	TEST_EXPECT(refused(h.as_admin("POST", jo, "{}"), 400, "invalid_field"));
	TEST_EXPECT(refused(h.as_admin("POST", jo, "{\"version\": \"1.0\", \"revision\": 99}"), 400,
			"no_revision"));
	TEST_EXPECT(
			refused(h.as_admin("POST", "/api/products/JOX01/releases", "{\"version\": \"1.0\"}"),
					400, "no_revision"));
	TEST_EXPECT(refused(h.as_admin("POST", "/api/products/NOPE/releases", "{\"version\": \"1.0\"}"),
			404, "not_found"));
	TEST_EXPECT(refused(h.send("POST", jo, { kJson, kCsrf, h.player }, "{\"version\": \"1.0\"}"),
			404, "not_found"));
	TEST_EXPECT(refused(
			h.send("POST", jo, { kJson, kBearer }, "{\"version\": \"1.0\"}"), 401, "unauthorized"));
	TEST_EXPECT(refused(h.send("POST", jo, { kJson, h.admin }, "{\"version\": \"1.0\"}"), 403,
			"csrf_header_required"));
	// The admin's route of old is gone: no route takes a POST there (the catch-all, a GET's, alone
	// matches the path).
	const auto gone = h.send("POST", "/api/admin/products/JO/releases", { kJson, kBearer },
			"{\"version\": \"1.0\"}");
	TEST_EXPECT(gone.transport_ok && gone.code == 405);

	const auto head = json_of(h.send("GET", "/api/products/JO", { h.admin }), 200);
	TEST_EXPECT(head && head["project"]["head"].i() >= 1 && head["project"]["can_release"].b() &&
			head["project"]["release"].t() == crow::json::type::Null);
	const int64_t number = head ? head["project"]["head"].i() : 0;
	const auto made = json_of(h.as_admin("POST", jo, "{\"version\": \"1.0.1\"}"), 201);
	TEST_EXPECT(made && std::string(made["release"]["status"].s()) == "building" &&
			made["release"]["product_id"].i() == h.jo &&
			std::string(made["release"]["source"]["project"].s()) == "JO" &&
			made["release"]["source"]["revision"].i() == number &&
			std::string(made["release"]["version"].s()) == "1.0.1" &&
			std::string(made["release"]["name"].s()) == "JO 1.0.1" &&
			std::string(made["release"]["created_by"].s()) == "admin content_admin");
	const auto listed = json_of(h.send("GET", jo, { h.admin }), 200);
	TEST_EXPECT(listed && listed["releases"].size() == 1 &&
			listed["releases"][0]["id"].i() == made["release"]["id"].i());
	const auto admin = [&](const char *method, const std::string &path,
							   const std::string &body = {}) {
		return h.send(method, path, { kJson, kBearer }, body);
	};
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

// The id of the account named `username`, 0 for none.
int64_t account_id(Harness &h, const char *username) {
	const auto rows = h.pool.acquire()->query(
			"SELECT id FROM players WHERE username = $1;", { BindValue(username) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

// The product.purge jobs queued for `product_id`.
int64_t purges_of(Harness &h, int64_t product_id) {
	const auto rows = h.pool.acquire()->query(
			"SELECT count(*) FROM jobs WHERE kind = 'product.purge' AND state = 'queued' AND "
			"(payload->>'product_id')::bigint = $1;",
			{ BindValue(product_id) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

// The release `id` made a draft as release.build and release.validate leave one (its package's sha
// and bytes, its one section of `product_id`'s `command_file`).
int draft(Harness &h, int64_t id, int64_t product_id, const std::string &command_file,
		const std::string &sha, int64_t bytes) {
	auto db = h.pool.acquire();
	TEST_EXPECT(hosted::set_release_built(*db, id, "built", "0.0.22"));
	TEST_EXPECT(hosted::set_release_validated(
			*db, id, sha, bytes, "x", { { product_id, "", command_file, "", 1, 1 } }));
	return 0;
}

// An account's own releases (ADR 0074): built from its project's revision by its owner (its
// creator "user <name>"), listed, one at a time (409 release_in_progress), five a UTC day (429
// too_many_releases with Retry-After), none past the quota (413 quota_exceeded), an admin exempt
// from all three; published by its owner for an admin's approval (awaiting_approval: served,
// listed and applied nowhere), named in the admin's attention list, rejected back to draft with
// a note, withdrawn by its owner's delete, and published by an admin (an own game's first
// publish retaining its selected lobby); its reply naming the release once published; retired and
// deleted; another account refused, a release of another project no release of this one's; the
// mods a game's players may install (GET /api/mods: the published public ones, and the caller's
// own private ones); its package counted in the owner's usage.
int test_own_releases(Harness &h) {
	const std::string owner = signed_in(h, "rel_owner", "player");
	const std::string other = signed_in(h, "rel_other", "player");
	TEST_EXPECT(!owner.empty() && !other.empty());
	const int64_t owner_id = account_id(h, "rel_owner");
	const auto as = [&h](const std::string &cookie, const char *method, const std::string &path,
							const std::string &body = {}) {
		return h.send(method, path, { kJson, kCsrf, cookie }, body);
	};
	auto json = json_of(
			as(owner, "POST", "/api/products",
					R"({"key":"RELGAME","display_name":"Release game","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})"),
			201);
	const int64_t game = json ? json["project"]["product_id"].i() : 0;
	TEST_EXPECT(json && json["project"]["can_release"].b() &&
			json["project"]["release"].t() == crow::json::type::Null);
	json = json_of(
			as(owner, "POST", "/api/products",
					R"({"key":"RELMOD","display_name":"Release mod","kind":"expansion","base":"JO","expansion_code":"relmod","visibility":"private"})"),
			201);
	const int64_t mod = json ? json["project"]["product_id"].i() : 0;
	// A revision of each to build from.
	for (const char *key : { "RELGAME", "RELMOD" }) {
		const std::string bytes = std::string("release file of ") + key;
		const std::vector<ProjectFile> files = { file_of("a.def", bytes) };
		TEST_EXPECT(json_of(as(owner, "POST", std::string("/api/products/") + key + "/missing",
									missing_body(files)),
				200));
		TEST_EXPECT(json_of(
				h.send("PUT", std::string("/api/products/") + key + "/blobs/" + files[0].sha256,
						{ kOctets, kCsrf, owner }, bytes),
				200));
		TEST_EXPECT(
				json_of(as(owner, "POST", std::string("/api/products/") + key + "/revisions",
								push_body(0, "First", opennova::project::encode_manifest(files))),
						201));
	}
	const std::string releases = "/api/products/RELGAME/releases";
	// Another account may not release it, nor list its releases.
	TEST_EXPECT(refused(as(other, "POST", releases, R"({"version":"1.0"})"), 403, "forbidden"));
	TEST_EXPECT(refused(h.send("GET", releases, { other }), 403, "forbidden"));
	TEST_EXPECT(
			refused(h.send("GET", "/api/products/RELMOD/releases", { other }), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", releases), 401, "unauthorized"));

	// Its owner builds one; another waits for it.
	json = json_of(as(owner, "POST", releases, R"({"version":"1.0"})"), 201);
	TEST_EXPECT(json && std::string(json["release"]["status"].s()) == "building" &&
			std::string(json["release"]["created_by"].s()) == "user rel_owner" &&
			json["release"]["source"]["revision"].i() == 1);
	const int64_t first = json ? json["release"]["id"].i() : 0;
	TEST_EXPECT(refused(as(owner, "POST", "/api/products/RELMOD/releases", R"({"version":"1.0"})"),
			409, "release_in_progress"));
	json = json_of(h.send("GET", releases, { owner }), 200);
	TEST_EXPECT(json && json["releases"].size() == 1 && json["releases"][0]["id"].i() == first);
	// Built and validated: a draft, its owner's to publish with its selected lobby.
	TEST_EXPECT(draft(h, first, game, "RELGAME.CMD", sha_of("first package"), 1000) == 0);
	TEST_EXPECT(
			refused(as(owner, "POST",
							"/api/products/RELMOD/releases/" + std::to_string(first) + "/publish"),
					404, "not_found"));
	TEST_EXPECT(refused(as(other, "POST", releases + "/" + std::to_string(first) + "/publish"), 403,
			"forbidden"));
	// Its owner's publish waits for an admin's approval: nothing served, applied or listed.
	const std::string publish_first = releases + "/" + std::to_string(first) + "/publish";
	json = json_of(as(owner, "POST", publish_first), 202);
	TEST_EXPECT(json && std::string(json["release"]["status"].s()) == "awaiting_approval" &&
			std::string(json["release"]["review_note"].s()).empty());
	TEST_EXPECT(refused(as(owner, "POST", publish_first), 409, "not_draft"));
	json = json_of(h.send("GET", "/api/products/RELGAME"), 200);
	TEST_EXPECT(json && json["project"]["release"].t() == crow::json::type::Null);
	TEST_EXPECT(h.pool.acquire()
						->query("SELECT 1 FROM products WHERE key = 'RELGAME' AND lobby <> '' AND "
								"lobby_locked;")
						.size() == 1);
	TEST_EXPECT(h.pool.acquire()
					->query("SELECT 1 FROM jobs WHERE kind = 'release.apply' AND "
							"(payload->>'release_id')::bigint = $1;",
							{ BindValue(first) })
					.empty());
	// The admin's attention list names it, linked to its product's page.
	const auto awaiting = [&h](const char *subject) {
		const auto json = json_of(h.send("GET", "/api/admin/attention", { kBearer }), 200);
		int found = 0;
		if (json)
			for (const auto &f : json["findings"])
				if (std::string(f["source"].s()) == "releases" &&
						std::string(f["subject"].s()) == subject) {
					found += std::string(f["level"].s()) == "warn" &&
									std::string(f["text"].s()) ==
											"Awaits approval (by rel_owner)" &&
									std::string(f["link"].s()) == "/admin/products/RELGAME" &&
									f["since"].t() == crow::json::type::String
							? 1
							: 100;
				}
		return found;
	};
	TEST_EXPECT(awaiting("RELGAME 1.0") == 1);
	// An admin sends it back with a note (trimmed); a second reject finds none awaiting.
	const std::string reject_first = "/api/admin/releases/" + std::to_string(first) + "/reject";
	TEST_EXPECT(
			refused(h.send("POST", reject_first, { kJson, kCsrf, owner }, "{}"), 403, "forbidden"));
	TEST_EXPECT(refused(h.send("POST", reject_first, { kJson, kBearer },
								"{\"message\": \"" + std::string(501, 'x') + "\"}"),
			400, "invalid_field"));
	json = json_of(h.send("POST", reject_first, { kJson, kBearer },
						   R"({"message":"  Name the maps, please.  "})"),
			202);
	TEST_EXPECT(json && std::string(json["release"]["status"].s()) == "draft" &&
			std::string(json["release"]["review_note"].s()) == "Name the maps, please.");
	TEST_EXPECT(
			refused(h.send("POST", reject_first, { kJson, kBearer }, "{}"), 409, "not_awaiting"));
	TEST_EXPECT(awaiting("RELGAME 1.0") == 0);
	// Published again by its owner (the note cleared), then approved: an admin's publish of a
	// release awaiting approval publishes it with the selected lobby unchanged.
	json = json_of(as(owner, "POST", publish_first), 202);
	TEST_EXPECT(json && std::string(json["release"]["status"].s()) == "awaiting_approval" &&
			std::string(json["release"]["review_note"].s()).empty());
	json = json_of(h.as_admin("POST", publish_first), 202);
	TEST_EXPECT(json && std::string(json["release"]["status"].s()) == "published");
	TEST_EXPECT(awaiting("RELGAME 1.0") == 0);
	{
		const auto lobby = h.pool.acquire()->query(
				"SELECT p.lobby, g.gate_tag, g.gsb_path, g.protocol FROM products p "
				"JOIN games g ON g.slug = p.lobby WHERE p.key = 'RELGAME';");
		TEST_EXPECT(lobby.size() == 1 && lobby[0].as_text(0).value_or("").find("on_") == 0 &&
				lobby[0].as_text(1).value_or("").find("on:") == 0 && lobby[0].as_text(2) == lobby[0].as_text(0).value_or("") + ".gsb?a=1" &&
				lobby[0].as_text(3) == "jo");
	}
	json = json_of(h.send("GET", "/api/products/RELGAME"), 200);
	TEST_EXPECT(json && json["project"]["release"]["id"].i() == first &&
			std::string(json["project"]["release"]["version"].s()) == "1.0" &&
			std::string(json["project"]["release"]["one_version"].s()) == "0.0.22" &&
			!json["project"]["can_release"].b());
	// Its package counts in its owner's usage.
	json = json_of(h.send("GET", "/api/me/projects", { owner }), 200);
	TEST_EXPECT(json && json["usage"]["release_bytes"].i() == 1000 &&
			json["usage"]["bytes"].i() > 1000);

	// Five a UTC day: the first and four more, each settled before the next; the sixth refused.
	for (int i = 0; i < 4; ++i) {
		json = json_of(as(owner, "POST", "/api/products/RELMOD/releases",
							   "{\"version\":\"1." + std::to_string(i + 1) + "\"}"),
				201);
		TEST_EXPECT(json);
		h.pool.acquire()->exec("UPDATE releases SET status = 'invalid' WHERE id = $1;",
				{ BindValue(json["release"]["id"].i()) });
	}
	const auto sixth = as(owner, "POST", "/api/products/RELMOD/releases", R"({"version":"2.0"})");
	TEST_EXPECT(refused(sixth, 429, "too_many_releases") &&
			!header_of(sixth, "Retry-After").empty() &&
			crow::json::load(body_text(sixth))["limit"].i() == 5);
	// An admin starts one on it all the same.
	json = json_of(
			h.as_admin("POST", "/api/products/RELMOD/releases", R"({"version":"2.0"})"), 201);
	TEST_EXPECT(json && std::string(json["release"]["created_by"].s()) == "admin content_admin");
	const int64_t by_admin = json ? json["release"]["id"].i() : 0;
	TEST_EXPECT(draft(h, by_admin, mod, "RELMOD.CMD", sha_of("mod package"), 10) == 0);
	// A new day; at the quota, no release starts but an admin's.
	h.pool.acquire()->exec(
			"DELETE FROM release_starts WHERE player_id = $1;", { BindValue(owner_id) });
	{
		auto db = h.pool.acquire();
		const int64_t used = projects::owned_usage(*db, owner_id).bytes;
		db->exec("INSERT INTO project_blobs (product_id, sha256, bytes) VALUES ($1, $2, $3);",
				{ BindValue(game), BindValue(sha_of("a quota filler")),
						BindValue(hosted::kOwnedBytesMax - used) });
	}
	const auto full = as(owner, "POST", releases, R"({"version":"3.0"})");
	TEST_EXPECT(refused(full, 413, "quota_exceeded") &&
			crow::json::load(body_text(full))["used"].i() == hosted::kOwnedBytesMax);
	json = json_of(h.as_admin("POST", releases, R"({"version":"3.0"})"), 201);
	TEST_EXPECT(json);
	const int64_t third = json ? json["release"]["id"].i() : 0;
	h.pool.acquire()->exec("DELETE FROM project_blobs WHERE sha256 = $1;",
			{ BindValue(sha_of("a quota filler")) });
	// Published by its owner, then withdrawn: its owner's delete takes one awaiting approval.
	TEST_EXPECT(draft(h, third, game, "RELGAME.CMD", sha_of("third package"), 10) == 0);
	TEST_EXPECT(
			json_of(as(owner, "POST", releases + "/" + std::to_string(third) + "/publish"), 202));
	TEST_EXPECT(awaiting("RELGAME 3.0") == 1);
	const auto withdrawn = as(owner, "DELETE", releases + "/" + std::to_string(third));
	TEST_EXPECT(withdrawn.transport_ok && withdrawn.code == 204);
	TEST_EXPECT(awaiting("RELGAME 3.0") == 0);

	// The mods of Joint Operations: the private one its owner's alone, once published.
	const auto mod_keys = [&h](const std::vector<std::string> &headers, const char *game) {
		std::vector<std::string> keys;
		const auto json =
				json_of(h.send("GET", std::string("/api/mods?game=") + game, headers), 200);
		if (json)
			for (const auto &m : json["mods"])
				keys.emplace_back(m["key"].s());
		return keys;
	};
	TEST_EXPECT(mod_keys({ owner }, "JOPTR.CMD").empty());
	// Awaiting approval it lists nowhere; the admin's publish route approves it.
	json = json_of(
			as(owner, "POST",
					"/api/products/RELMOD/releases/" + std::to_string(by_admin) + "/publish"),
			202);
	TEST_EXPECT(json && std::string(json["release"]["status"].s()) == "awaiting_approval");
	TEST_EXPECT(mod_keys({ owner }, "JOPTR.CMD").empty());
	TEST_EXPECT(
			json_of(h.send("POST", "/api/admin/releases/" + std::to_string(by_admin) + "/publish",
							{ kJson, kBearer }),
					202));
	TEST_EXPECT(mod_keys({}, "JOPTR.CMD").empty() && mod_keys({ other }, "joptr.cmd").empty());
	TEST_EXPECT((mod_keys({ owner }, "joptr.cmd") == std::vector<std::string>{ "RELMOD" }));
	TEST_EXPECT(
			json_of(as(owner, "PATCH", "/api/products/RELMOD", R"({"visibility":"public"})"), 200));
	json = json_of(h.send("GET", "/api/mods?game=JOPTR.CMD"), 200);
	TEST_EXPECT(json && json["mods"].size() == 1);
	if (json && json["mods"].size() == 1) {
		const auto &m = json["mods"][0];
		TEST_EXPECT(std::string(m["key"].s()) == "RELMOD" &&
				std::string(m["code"].s()) == "relmod" &&
				std::string(m["name"].s()) == "Release mod" &&
				std::string(m["owner"].s()) == "rel_owner" &&
				std::string(m["version"].s()) == "2.0" &&
				std::string(m["updater_login"].s()) == "RELMOD" &&
				std::string(m["command_file"].s()) == "RELMOD.CMD");
	}
	TEST_EXPECT(mod_keys({}, "RELGAME.CMD").empty());
	TEST_EXPECT(refused(h.send("GET", "/api/mods"), 400, "invalid_query"));

	// Retired, then deleted, by its owner; a published one is retired first.
	const std::string at = releases + "/" + std::to_string(first);
	TEST_EXPECT(refused(as(owner, "DELETE", at), 409, "published"));
	json = json_of(as(owner, "POST", at + "/retire"), 202);
	TEST_EXPECT(json && std::string(json["release"]["status"].s()) == "retired");
	TEST_EXPECT(refused(as(owner, "POST", at + "/retire"), 409, "not_published"));
	const auto deleted = as(owner, "DELETE", at);
	TEST_EXPECT(deleted.transport_ok && deleted.code == 204);
	TEST_EXPECT(refused(as(owner, "DELETE", at), 404, "not_found"));
	return 0;
}

// An account's own projects (POST /api/products and its kin): made, with every refusal (the
// shapes, a key in use, an expansion code its base has, no base the caller may build on, the rate
// limit, the cap); read, changed and pushed by the one rule (no one, another player, the owner, an
// admin's session and the token over a private and a public one); each caller's list; the quota
// on their stores, an admin's push into one counted against its owner; the account's own page; a
// delete with its purge queued; and the account's delete taking them all, refused while another
// account's expansion builds on one.
int test_own_projects(Harness &h) {
	const std::string maker = signed_in(h, "own_maker", "player");
	const std::string other = signed_in(h, "own_other", "player");
	const std::string filler = signed_in(h, "own_filler", "player");
	TEST_EXPECT(!maker.empty() && !other.empty() && !filler.empty());
	const int64_t maker_id = account_id(h, "own_maker"), filler_id = account_id(h, "own_filler");
	const auto as = [&h](const std::string &cookie, const char *method, const std::string &path,
							const std::string &body = {}) {
		return h.send(method, path, { kJson, kCsrf, cookie }, body);
	};
	const auto made = [&](const std::string &cookie, const std::string &body) {
		return as(cookie, "POST", "/api/products", body);
	};
	const auto field_is = [](const net::HttpReply &reply, const char *field) {
		const auto json = crow::json::load(body_text(reply));
		return json && json.has("field") && std::string(json["field"].s()) == field;
	};

	// The shapes, each refused before it draws on the account's rate limit.
	struct Shape {
		const char *body;
		const char *error;
		const char *field;
	};
	const Shape shapes[] = {
		{ R"({"display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})",
				"missing_field", "key" },
		{ R"({"key":"bad key","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})",
				"invalid_field", "key" },
		{ R"({"key":"X1","display_name":"","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})",
				"invalid_field", "display_name" },
		{ R"({"key":"X1","display_name":"X","kind":"app","visibility":"public"})", "invalid_field",
				"kind" },
		{ R"({"key":"X1","display_name":"X","kind":"game","visibility":"public"})", "missing_field",
				"game" },
		{ R"({"key":"X1","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"bhd","visibility":"public"})",
				"invalid_field", "game" },
		{ R"({"key":"X1","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","base":"JO","visibility":"public"})",
				"invalid_field", "base" },
		{ R"({"key":"X1","display_name":"X","kind":"expansion","base":null,"expansion_code":"x","visibility":"public"})",
				"missing_field", "base" },
		{ R"({"key":"X1","display_name":"X","kind":"expansion","base":"JO","expansion_code":"X 1","visibility":"public"})",
				"invalid_field", "expansion_code" },
		{ R"({"key":"X1","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo"})", "missing_field",
				"visibility" },
		{ R"({"key":"X1","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"secret"})",
				"invalid_field", "visibility" },
		{ R"({"key":1,"display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})",
				"invalid_field", "key" },
		// An own key names its updater product: at most 11 characters, with no QQ.
		{ R"({"key":"TWELVE_CHARS","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})",
				"invalid_field", "key" },
		{ R"({"key":"MYQQGAME","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})",
				"invalid_field", "key" },
	};
	for (const Shape &shape : shapes) {
		const auto reply = made(maker, shape.body);
		TEST_EXPECT(refused(reply, 400, shape.error) && field_is(reply, shape.field));
	}
	TEST_EXPECT(refused(made(maker, "nope"), 400, "invalid_json"));
	TEST_EXPECT(refused(
			h.send("POST", "/api/products", { kJson, kCsrf },
					R"({"key":"X1","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})"),
			401, "unauthorized"));
	TEST_EXPECT(refused(
			h.send("POST", "/api/products", { kJson, maker },
					R"({"key":"X1","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})"),
			403, "csrf_header_required"));

	// A game of its own and a mod of the private retail Joint Operations.
	auto json = json_of(
			made(maker,
					R"({"key":"MAKERGAME","display_name":"Maker game","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"private"})"),
			201);
	TEST_EXPECT(json);
	{
		const auto &p = json["project"];
		TEST_EXPECT(std::string(p["key"].s()) == "MAKERGAME" &&
				std::string(p["title"].s()) == "Maker game" &&
				std::string(p["kind"].s()) == "game" && std::string(p["game"].s()) == "jo" &&
				std::string(p["expansion_code"].s()).empty() &&
				std::string(p["visibility"].s()) == "private" && !p["retail"].b() &&
				p["base"].t() == crow::json::type::Null && p["head"].i() == 0 &&
				std::string(p["owner"].s()) == "own_maker" && p["can_push"].b() &&
				p["can_manage"].b() && p["storage"]["files"].i() == 0);
	}
	const int64_t game_id = json ? json["project"]["product_id"].i() : 0;
	json = json_of(
			made(maker,
					R"({"key":"MAKERMOD","display_name":"Maker mod","kind":"expansion","base":"JO","expansion_code":"makermod","visibility":"public"})"),
			201);
	TEST_EXPECT(json && std::string(json["project"]["kind"].s()) == "expansion" &&
			std::string(json["project"]["base"].s()) == "JO" &&
			std::string(json["project"]["game"].s()) == "jo" &&
			std::string(json["project"]["expansion_code"].s()) == "makermod" &&
			std::string(json["project"]["owner"].s()) == "own_maker");
	const int64_t mod_id = json ? json["project"]["product_id"].i() : 0;
	{
		const auto row = h.pool.acquire()->query(
				"SELECT lobby, updater_login, retail, owner_id, command_file, download, created_by "
				"FROM products WHERE key = 'MAKERGAME';");
		TEST_EXPECT(row.size() == 1 && row[0].as_text(0).value_or("").find("on_") == 0 &&
				row[0].as_text(1) == "MAKERGAME" && row[0].as_int(2).value_or(1) == 0 &&
				row[0].as_int(3).value_or(0) == maker_id && row[0].as_text(4) == "MAKERGAME.CMD" &&
				row[0].as_text(5) == "player-makergame-game-windows.zip" &&
				row[0].as_text(6) == "player own_maker");
		const auto mod = h.pool.acquire()->query("SELECT updater_login, command_file, download "
												 "FROM products WHERE key = 'MAKERMOD';");
		TEST_EXPECT(mod.size() == 1 && mod[0].as_text(0) == "MAKERMOD" &&
				mod[0].as_text(1) == "MAKERMOD.CMD" && mod[0].as_text(2).value_or("x").empty());
	}
	// A key in use, one another product's updater goes by (JO's command file JOPTR.CMD), a code
	// any expansion has (the service's own Nifty Package's, under another game), no base: then the
	// account's creates are spent.
	TEST_EXPECT(refused(
			made(maker,
					R"({"key":"JO","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})"),
			409, "duplicate_key"));
	TEST_EXPECT(refused(
			made(other,
					R"({"key":"JOPTR","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})"),
			409, "duplicate_key"));
	{
		const auto taken = made(maker,
				R"({"key":"MAKERMOD2","display_name":"X","kind":"expansion","base":"JO","expansion_code":"nifty","visibility":"public"})");
		const auto refusal = crow::json::load(body_text(taken));
		TEST_EXPECT(refused(taken, 409, "duplicate_expansion") &&
				field_is(taken, "expansion_code") &&
				std::string(refusal["message"].s()) == "another mod already uses this short name");
	}
	TEST_EXPECT(refused(
			made(maker,
					R"({"key":"MAKERMOD2","display_name":"X","kind":"expansion","base":"NOPE","expansion_code":"x","visibility":"public"})"),
			404, "no_such_base"));
	const auto braked = made(maker,
			R"({"key":"MAKERX","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})");
	TEST_EXPECT(
			braked.transport_ok && braked.code == 429 && !header_of(braked, "Retry-After").empty());
	// No base in an app, nor in another account's game (its private one here; its public one
	// below).
	TEST_EXPECT(refused(
			made(other,
					R"({"key":"OTHERMOD","display_name":"X","kind":"expansion","base":"ONRUNTIME","expansion_code":"x","visibility":"public"})"),
			404, "no_such_base"));
	TEST_EXPECT(refused(
			made(other,
					R"({"key":"OTHERMOD","display_name":"X","kind":"expansion","base":"MAKERGAME","expansion_code":"x","visibility":"public"})"),
			404, "no_such_base"));
	// The cap: an account that owns kOwnedProjectsMax makes no more.
	{
		auto db = h.pool.acquire();
		for (int64_t i = 0; i < hosted::kOwnedProjectsMax; ++i) {
			hosted::NewProduct full;
			full.display_name = "Filler";
			full.title = "dfx2";
			full.new_lobby_name = "Filler lobby";
			full.key = "FILLER" + std::to_string(i);
			full.visibility = "private";
			full.owner_id = filler_id;
			int64_t id = 0;
			TEST_EXPECT(hosted::create_product(*db, full, id) == hosted::ProductOutcome::Ok);
		}
	}
	const auto capped = made(filler,
			R"({"key":"FILLERX","display_name":"X","kind":"game","new_lobby_name":"Test lobby","game":"jo","visibility":"public"})");
	TEST_EXPECT(refused(capped, 403, "too_many_projects") &&
			crow::json::load(body_text(capped))["limit"].i() == hosted::kOwnedProjectsMax);

	// Who reads, changes and pushes the private game and the public mod.
	const auto can = [&h](const std::vector<std::string> &headers, const std::string &key,
							 bool push, bool manage) {
		const auto json = json_of(h.send("GET", "/api/products/" + key, headers), 200);
		return json && json["project"]["can_push"].b() == push &&
				json["project"]["can_manage"].b() == manage &&
				json["project"].has("storage") == manage;
	};
	TEST_EXPECT(refused(h.send("GET", "/api/products/MAKERGAME"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/products/MAKERGAME", { other }), 404, "not_found"));
	TEST_EXPECT(can({ maker }, "MAKERGAME", true, true));
	TEST_EXPECT(can({ h.admin }, "MAKERGAME", true, true));
	TEST_EXPECT(can({ kBearer }, "MAKERGAME", false, true));
	TEST_EXPECT(can({}, "MAKERMOD", false, false));
	TEST_EXPECT(can({ other }, "MAKERMOD", false, false));
	TEST_EXPECT(can({ maker }, "MAKERMOD", true, true));
	const std::string a_file = missing_body({ file_of("a.def", "maker file") });
	TEST_EXPECT(
			refused(as(other, "POST", "/api/products/MAKERMOD/missing", a_file), 403, "forbidden"));
	TEST_EXPECT(refused(as(other, "PATCH", "/api/products/MAKERMOD", R"({"display_name":"Mine"})"),
			403, "forbidden"));
	TEST_EXPECT(refused(as(other, "DELETE", "/api/products/MAKERMOD"), 403, "forbidden"));
	TEST_EXPECT(refused(
			as(other, "POST", "/api/products/MAKERGAME/missing", a_file), 404, "not_found"));
	TEST_EXPECT(refused(as(other, "PATCH", "/api/products/MAKERGAME", R"({"display_name":"Mine"})"),
			404, "not_found"));
	TEST_EXPECT(refused(as(other, "DELETE", "/api/products/MAKERGAME"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("PATCH", "/api/products/MAKERGAME", { kJson, kBearer },
								R"({"display_name":"Mine"})"),
			401, "unauthorized"));

	// The lists: the public ones and the caller's own; every one for an admin.
	const auto lists = [&h](const std::vector<std::string> &headers, const char *key) {
		const auto keys = keys_of(json_of(h.send("GET", "/api/products", headers), 200));
		return std::find(keys.begin(), keys.end(), key) != keys.end();
	};
	TEST_EXPECT(lists({}, "MAKERMOD") && !lists({}, "MAKERGAME"));
	TEST_EXPECT(lists({ other }, "MAKERMOD") && !lists({ other }, "MAKERGAME"));
	TEST_EXPECT(lists({ maker }, "MAKERMOD") && lists({ maker }, "MAKERGAME"));
	TEST_EXPECT(lists({ h.admin }, "MAKERGAME") && lists({ h.admin }, "FILLER0"));

	// Its owner pushes it; so may an admin.
	const std::string bytes = "maker file";
	const std::vector<ProjectFile> files = { file_of("a.def", bytes) };
	json = json_of(as(maker, "POST", "/api/products/MAKERGAME/missing", missing_body(files)), 200);
	TEST_EXPECT(json && json["missing"].size() == 1);
	TEST_EXPECT(json_of(h.send("PUT", "/api/products/MAKERGAME/blobs/" + files[0].sha256,
								{ kOctets, kCsrf, maker }, bytes),
			200));
	json = json_of(as(maker, "POST", "/api/products/MAKERGAME/revisions",
						   push_body(0, "First", opennova::project::encode_manifest(files))),
			201);
	TEST_EXPECT(json && json["head"].i() == 1 &&
			std::string(json["revision"]["author"].s()) == "own_maker");
	TEST_EXPECT(json_of(
			h.as_admin("POST", "/api/products/MAKERGAME/missing", missing_body(files)), 200));

	// Changed by its owner, and by an admin.
	json = json_of(as(maker, "PATCH", "/api/products/MAKERGAME",
						   R"({"visibility":"public","display_name":"Maker's game"})"),
			200);
	TEST_EXPECT(json && std::string(json["project"]["visibility"].s()) == "public" &&
			std::string(json["project"]["title"].s()) == "Maker's game" &&
			json["project"]["head"].i() == 1);
	const auto bad = as(maker, "PATCH", "/api/products/MAKERGAME", R"({"visibility":"secret"})");
	TEST_EXPECT(refused(bad, 400, "invalid_field") && field_is(bad, "visibility"));
	json = json_of(
			as(h.admin, "PATCH", "/api/products/MAKERMOD", R"({"display_name":"Renamed mod"})"),
			200);
	TEST_EXPECT(json && std::string(json["project"]["title"].s()) == "Renamed mod");
	TEST_EXPECT(can({ other }, "MAKERGAME", false, false));

	// The quota: an upload that would take the account's projects past 10 GiB together is refused,
	// whoever pushes it; one that fits is signed.
	const int64_t limit = hosted::kOwnedBytesMax;
	{
		auto db = h.pool.acquire();
		const auto used = db->query(
				"SELECT COALESCE(sum(b.bytes), 0)::bigint FROM project_blobs b JOIN products p ON "
				"p.id = b.product_id WHERE p.owner_id = $1;",
				{ BindValue(maker_id) });
		const int64_t held = used.empty() ? 0 : used.front().as_int(0).value_or(0);
		db->exec("INSERT INTO project_blobs (product_id, sha256, bytes) VALUES ($1, $2, $3);",
				{ BindValue(mod_id), BindValue(sha_of("a large file")),
						BindValue(limit - held - 5) });
	}
	const std::vector<ProjectFile> six = { file_of("six.def", "sixsix") },
								   five = { file_of("five.def", "fivef") };
	for (const std::string &cookie : { maker, h.admin }) {
		const auto over = as(cookie, "POST", "/api/products/MAKERGAME/missing", missing_body(six));
		const auto refusal = crow::json::load(body_text(over));
		TEST_EXPECT(refused(over, 413, "quota_exceeded") && refusal["used"].i() == limit - 5 &&
				refusal["limit"].i() == limit);
	}
	TEST_EXPECT(refused(h.send("PUT", "/api/products/MAKERGAME/blobs/" + six[0].sha256,
								{ kOctets, kCsrf, maker }, "sixsix"),
			413, "quota_exceeded"));
	json = json_of(as(maker, "POST", "/api/products/MAKERGAME/missing", missing_body(five)), 200);
	TEST_EXPECT(json && json["missing"].size() == 1);
	// A file the store got through its own URL is counted before it is held: past the quota,
	// neither the missing request nor a push records it.
	{
		test_temp::TempDir scratch("project_routes_found");
		const std::string path = scratch.file("six.bin");
		std::string error;
		TEST_EXPECT(io::write_file_atomic(path, std::string("sixsix"), error));
		TEST_EXPECT(h.store.put_file(projects::blob_key(game_id, six[0].sha256), path, error));
	}
	TEST_EXPECT(refused(as(maker, "POST", "/api/products/MAKERGAME/missing", missing_body(six)),
			413, "quota_exceeded"));
	TEST_EXPECT(refused(as(maker, "POST", "/api/products/MAKERGAME/revisions",
								push_body(1, "Six", opennova::project::encode_manifest(six))),
			413, "quota_exceeded"));
	TEST_EXPECT(h.pool.acquire()
					->query("SELECT 1 FROM project_blobs WHERE sha256 = $1;",
							{ BindValue(six[0].sha256) })
					.empty());
	// One the store holds at another size than the request names is removed, and asked for again.
	{
		test_temp::TempDir scratch("project_routes_sized");
		const std::string path = scratch.file("five.bin");
		std::string error;
		TEST_EXPECT(io::write_file_atomic(path, std::string("fi"), error));
		TEST_EXPECT(h.store.put_file(projects::blob_key(game_id, five[0].sha256), path, error));
	}
	json = json_of(as(maker, "POST", "/api/products/MAKERGAME/missing", missing_body(five)), 200);
	TEST_EXPECT(json && json["missing"].size() == 1);
	{
		bool exists = true;
		uint64_t size = 0;
		std::string error;
		TEST_EXPECT(
				h.store.head(projects::blob_key(game_id, five[0].sha256), exists, size, error) &&
				!exists);
	}

	// The account's own page: its projects, their usage and the limits, and what a new one may be.
	json = json_of(h.send("GET", "/api/me/projects", { maker }), 200);
	TEST_EXPECT(json && json["projects"].size() == 2 &&
			std::string(json["projects"][0]["key"].s()) == "MAKERGAME" &&
			std::string(json["projects"][1]["key"].s()) == "MAKERMOD" &&
			json["usage"]["projects"].i() == 2 && json["usage"]["bytes"].i() == limit - 5 &&
			json["usage"]["release_bytes"].i() == 0 &&
			json["limits"]["projects"].i() == hosted::kOwnedProjectsMax &&
			json["limits"]["bytes"].i() == limit);
	{
		std::vector<std::string> games;
		for (const auto &g : json["games"])
			games.emplace_back(g["key"].s());
		TEST_EXPECT((games == std::vector<std::string>{ "ACIDGAMBIT", "DFX2", "JO", "MAKERGAME" }));
		TEST_EXPECT(std::string(json["games"][1]["game"].s()) == "dfx2" &&
				std::string(json["games"][2]["title"].s()) == "Joint Operations");
		TEST_EXPECT(json["titles"].size() == 3 && std::string(json["titles"][0].s()) == "jo" &&
				std::string(json["titles"][1].s()) == "dfx2" &&
				std::string(json["titles"][2].s()) == "acidgambit" &&
				std::string(json["protocols"]["acidgambit"]["code"].s()) == "jo" &&
				std::string(json["protocols"]["dfx2"]["code"].s()) == "dfx2");
		// The lobbies its games may select, each with its protocol and no title.
		TEST_EXPECT(json["lobbies"].size() > 0);
		for (const auto &lobby : json["lobbies"])
			TEST_EXPECT(!lobby.has("title") && lobby["protocol"].t() == crow::json::type::Object &&
					std::string(lobby["protocol"]["version"].s()) == "16");
	}
	TEST_EXPECT(refused(h.send("GET", "/api/me/projects"), 401, "unauthorized"));

	// Another account builds no mod on the game, public as it is now (ADR 0074: a mod builds on a
	// game of the service's or one's own), and is offered none to build on.
	TEST_EXPECT(refused(
			made(other,
					R"({"key":"OTHERMOD","display_name":"Other mod","kind":"expansion","base":"MAKERGAME","expansion_code":"othermod","visibility":"private","game":"jo"})"),
			404, "no_such_base"));
	json = json_of(h.send("GET", "/api/me/projects", { other }), 200);
	{
		std::vector<std::string> games;
		for (const auto &g : json["games"])
			games.emplace_back(g["key"].s());
		TEST_EXPECT((games == std::vector<std::string>{ "ACIDGAMBIT", "DFX2", "JO" }));
	}
	// An expansion of the service's own an admin made on it: neither the game's delete nor its
	// owner's account's goes while it is there; its delete queues its purge.
	json = json_of(
			h.send("POST", "/api/admin/products", { kJson, kBearer },
					R"({"display_name":"Admin mod","kind":"expansion","parent_id":)" +
							std::to_string(game_id) +
							R"(,"expansion_code":"adminmod","key":"ADMINMOD","visibility":"private"})"),
			201);
	const int64_t admin_mod = json ? json["product"]["id"].i() : 0;
	TEST_EXPECT(refused(as(maker, "DELETE", "/api/products/MAKERGAME"), 409, "has_expansion"));
	const std::string account = "/api/admin/users/" + std::to_string(maker_id);
	TEST_EXPECT(refused(h.send("DELETE", account, { kBearer }), 409, "has_expansion"));
	TEST_EXPECT(json_of(h.send("GET", "/api/products/MAKERMOD"), 200) && purges_of(h, mod_id) == 0);
	const auto gone = h.send(
			"DELETE", "/api/admin/products/" + std::to_string(admin_mod), { kJson, kBearer });
	TEST_EXPECT(gone.transport_ok && gone.code == 204 && purges_of(h, admin_mod) == 1);

	// The account's delete takes its projects, each one's purge queued.
	const auto deleted = h.send("DELETE", account, { kBearer });
	TEST_EXPECT(deleted.transport_ok && deleted.code == 204);
	TEST_EXPECT(refused(h.send("GET", "/api/products/MAKERGAME", { h.admin }), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", "/api/products/MAKERMOD", { h.admin }), 404, "not_found"));
	TEST_EXPECT(purges_of(h, game_id) == 1 && purges_of(h, mod_id) == 1);
	return 0;
}

// A game's lobby through the project routes: chosen new or existing at its create, offered to the
// caller (a player never a retail game's, which the original clients browse; an admin every
// one), changed until a build locks it (a created lobby it leaves going with it, no other game
// naming it), and a locked game with no lobby still taking one.
int test_lobby_choices(Harness &h) {
	const std::string owner = signed_in(h, "choice_owner", "player");
	const std::string other = signed_in(h, "choice_other", "player");
	const auto write = [&h](const std::string &cookie, const char *method, const std::string &path,
							   const std::string &body) {
		return h.send(method, path, { kJson, kCsrf, cookie }, body);
	};
	TEST_EXPECT(refused(
			write(owner, "POST", "/api/products",
					R"({"key":"CHOICE","display_name":"Choice","kind":"game","game":"jo","visibility":"private"})"),
			400, "missing_field"));
	auto result = json_of(
			write(owner, "POST", "/api/products",
					R"({"key":"CHOICE","display_name":"Choice","kind":"game","game":"jo","visibility":"private","new_lobby_name":"Choice lobby"})"),
			201);
	TEST_EXPECT(result && !result["project"]["lobby_locked"].b());
	const std::string slug = std::string(result["project"]["lobby"]["slug"].s());
	TEST_EXPECT(std::string(result["project"]["lobby"]["display_name"].s()) == "Choice lobby" &&
			std::string(result["project"]["lobby"]["protocol"]["code"].s()) == "jo" &&
			!result["project"]["lobby"].has("title") &&
			!std::string(result["project"]["lobby"]["gate_tag_file"].s()).empty());
	const auto contains_lobby = [](const crow::json::rvalue &rows, const std::string &wanted) {
		for (const auto &row : rows)
			if (std::string(row["slug"].s()) == wanted)
				return true;
		return false;
	};
	result = json_of(h.send("GET", "/api/games"), 200);
	TEST_EXPECT(result && !contains_lobby(result["games"], slug));
	result = json_of(h.send("GET", "/api/me/projects", { owner }), 200);
	TEST_EXPECT(result && contains_lobby(result["lobbies"], slug));
	result = json_of(h.send("GET", "/api/me/projects", { other }), 200);
	TEST_EXPECT(result && !contains_lobby(result["lobbies"], slug) &&
			contains_lobby(result["lobbies"], "acidgambit_0_consumer") &&
			!contains_lobby(result["lobbies"], "jop_2_consumer") &&
			!contains_lobby(result["lobbies"], "dfx2_consumer"));
	result = json_of(h.send("GET", "/api/me/projects", { h.admin }), 200);
	TEST_EXPECT(result && contains_lobby(result["lobbies"], slug) &&
			contains_lobby(result["lobbies"], "jop_2_consumer") &&
			contains_lobby(result["lobbies"], "dfx2_consumer"));
	TEST_EXPECT(refused(
			write(other, "POST", "/api/products",
					R"({"key":"RETAILPICK","display_name":"Other","kind":"game","game":"jo","visibility":"private","lobby":"jop_2_consumer"})"),
			400, "invalid_field"));
	TEST_EXPECT(refused(
			write(other, "POST", "/api/products",
					R"({"key":"STEAL","display_name":"Other","kind":"game","game":"jo","visibility":"private","lobby":")" +
							slug + R"("})"),
			400, "invalid_field"));
	TEST_EXPECT(
			refused(write(owner, "PATCH", "/api/products/CHOICE", R"({"lobby":"dfx2_consumer"})"),
					400, "invalid_field"));
	TEST_EXPECT(refused(write(owner, "PATCH", "/api/products/CHOICE",
								R"({"lobby":"jop_2_consumer","new_lobby_name":"Both"})"),
			400, "invalid_field"));
	TEST_EXPECT(
			refused(write(owner, "PATCH", "/api/products/CHOICE", R"({"lobby":"jop_2_consumer"})"),
					400, "invalid_field"));
	result = json_of(
			write(owner, "PATCH", "/api/products/CHOICE", R"({"lobby":"acidgambit_0_consumer"})"),
			200);
	TEST_EXPECT(result &&
			std::string(result["project"]["lobby"]["slug"].s()) == "acidgambit_0_consumer");
	// The lobby it made, which no other game names, went with the move.
	TEST_EXPECT(h.pool.acquire()
					->query("SELECT 1 FROM games WHERE slug = $1;", { BindValue(slug) })
					.empty());
	const std::string encoded = std::string(result["project"]["lobby"]["gate_tag_file"].s());
	result = json_of(
			write(owner, "POST", "/api/products",
					R"({"key":"CHOICEMOD","display_name":"Choice mod","kind":"expansion","base":"CHOICE","expansion_code":"choice","visibility":"private"})"),
			201);
	TEST_EXPECT(result && std::string(result["project"]["lobby"]["gate_tag_file"].s()) == encoded);
	// No version.txt before a release names its version (the row names no line).
	TEST_EXPECT(result && result["project"]["version_file"].t() == crow::json::type::Null);
	const std::vector<ProjectFile> managed = { file_of("GT.SSC", "manual") };
	TEST_EXPECT(refused(
			write(owner, "POST", "/api/products/CHOICE/revisions",
					push_body(0, "Manual lobby file", opennova::project::encode_manifest(managed))),
			400, "invalid_manifest"));
	// An expansion's version.txt is NovaWorld's too.
	const std::vector<ProjectFile> version_text = { file_of("Version.txt", "Choice mod, V1.0; English") };
	TEST_EXPECT(refused(write(owner, "POST", "/api/products/CHOICEMOD/revisions",
								push_body(0, "Manual version file",
										opennova::project::encode_manifest(version_text))),
			400, "invalid_manifest"));
	const std::vector<ProjectFile> empty;
	TEST_EXPECT(json_of(write(owner, "POST", "/api/products/CHOICE/revisions",
								push_body(0, "First", opennova::project::encode_manifest(empty))),
			201));
	result = json_of(
			write(owner, "POST", "/api/products/CHOICE/releases", R"({"version":"1.0"})"), 201);
	TEST_EXPECT(result);
	const std::string release = std::to_string(result["release"]["id"].i());
	TEST_EXPECT(refused(
			write(owner, "PATCH", "/api/products/CHOICE", R"({"new_lobby_name":"Too late"})"), 409,
			"lobby_locked"));
	TEST_EXPECT(json_of(
			write(owner, "PATCH", "/api/products/CHOICE", R"({"lobby":"acidgambit_0_consumer"})"),
			200));
	result = json_of(h.send("GET", "/api/products/CHOICEMOD", { owner }), 200);
	TEST_EXPECT(result && result["project"]["lobby_locked"].b());
	const std::string limited = signed_in(h, "choice_limited", "player");
	TEST_EXPECT(json_of(
			write(limited, "POST", "/api/products",
					R"({"key":"LOBBYLIMIT","display_name":"Limited","kind":"game","game":"jo","visibility":"private","lobby":"acidgambit_0_consumer"})"),
			201));
	const auto lobbies = [&h] {
		return h.pool.acquire()->query("SELECT count(*) FROM games;")[0].as_int(0).value_or(0);
	};
	const int64_t before = lobbies();
	// Each new lobby takes the place of the one before it: one row more than at the start.
	for (int i = 0; i < 4; ++i)
		TEST_EXPECT(json_of(write(limited, "PATCH", "/api/products/LOBBYLIMIT",
									R"({"new_lobby_name":"Another lobby"})"),
				200));
	TEST_EXPECT(lobbies() == before + 1);
	const auto count = h.pool.acquire()->query("SELECT count(*) FROM games;")[0].as_int(0);
	TEST_EXPECT(refused(write(limited, "PATCH", "/api/products/LOBBYLIMIT",
								R"({"new_lobby_name":"Past the creation limit"})"),
			429, "rate_limited"));
	TEST_EXPECT(h.pool.acquire()->query("SELECT count(*) FROM games;")[0].as_int(0) == count);
	// Locked with no lobby (as the lobby migration leaves a game it found without one): the
	// selection is not fixed, so the reply says unlocked and the game takes a lobby, which is.
	h.pool.acquire()->exec("UPDATE products SET lobby = '' WHERE key = 'CHOICE';");
	result = json_of(h.send("GET", "/api/products/CHOICE", { owner }), 200);
	TEST_EXPECT(result && result["project"]["lobby"].t() == crow::json::type::Null &&
			!result["project"]["lobby_locked"].b());
	result = json_of(
			write(owner, "PATCH", "/api/products/CHOICE", R"({"new_lobby_name":"Found lobby"})"),
			200);
	TEST_EXPECT(result &&
			std::string(result["project"]["lobby"]["display_name"].s()) == "Found lobby" &&
			result["project"]["lobby_locked"].b());
	TEST_EXPECT(refused(
			write(owner, "PATCH", "/api/products/CHOICE", R"({"lobby":"acidgambit_0_consumer"})"),
			409, "lobby_locked"));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("project_routes");
		test_db::TestDatabase database;
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
			const auto rows = pool.acquire()->query("SELECT updater_login FROM products WHERE "
													"kind = 'game' AND updater_login <> '' "
													"AND NOT retail;");
			TEST_EXPECT(rows.size() == 1);
			harness.own_game = rows.front().as_text(0).value_or("");
			const auto jo = pool.acquire()->query("SELECT id FROM products WHERE key = 'JO';");
			TEST_EXPECT(jo.size() == 1);
			harness.jo = jo.empty() ? 0 : jo.front().as_int(0).value_or(0);
		}

		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "who_reads_what", test_who_reads_what },
			{ "who_pushes", test_who_pushes },
			{ "push", test_push },
			{ "body_cap", test_body_cap },
			{ "game_projects", test_game_projects },
			{ "release_route", test_release_route },
			{ "own_releases", test_own_releases },
			{ "own_projects", test_own_projects },
			{ "lobby_choices", test_lobby_choices },
		};
		rc = 0;
		for (const Case &c : cases) {
			const int result = c.fn(harness);
			std::printf("project_routes: %s %s\n", c.name, result == 0 ? "ok" : "FAILED");
			rc |= result;
		}
		http.stop();
	}
	net::shutdown();
	return rc;
}
