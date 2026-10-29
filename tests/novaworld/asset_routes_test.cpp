// Public asset routes over a real listener, migrated database, Redis namespace and local
// object store. Synthetic native bytes exercise the publication boundary and ZIP transport
// without proprietary game data or the extractor: private sources remain private, every
// contributor must share, and cached downloads are checked again after sharing is revoked.
#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"
#include "core/local_object_store.h"
#include "core/zip_files.h"
#include "http_listener.h"
#include "net_http.h"
#include "net_sockets.h"
#include "products/asset_catalog_repository.h"
#include "products/product_repository.h"
#include "products/project_repository.h"
#include "program/server_config.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <crow/json.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace io = opennova::io;
namespace db = opennova::db;
namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace projects = nws::projects;
using namespace nws::hosted;

namespace {
constexpr const char *kAdminToken = "asset-test-admin";
constexpr const char *kBearer = "Authorization: Bearer asset-test-admin";

struct NativeFile {
	std::string name, bytes, origin = "project";
	std::string sha() const { return io::sha256_hex(bytes.data(), bytes.size()); }
	io::JsonValue json() const {
		auto value = io::JsonValue::make_object();
		value.set("name", io::JsonValue::make_string(name));
		value.set("sha256", io::JsonValue::make_string(sha()));
		value.set("size", io::JsonValue::make_number(double(bytes.size())));
		value.set("origin", io::JsonValue::make_string(origin));
		return value;
	}
};

io::JsonValue asset(const char *kind, const std::vector<NativeFile> &files) {
	auto value = files.front().json();
	value.set("kind", io::JsonValue::make_string(kind));
	auto members = io::JsonValue::make_array();
	for (const auto &file : files) members.push(file.json());
	value.set("files", std::move(members));
	return value;
}

struct Harness {
	test_temp::TempDir &temp;
	db::ConnectionPool &pool;
	db::Database &db;
	nws::LocalObjectStore &store;
	uint16_t port;
	std::string error;

	net::HttpReply send(const char *method, const std::string &path,
			const std::vector<std::string> &headers = {}, const std::string &body = {}) const {
		return net::http_exchange(method, "http://127.0.0.1:" + std::to_string(port) + path,
				headers, body, 30000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
	}
	int64_t product(const char *key, int64_t base) {
		NewProduct p;
		p.key = key; p.display_name = std::string(key) + " project";
		p.kind = ProductKind::Expansion; p.parent_id = base; p.expansion_code = key;
		p.visibility = "private"; p.assets_public = true; p.created_by = "asset routes test";
		int64_t id = 0;
		return create_product(db, p, id) == ProductOutcome::Ok ? id : 0;
	}
	int64_t release(int64_t product) {
		NewBuiltRelease r;
		r.name = "Synthetic asset release"; r.product_id = product; r.revision = 1;
		r.base_revision = 7; r.version = "1.2.3"; r.created_by = "asset routes test";
		return create_built_release(db, r);
	}
	void publish(int64_t product, int64_t release) {
		db.exec("UPDATE releases SET status='published' WHERE id=$1", { release });
		db.exec("UPDATE products SET published_release_id=$2 WHERE id=$1", { product, release });
	}
	void share(int64_t product, bool enabled) {
		db.exec("UPDATE products SET assets_public=$2 WHERE id=$1", { product, db::Bool{enabled} });
	}
	bool snapshot(int64_t release, std::vector<io::JsonValue> entries, const std::vector<NativeFile> &files) {
		const std::string dir = temp.file(("snapshot" + std::to_string(release)).c_str());
		if (!io::ensure_directory(io::utf8_join(dir, "blobs"), error)) return false;
		for (const auto &file : files)
			if (!io::write_file_atomic(io::utf8_join(dir, "blobs/" + file.sha()), file.bytes, error)) return false;
		auto manifest = io::JsonValue::make_object(), members = io::JsonValue::make_array();
		members.array = std::move(entries);
		manifest.set("schema_version", io::JsonValue::make_number(1));
		manifest.set("assets", std::move(members));
		return io::write_file_atomic(io::utf8_join(dir, "manifest.json"), io::json_write(manifest), error) &&
				store_asset_snapshot(db, store, release, dir, error);
	}
	int64_t asset_id(int64_t release, const std::string &name) {
		const auto rows = db.query("SELECT id FROM asset_entries WHERE release_id=$1 AND name=$2", { release, name });
		return rows.size() == 1 ? rows.front().as_int(0).value_or(0) : 0;
	}
};

std::string body_text(const net::HttpReply &reply) { return { reply.body.begin(), reply.body.end() }; }
crow::json::rvalue json_of(const net::HttpReply &reply, int code) {
	if (!reply.transport_ok || reply.code != code) {
		std::fprintf(stderr, "HTTP %d (%s), want %d: %s\n", reply.code, reply.error.c_str(), code,
				body_text(reply).substr(0, 600).c_str());
		return {};
	}
	return crow::json::load(body_text(reply));
}
bool refused(const net::HttpReply &reply, int code, const char *error) {
	const auto json = json_of(reply, code);
	return json && json.has("error") && std::string(json["error"].s()) == error;
}
std::string header_of(const net::HttpReply &reply, const char *name) {
	for (const auto &line : reply.headers) {
		const auto colon = line.find(':');
		if (colon == std::string::npos) continue;
		std::string key = line.substr(0, colon);
		for (char &c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (key != name) continue;
		size_t start = colon + 1;
		while (start < line.size() && line[start] == ' ') ++start;
		return line.substr(start);
	}
	return {};
}

int test_routes(Harness &h) {
	const auto base = find_product_by_key(h.db, "JO");
	TEST_EXPECT(base);
	h.share(base->id, true);
	const int64_t product = h.product("LIBRARY", base->id), other = h.product("OTHERLIB", base->id);
	const int64_t hidden = h.product("DRAFTLIB", base->id);
	TEST_EXPECT(product && other && hidden);
	const int64_t release = h.release(product), other_release = h.release(other), draft = h.release(hidden);
	TEST_EXPECT(release && other_release && draft);
	const NativeFile model{"crate.3di", std::string("native\0model", 12)};
	const NativeFile skin{"skin.dds", std::string("base\0texture", 12), "base"};
	const std::vector<NativeFile> files{model, {"walk.bad", "animation"}, {"soldier.adm", "animation map"},
			{"island.trn", "terrain"}, {"island.dds", "texture"}, skin};
	const std::vector<std::string> kinds{"model", "animation", "animation_map", "terrain", "texture"};
	std::vector<io::JsonValue> entries{asset("model", {model, skin})};
	for (size_t i = 1; i < kinds.size(); ++i) entries.push_back(asset(kinds[i].c_str(), {files[i]}));
	TEST_EXPECT(h.snapshot(release, std::move(entries), files));
	TEST_EXPECT(h.snapshot(other_release, {asset("model", {model})}, {model}));
	TEST_EXPECT(h.snapshot(draft, {asset("model", {model})}, {model}));
	h.publish(product, release); h.publish(other, other_release);
	const int64_t model_id = h.asset_id(release, model.name), draft_id = h.asset_id(draft, model.name);
	TEST_EXPECT(model_id && draft_id);
	const std::string download = "/api/assets/" + std::to_string(model_id) + "/download";
	const std::string file_url = "/api/assets/" + std::to_string(model_id) + "/file";

	// Every kind and a same-named asset from another project appear anonymously, with metadata
	// sufficient for the web table and no source manifests, hashes or object-store locations.
	const auto reply = h.send("GET", "/api/assets");
	const auto all = json_of(reply, 200);
	TEST_EXPECT(all && all["assets"].size() == 6 && all["next_before"].t() == crow::json::type::Null);
	TEST_EXPECT(header_of(reply, "cache-control") == "no-store");
	for (const auto &row : all["assets"]) {
		TEST_EXPECT(!row.has("sha256") && !row.has("bundle_key") && !row.has("files"));
		TEST_EXPECT(row["project"]["owner"].t() == crow::json::type::Null);
		TEST_EXPECT(std::string(row["release"]["version"].s()) == "1.2.3");
		if (row["id"].i() == model_id) {
			TEST_EXPECT(std::string(row["project"]["key"].s()) == "LIBRARY");
			TEST_EXPECT(std::string(row["project"]["name"].s()) == "LIBRARY project");
			TEST_EXPECT(row["release"]["id"].i() == release && row["size"].i() == int64_t(model.bytes.size()));
			TEST_EXPECT(row["file_count"].i() == 2 && row["total_bytes"].i() == int64_t(model.bytes.size() + skin.bytes.size()));
		}
	}
	for (const auto &kind : kinds) {
		const auto filtered = json_of(h.send("GET", "/api/assets?kind=" + kind), 200);
		TEST_EXPECT(filtered && filtered["assets"].size() == (kind == "model" ? 2 : 1));
		for (const auto &row : filtered["assets"]) TEST_EXPECT(std::string(row["kind"].s()) == kind);
	}
	const auto filtered = json_of(h.send("GET", "/api/assets?q=CRATE&kind=model&project=LIBRARY"), 200);
	TEST_EXPECT(filtered && filtered["assets"].size() == 1 && filtered["assets"][0]["id"].i() == model_id);
	for (const char *query : { "q=missing", "q=%25", "project=LIB", "project=DRAFTLIB" }) {
		const auto empty = json_of(h.send("GET", std::string("/api/assets?") + query), 200);
		TEST_EXPECT(empty && empty["assets"].size() == 0 && empty["next_before"].t() == crow::json::type::Null);
	}
	std::vector<int64_t> ids;
	int64_t before = 0;
	for (int page = 0; page < 3; ++page) {
		const auto result = json_of(h.send("GET", "/api/assets?limit=2&before=" + std::to_string(before)), 200);
		TEST_EXPECT(result && result["assets"].size() == 2);
		for (const auto &row : result["assets"]) ids.push_back(row["id"].i());
		if (page < 2) { before = result["next_before"].i(); TEST_EXPECT(before == ids.back()); }
		else TEST_EXPECT(result["next_before"].t() == crow::json::type::Null);
	}
	TEST_EXPECT(std::adjacent_find(ids.begin(), ids.end(), [](int64_t a, int64_t b) { return a <= b; }) == ids.end());
	for (const std::string &query : std::vector<std::string>{"kind=unknown", "limit=0", "limit=101", "limit=one", "before=-1",
			"before=1.5", "before=9223372036854775808", "q=" + std::string(257, 'x'), "project=" + std::string(129, 'x')})
		TEST_EXPECT(refused(h.send("GET", "/api/assets?" + query), 400, "invalid_filter"));

	// Put a real editor revision beside the published native snapshot. A privileged read proves
	// it exists; sharing the asset still grants no anonymous access to its editor source routes.
	const std::string source = h.temp.file("source.bin"), source_manifest = h.temp.file("source.json");
	const std::string manifest = opennova::project::encode_manifest({{"art/crate.3di", model.sha(), model.bytes.size()}});
	const std::string manifest_sha = io::sha256_hex(manifest.data(), manifest.size());
	TEST_EXPECT(io::write_file_atomic(source, model.bytes, h.error) && io::write_file_atomic(source_manifest, manifest, h.error));
	TEST_EXPECT(h.store.put_file(projects::blob_key(product, model.sha()), source, h.error));
	TEST_EXPECT(h.store.put_file(projects::manifest_key(product, manifest_sha), source_manifest, h.error));
	projects::record_blobs(h.db, product, {{model.sha(), int64_t(model.bytes.size())}});
	projects::NewRevision revision;
	revision.created_by = "asset routes test"; revision.manifest_sha256 = manifest_sha;
	revision.message = "Private source for the published asset";
	revision.file_count = 1; revision.bytes = int64_t(model.bytes.size());
	int64_t head = 0;
	TEST_EXPECT(projects::commit_revision(h.db, product, 0, revision, head) == projects::CommitOutcome::Ok);
	for (const std::string &route : std::vector<std::string>{"/api/products/LIBRARY", "/api/products/LIBRARY/revisions/1/manifest",
			"/api/products/LIBRARY/blobs/" + model.sha()}) {
		TEST_EXPECT(h.send("GET", route, {kBearer}).code == 200);
		TEST_EXPECT(refused(h.send("GET", route), 404, "not_found"));
	}

	// An anonymous click queues one bundle. Polling remains inexpensive beyond the new-download
	// rate limit, then the worker's complete ZIP is served with its user-facing filename.
	TEST_EXPECT(refused(h.send("POST", "/api/assets/99999999/download"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("POST", "/api/assets/" + std::to_string(draft_id) + "/download"), 404, "not_found"));
	TEST_EXPECT(refused(h.send("GET", file_url), 404, "not_found"));
	TEST_EXPECT(refused(h.send("POST", download, {"Content-Type: application/json"}, "{"), 400, "invalid_json"));
	TEST_EXPECT(refused(h.send("POST", download, {"Content-Type: application/json"}, "{\"retry\":\"true\"}"), 400, "invalid_field"));
	for (int i = 0; i < 25; ++i) {
		const auto pending = json_of(h.send("POST", download, {"Content-Type: application/json"}, "{}"), 202);
		TEST_EXPECT(pending && std::string(pending["state"].s()) == "preparing");
	}
	TEST_EXPECT(asset_bundle_pending(h.db, model_id));
	TEST_EXPECT(h.db.query("SELECT count(*) FROM jobs WHERE kind='asset.bundle'")[0].as_int(0).value_or(0) == 1);
	nws::jobs::JobRegistry registry;
	register_asset_bundle(registry, h.store);
	TEST_EXPECT(nws::jobs::run_due(h.pool, registry) == 1);
	const auto ready = json_of(h.send("POST", download), 200);
	const std::string filename = "LIBRARY-r" + std::to_string(release) + "-crate.3di.zip";
	TEST_EXPECT(ready && std::string(ready["state"].s()) == "ready" && std::string(ready["url"].s()) == file_url);
	TEST_EXPECT(std::string(ready["filename"].s()) == filename);
	const auto zip = h.send("GET", file_url);
	TEST_EXPECT(zip.transport_ok && zip.code == 200 && zip.body.size() > 4 && zip.body[0] == 'P' && zip.body[1] == 'K');
	TEST_EXPECT(header_of(zip, "content-type") == "application/zip" && header_of(zip, "cache-control") == "no-store");
	TEST_EXPECT(header_of(zip, "content-disposition") == "attachment; filename=\"" + filename + "\"");
	const std::string archive = h.temp.file("bundle.zip"), unpacked = h.temp.file("unpacked");
	TEST_EXPECT(io::write_file_atomic(archive, body_text(zip), h.error) && nws::unzip_files(archive, unpacked, h.error));
	for (const auto &file : {model, skin}) {
		std::string bytes;
		TEST_EXPECT(io::read_file_text(io::utf8_join(unpacked, "files/" + file.name), bytes, h.error) && bytes == file.bytes);
	}
	std::string bundle_manifest;
	TEST_EXPECT(io::read_file_text(io::utf8_join(unpacked, "manifest.json"), bundle_manifest, h.error));
	const auto metadata = crow::json::load(bundle_manifest);
	TEST_EXPECT(metadata && std::string(metadata["asset"].s()) == model.name && metadata["release_id"].i() == release);

	// A stored failure is a bounded error, not an automatic requeue loop. A fresh user click
	// explicitly retries once, after which ordinary polling can observe the completed bundle.
	const int64_t failed_id = h.asset_id(release, "walk.bad");
	const std::string failed_url = "/api/assets/" + std::to_string(failed_id) + "/download";
	h.db.exec("UPDATE asset_entries SET bundle_error='private storage diagnostics' WHERE id=$1", {failed_id});
	const auto failed = h.send("POST", failed_url);
	TEST_EXPECT(refused(failed, 503, "bundle_failed") && body_text(failed).find("private storage") == std::string::npos);
	TEST_EXPECT(!asset_bundle_pending(h.db, failed_id));
	const auto retry = json_of(h.send("POST", failed_url, {"Content-Type: application/json"}, "{\"retry\":true}"), 202);
	TEST_EXPECT(retry && std::string(retry["state"].s()) == "preparing");
	// Losing the worker on its last attempt (or an admin cancellation) can leave only a failed
	// jobs row. Ordinary polls must stop even though no worker wrote a bundle_error.
	TEST_EXPECT(find_library_asset(h.db, failed_id)->bundle_error.empty());
	const std::string failed_key = "asset.bundle:" + std::to_string(failed_id);
	h.db.exec("UPDATE jobs SET state='failed',finished_at=now(),last_error='worker lease expired' "
			"WHERE dedupe_key=$1 AND state='queued'", {failed_key});
	TEST_EXPECT(h.db.changes() == 1 && asset_bundle_failed(h.db, failed_id));
	const auto job_count = h.db.query("SELECT count(*) FROM jobs WHERE dedupe_key=$1", {failed_key})[0].as_int(0);
	for (int poll = 0; poll < 3; ++poll)
		TEST_EXPECT(refused(h.send("POST", failed_url), 503, "bundle_failed"));
	TEST_EXPECT(!asset_bundle_pending(h.db, failed_id));
	TEST_EXPECT(h.db.query("SELECT count(*) FROM jobs WHERE dedupe_key=$1", {failed_key})[0].as_int(0) == job_count);
	const auto explicit_retry = json_of(h.send("POST", failed_url, {"Content-Type: application/json"}, "{\"retry\":true}"), 202);
	TEST_EXPECT(explicit_retry && std::string(explicit_retry["state"].s()) == "preparing");
	TEST_EXPECT(!asset_bundle_failed(h.db, failed_id) && asset_bundle_pending(h.db, failed_id));
	TEST_EXPECT(json_of(h.send("POST", failed_url), 202));
	// An admin can retry the older row in place. Its active work wins over both a newer failed
	// row and an old asset error; after it completes, the artifact wins over either error too.
	const auto attempts = h.db.query("SELECT id FROM jobs WHERE dedupe_key=$1 ORDER BY id", {failed_key});
	TEST_EXPECT(attempts.size() == 2);
	h.db.exec("UPDATE jobs SET state='failed',finished_at=now() WHERE dedupe_key=$1 AND state='queued'", {failed_key});
	h.db.exec("UPDATE jobs SET state='queued',attempts=0,finished_at=NULL,run_at=now() WHERE id=$1",
			{attempts.front().as_int(0).value()});
	h.db.exec("UPDATE asset_entries SET bundle_error='stale diagnostics' WHERE id=$1", {failed_id});
	TEST_EXPECT(asset_bundle_failed(h.db, failed_id) && asset_bundle_pending(h.db, failed_id));
	TEST_EXPECT(json_of(h.send("POST", failed_url), 202));
	TEST_EXPECT(nws::jobs::run_due(h.pool, registry) == 1);
	TEST_EXPECT(asset_bundle_failed(h.db, failed_id));
	h.db.exec("UPDATE asset_entries SET bundle_error='stale diagnostics' WHERE id=$1", {failed_id});
	TEST_EXPECT(json_of(h.send("POST", failed_url), 200));

	// Revoking a base contributor hides just the closures using it, including a cached file URL.
	h.share(base->id, false);
	const auto revoked = json_of(h.send("GET", "/api/assets?q=crate&project=LIBRARY"), 200);
	TEST_EXPECT(revoked && revoked["assets"].size() == 0);
	TEST_EXPECT(refused(h.send("POST", download), 404, "not_found") && refused(h.send("GET", file_url), 404, "not_found"));
	h.share(base->id, true);
	TEST_EXPECT(h.send("GET", file_url).body == zip.body);
	h.share(product, false);
	const auto hidden_list = json_of(h.send("GET", "/api/assets?project=LIBRARY"), 200);
	TEST_EXPECT(hidden_list && hidden_list["assets"].size() == 0);
	TEST_EXPECT(refused(h.send("POST", download), 404, "not_found") && refused(h.send("GET", file_url), 404, "not_found"));
	h.share(product, true);
	h.db.exec("UPDATE releases SET status='retired' WHERE id=$1", {release});
	TEST_EXPECT(refused(h.send("POST", download), 404, "not_found") && refused(h.send("GET", file_url), 404, "not_found"));
	const auto retired_list = json_of(h.send("GET", "/api/assets?project=LIBRARY"), 200);
	TEST_EXPECT(retired_list && retired_list["assets"].size() == 0);
	h.db.exec("UPDATE releases SET status='published' WHERE id=$1", {release});
	bool limited = false;
	for (int i = 0; i < 30; ++i) {
		const auto result = h.send("POST", download);
		if (result.code == 429) {
			TEST_EXPECT(refused(result, 429, "rate_limited") && !header_of(result, "retry-after").empty());
			limited = true; break;
		}
		TEST_EXPECT(json_of(result, 200));
	}
	TEST_EXPECT(limited);
	return 0;
}
} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("asset_routes");
		test_db::TestDatabase database;
		db::ConnectionPool pool(database.options());
		auto db = database.open();
		test_redis::TestRedis redis;
		nws::LocalObjectStore store(temp.file("store"));
		nws::ServerConfig config;
		config.public_host = "127.0.0.1"; config.web_host = "127.0.0.1";
		config.admin_api_token = kAdminToken; config.http_port = 0;
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates"; config.static_dir = temp.path / "static";
		std::filesystem::create_directories(config.templates_dir);
		nws::HttpListener http(pool, redis.pool());
		http.set_object_store(&store);
		TEST_EXPECT(http.start(config) && http.bound_port() != 0);
		Harness harness{temp, pool, db, store, http.bound_port(), {}};
		rc = test_routes(harness);
		http.stop();
	}
	net::shutdown();
	std::printf("asset_routes: public catalog, private sources, prepared ZIPs and revocation %s\n", rc ? "FAILED" : "ok");
	return rc;
}
