#include "project_routes.h"

#include "core/loggable.h"
#include "core/object_store.h"
#include "http_json.h"
#include "products/product_repository.h"
#include "products/project_repository.h"
#include "products/release_build.h"
#include "products/release_routes.h"
#include "project_urls.h"

#include <base/content/content_manifest.h>
#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::content {

namespace {

using opennova::content::ContentFile;
using hosted::ProductRow;

// A working copy bound to a key renamed since (or a game deleted) reads this: its binding names
// the old key, so the editor's NovaWorld window binds it again.
crow::response no_such_project() {
	return json_error(404, "not_found",
			"no game's project has this key (a renamed key or a deleted game: bind the project "
			"again)");
}

// A push and an upload are an admin's own: the Bearer token, a machine's, names no author.
crow::response session_required() {
	return json_error(403, "session_required", "a push is an admin's own: sign in to push");
}

// A release's version: one to four numbers between dots ("0.2.0", "1.7.5.8"), as a command
// file's VERSION reads.
bool valid_version(std::string_view version) {
	if (version.empty() || version.size() > 32)
		return false;
	int dots = 0;
	size_t digits = 0;
	for (const char c : version) {
		if (c >= '0' && c <= '9') {
			++digits;
			continue;
		}
		if (c != '.' || digits == 0)
			return false;
		++dots;
		digits = 0;
	}
	return digits > 0 && dots < 4;
}

// A revision's message: UTF-8 with no control character but a line break or a tab.
bool valid_message(std::string_view message) {
	if (message.empty() || message.size() > ContentRoutes::kMessageMax ||
			!opennova::content::is_utf8(message) || has_c1_control(message))
		return false;
	for (const char c : message) {
		const auto byte = static_cast<unsigned char>(c);
		if ((byte < 0x20 && c != '\n' && c != '\t') || byte == 0x7F)
			return false;
	}
	return true;
}

// The caching a reply of `project`'s immutable bytes takes (a manifest, a file: each named by its
// SHA-256): anyone's for a public project, the browser's own for a private one.
std::string immutable_cache(const ProductRow &project) {
	return project_is_public(project) ? "public, max-age=31536000, immutable"
													: "private, max-age=31536000, immutable";
}

// The project `key` names as a caller (`admin` or not) may read it: none for a private one asked
// for by anyone but an admin.
std::optional<ProductRow> readable(db::Database &db, const std::string &key, bool admin) {
	auto project = hosted::find_product_by_key(db, key);
	if (!project || (!project_is_public(*project) && !admin))
		return std::nullopt;
	return project;
}

// The files of `unrecorded` the project's store holds at their sizes (each an upload through the
// store's own URL, which no request records) into `found`, the rest into `absent`; false with
// `error` when the store fails. A few are asked of the store one by one; more by one listing of
// the project's files, since each store call is a process of the AWS CLI's (a first push of a
// retail game names thousands).
bool sort_by_store(ObjectStore &store, int64_t product_id,
		const std::vector<StoredBlob> &unrecorded, std::vector<StoredBlob> &found,
		std::vector<StoredBlob> &absent, std::string &error) {
	const bool listing = unrecorded.size() > ContentRoutes::kHeadsAtMost;
	std::unordered_map<std::string, uint64_t> stored; // key -> size, when listed
	if (listing) {
		std::vector<StoredObject> objects;
		if (!store.list(project_prefix(product_id) + "blobs/", objects, error))
			return false;
		for (const StoredObject &object : objects)
			stored.emplace(object.key, object.size);
	}
	for (const StoredBlob &blob : unrecorded) {
		const std::string object = blob_key(product_id, blob.sha256);
		bool exists = false;
		uint64_t size = 0;
		if (listing) {
			const auto at = stored.find(object);
			exists = at != stored.end();
			size = exists ? at->second : 0;
		} else if (!store.head(object, exists, size, error)) {
			return false;
		}
		(exists && size == static_cast<uint64_t>(blob.bytes) ? found : absent).push_back(blob);
	}
	return true;
}

// A transfer as the editor makes it: {method, url, headers: {name: value}}.
crow::json::wvalue transfer_json(const SignedTransfer &t) {
	crow::json::wvalue out;
	out["method"] = t.method;
	out["url"] = t.url;
	out["headers"] = crow::json::wvalue::object();
	for (const auto &[name, value] : t.headers)
		out["headers"][name] = value;
	return out;
}

crow::json::wvalue revision_json(const ContentRevision &r) {
	crow::json::wvalue out;
	out["number"] = r.number;
	out["author"] = r.created_by;
	out["message"] = r.message;
	out["manifest_sha256"] = r.manifest_sha256;
	out["file_count"] = r.file_count;
	out["bytes"] = r.bytes;
	out["created_at"] = r.created_at;
	return out;
}

// A project as the routes answer it: {key, title (its game's name), visibility, retail, base (an
// expansion's base game's project key, or null), product_id (its game's), head, can_push}, and
// for an admin its storage ({files, bytes}). `keys` maps every game with a project to its key.
crow::json::wvalue project_json(db::Database &db, const ProductRow &p, bool admin,
		const std::unordered_map<int64_t, std::string> &keys) {
	crow::json::wvalue out;
	out["key"] = p.key;
	out["title"] = p.display_name;
	out["visibility"] = p.visibility;
	out["retail"] = p.retail;
	const auto base = p.parent_id ? keys.find(*p.parent_id) : keys.end();
	if (base != keys.end())
		out["base"] = base->second;
	else
		out["base"] = nullptr;
	out["product_id"] = p.id;
	out["head"] = p.head;
	out["can_push"] = admin;
	if (admin) {
		const ProjectStorage storage = project_storage(db, p.id);
		out["storage"]["files"] = storage.blobs;
		out["storage"]["bytes"] = storage.bytes;
	}
	return out;
}

std::unordered_map<int64_t, std::string> project_keys(db::Database &db) {
	std::unordered_map<int64_t, std::string> keys;
	for (const ProductRow &p : hosted::list_projects(db, true))
		keys.emplace(p.id, p.key);
	return keys;
}

// A whole number from the query string; `fallback` when it is absent, nullopt when it is no
// number in [0, max].
std::optional<int64_t> query_whole(
		const crow::request &req, const char *name, int64_t fallback, int64_t max) {
	const char *text = req.url_params.get(name);
	if (text == nullptr)
		return fallback;
	const auto value = strutil::parse_ulong(text);
	if (!value || *value > static_cast<unsigned long>(max))
		return std::nullopt;
	return static_cast<int64_t>(*value);
}

// A scratch file removed when the request is done with it.
struct ScratchFile {
	std::string path;
	~ScratchFile() {
		if (path.empty())
			return;
		std::error_code ec;
		std::filesystem::remove(io::os_path(path), ec);
	}
};

// The object at `key` read whole into `out` through a scratch file; false with `error`.
bool read_object(ObjectStore &store, const std::string &key, const std::string &scratch,
		std::string &out, std::string &error) {
	ScratchFile file{ scratch };
	if (!store.get_file(key, file.path, error))
		return false;
	return io::read_file_text(file.path, out, error);
}

// `bytes` put at `key` through a scratch file; false with `error`.
bool write_object(ObjectStore &store, const std::string &key, const std::string &scratch,
		const std::string &bytes, std::string &error) {
	ScratchFile file{ scratch };
	return io::write_file_atomic(file.path, bytes, error) && store.put_file(key, file.path, error);
}

} // namespace

std::string ContentRoutes::scratch_file(const char *what, std::string &error) {
	if (scratch_.empty()) {
		error = "no scratch directory";
		return {};
	}
	const uint64_t n = next_scratch_.fetch_add(1);
	return io::utf8_path(scratch_.path() / (std::string(what) + "-" + std::to_string(n)));
}

void ContentRoutes::register_routes(WebApp &app) {
	std::string error;
	scratch_ = ScratchDir::make("onnet-content-", error);
	if (scratch_.empty())
		std::fprintf(stderr,
				"[http] WARN content routes: no scratch directory (%s); their file routes "
				"answer 503\n",
				error.c_str());

	CROW_ROUTE(app, "/api/content/projects")
			.methods("GET"_method)([this](const crow::request &req) { return list(req); });
	CROW_ROUTE(app, "/api/content/projects/<string>")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key) {
				return project(req, key);
			});
	CROW_ROUTE(app, "/api/content/projects/<string>/revisions")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key) {
				return revisions(req, key);
			});
	CROW_ROUTE(app, "/api/content/projects/<string>/revisions/<uint>/manifest")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key,
										   uint64_t number) { return manifest(req, key, number); });
	CROW_ROUTE(app, "/api/content/projects/<string>/blobs/<string>")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key,
										   const std::string &sha) { return blob(req, key, sha); });
	CROW_ROUTE(app, "/api/content/projects/<string>/downloads")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return downloads(req, key);
			});
	CROW_ROUTE(app, "/api/content/projects/<string>/missing")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return missing(req, key);
			});
	CROW_ROUTE(app, "/api/content/projects/<string>/blobs/<string>")
			.methods("PUT"_method)(
					[this](const crow::request &req, const std::string &key,
							const std::string &sha) { return upload(req, key, sha); });
	CROW_ROUTE(app, "/api/content/projects/<string>/revisions")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return push(req, key);
			});
	CROW_ROUTE(app, "/api/admin/content/projects/<string>/releases")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return release(req, key);
			});
}

// GET /api/content/projects — {"projects"} by key: the public ones, and for an admin every one.
crow::response ContentRoutes::list(const crow::request &req) {
	const bool admin = access_.caller_is_admin(req);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto keys = project_keys(*db_conn);
		std::vector<crow::json::wvalue> projects;
		for (const ProductRow &p : hosted::list_projects(*db_conn, admin))
			projects.push_back(project_json(*db_conn, p, admin, keys));
		out["projects"] = std::move(projects);
	} catch (const db::Error &e) {
		return db_failure("GET /api/content/projects", e);
	}
	return no_store(json_reply(200, out));
}

// GET /api/content/projects/<key> — {"project", "head_revision" (null before the first push)};
// 404 not_found.
crow::response ContentRoutes::project(const crow::request &req, const std::string &key) {
	const bool admin = access_.caller_is_admin(req);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto p = readable(*db_conn, key, admin);
		if (!p)
			return no_such_project();
		out["project"] = project_json(*db_conn, *p, admin, project_keys(*db_conn));
		const auto head = p->head > 0 ? find_revision(*db_conn, p->id, p->head) : std::nullopt;
		if (head)
			out["head_revision"] = revision_json(*head);
		else
			out["head_revision"] = nullptr;
	} catch (const db::Error &e) {
		return db_failure("GET /api/content/projects/<key>", e);
	}
	return no_store(json_reply(200, out));
}

// GET /api/content/projects/<key>/revisions?before=<n>&limit=<m> — {"revisions"}: at most `limit`
// (50, at most 200) of those numbered below `before` (0, or none: from the head), newest first;
// 400 invalid_query; 404 not_found.
crow::response ContentRoutes::revisions(const crow::request &req, const std::string &key) {
	const bool admin = access_.caller_is_admin(req);
	const auto before = query_whole(req, "before", 0, INT64_MAX);
	const auto limit = query_whole(req, "limit", kPageDefault, kPageMax);
	if (!before || !limit || *limit == 0)
		return json_error(400, "invalid_query",
				"before is a revision's number; limit is 1 to " + std::to_string(kPageMax));
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto p = readable(*db_conn, key, admin);
		if (!p)
			return no_such_project();
		std::vector<crow::json::wvalue> list;
		for (const ContentRevision &r :
				list_revisions(*db_conn, p->id, *before, static_cast<int>(*limit)))
			list.push_back(revision_json(r));
		out["revisions"] = std::move(list);
	} catch (const db::Error &e) {
		return db_failure("GET /api/content/projects/<key>/revisions", e);
	}
	return no_store(json_reply(200, out));
}

// GET /api/content/projects/<key>/revisions/<n>/manifest — the manifest's canonical text
// (base/content/content_manifest.h), cached as immutable; 404 not_found; 503 store_unavailable.
crow::response ContentRoutes::manifest(
		const crow::request &req, const std::string &key, uint64_t number) {
	const bool admin = access_.caller_is_admin(req);
	if (store_ == nullptr)
		return store_unavailable();
	std::optional<ProductRow> p;
	std::optional<ContentRevision> revision;
	try {
		auto db_conn = pool_.acquire();
		p = readable(*db_conn, key, admin);
		if (p)
			revision = find_revision(*db_conn, p->id, path_id(number));
	} catch (const db::Error &e) {
		return db_failure("GET /api/content/projects/<key>/revisions/<n>/manifest", e);
	}
	if (!p)
		return no_such_project();
	if (!revision)
		return json_error(404, "not_found", "the project has no revision of this number");
	std::string error;
	const std::string scratch = scratch_file("manifest", error);
	std::string text;
	if (scratch.empty() ||
			!read_object(
					*store_, manifest_key(p->id, revision->manifest_sha256), scratch, text, error))
		return store_failure("GET /api/content/projects/<key>/revisions/<n>/manifest", error);
	crow::response res(200);
	res.body = std::move(text);
	res.set_header("Content-Type", "application/json");
	res.set_header("Cache-Control", immutable_cache(*p));
	res.set_header("ETag", "\"" + revision->manifest_sha256 + "\"");
	return res;
}

// GET /api/content/projects/<key>/blobs/<sha256> — the file's bytes, cached as immutable; 400
// invalid_sha256; 404 not_found (the project, or a file its store does not hold); 503
// store_unavailable.
crow::response ContentRoutes::blob(
		const crow::request &req, const std::string &key, const std::string &sha) {
	const bool admin = access_.caller_is_admin(req);
	if (!opennova::content::is_sha256_hex(sha))
		return json_error(
				400, "invalid_sha256", "a file is named by its SHA-256 in lower-case hex");
	if (store_ == nullptr)
		return store_unavailable();
	std::optional<ProductRow> p;
	try {
		p = readable(*pool_.acquire(), key, admin);
	} catch (const db::Error &e) {
		return db_failure("GET /api/content/projects/<key>/blobs/<sha256>", e);
	}
	if (!p)
		return no_such_project();
	const std::string object = blob_key(p->id, sha);
	if (urls_ != nullptr) {
		// No file passes through this program when the store signs its own URLs (ADR 0069 d6).
		SignedTransfer download;
		std::string error;
		if (!urls_->get(object, download, error))
			return store_failure("GET /api/content/projects/<key>/blobs/<sha256>", error);
		crow::response res(302);
		res.set_header("Location", download.url);
		res.set_header("Cache-Control", "no-store");
		return res;
	}
	bool exists = false;
	uint64_t size = 0;
	std::string error;
	if (!store_->head(object, exists, size, error))
		return store_failure("GET /api/content/projects/<key>/blobs/<sha256>", error);
	if (!exists)
		return json_error(404, "not_found", "the project's store holds no file of this SHA-256");
	if (size > kBodyMax)
		return json_error(413, "too_large", "the file is too large to send through this program");
	const std::string scratch = scratch_file("blob", error);
	std::string bytes;
	if (scratch.empty() || !read_object(*store_, object, scratch, bytes, error))
		return store_failure("GET /api/content/projects/<key>/blobs/<sha256>", error);
	crow::response res(200);
	res.body = std::move(bytes);
	res.set_header("Content-Type", "application/octet-stream");
	res.set_header("Cache-Control", immutable_cache(*p));
	res.set_header("ETag", "\"" + sha + "\"");
	return res;
}

// POST /api/content/projects/<key>/missing — {"blobs": [{"sha256", "size"}]} (at most 20000):
// {"missing": [{"sha256", "size", "upload": {"method", "url", "headers"}}]}, the files the
// project's store does not hold at their sizes, each with the request that uploads it; 400
// invalid_json, invalid_field; 403 session_required; 404 not_found.
crow::response ContentRoutes::missing(const crow::request &req, const std::string &key) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (!caller.user)
		return session_required();
	const auto body = crow::json::load(req.body);
	if (!body || body.t() != crow::json::type::Object || !body.has("blobs") ||
			body["blobs"].t() != crow::json::type::List)
		return json_error(400, "invalid_json", "the body is {\"blobs\": [{\"sha256\", \"size\"}]}");
	std::vector<StoredBlob> wanted;
	for (const auto &item : body["blobs"]) {
		if (wanted.size() == kMissingMax)
			return field_error(400, "invalid_field", "blobs",
					"at most " + std::to_string(kMissingMax) + " files a request");
		if (item.t() != crow::json::type::Object || !item.has("sha256") || !item.has("size") ||
				item["sha256"].t() != crow::json::type::String ||
				item["size"].t() != crow::json::type::Number ||
				item["size"].nt() == crow::json::num_type::Floating_point)
			return field_error(400, "invalid_field", "blobs", "each file is {sha256, size}");
		StoredBlob blob{ std::string(item["sha256"].s()), item["size"].i() };
		if (!opennova::content::is_sha256_hex(blob.sha256) || blob.bytes < 0 ||
				static_cast<uint64_t>(blob.bytes) > opennova::content::kMaxFileBytes)
			return field_error(400, "invalid_field", "blobs",
					"each sha256 is 64 lower-case hex digits, each size 0 to 4 GiB");
		wanted.push_back(std::move(blob));
	}
	std::vector<StoredBlob> absent;
	int64_t product_id = 0;
	try {
		auto db_conn = pool_.acquire();
		const auto p = hosted::find_product_by_key(*db_conn, key);
		if (!p)
			return no_such_project();
		product_id = p->id;
		absent = missing_blobs(*db_conn, p->id, wanted);
		// A file an earlier push uploaded through the store's own URL but never committed (an
		// upload past its URL's hour, a push that failed) is held once the store has it, so a push
		// again uploads only the rest.
		if (store_ != nullptr && !absent.empty()) {
			std::vector<StoredBlob> found, still;
			std::string error;
			if (!sort_by_store(*store_, p->id, absent, found, still, error))
				return store_failure("POST /api/content/projects/<key>/missing", error);
			record_blobs(*db_conn, p->id, found);
			absent = std::move(still);
		}
	} catch (const db::Error &e) {
		return db_failure("POST /api/content/projects/<key>/missing", e);
	}
	std::vector<crow::json::wvalue> list;
	for (const StoredBlob &blob : absent) {
		crow::json::wvalue entry;
		entry["sha256"] = blob.sha256;
		entry["size"] = blob.bytes;
		SignedTransfer upload;
		std::string error;
		if (urls_ != nullptr) {
			if (!urls_->put(blob_key(product_id, blob.sha256), blob.sha256, upload, error))
				return store_failure("POST /api/content/projects/<key>/missing", error);
		} else {
			upload = { "PUT", "/api/content/projects/" + key + "/blobs/" + blob.sha256,
				{ { "Content-Type", "application/octet-stream" } } };
		}
		entry["upload"] = transfer_json(upload);
		list.push_back(std::move(entry));
	}
	crow::json::wvalue out;
	out["missing"] = std::move(list);
	return no_store(json_reply(200, out));
}

// POST /api/content/projects/<key>/downloads — {"hashes": [sha256]} (at most 1000): {"downloads":
// [{"sha256", "method", "url", "headers"}]}, each file's download: the store's own signed URL, or
// this program's blob route. Anyone who may read the project; 400 invalid_json, invalid_field; 404
// not_found.
crow::response ContentRoutes::downloads(const crow::request &req, const std::string &key) {
	const bool admin = access_.caller_is_admin(req);
	const auto body = crow::json::load(req.body);
	if (!body || body.t() != crow::json::type::Object || !body.has("hashes") ||
			body["hashes"].t() != crow::json::type::List)
		return json_error(400, "invalid_json", "the body is {\"hashes\": [sha256]}");
	std::vector<std::string> hashes;
	for (const auto &item : body["hashes"]) {
		if (hashes.size() == kDownloadsMax)
			return field_error(400, "invalid_field", "hashes",
					"at most " + std::to_string(kDownloadsMax) + " files a request");
		if (item.t() != crow::json::type::String ||
				!opennova::content::is_sha256_hex(std::string(item.s())))
			return field_error(
					400, "invalid_field", "hashes", "each hash is 64 lower-case hex digits");
		hashes.emplace_back(item.s());
	}
	int64_t product_id = 0;
	try {
		const auto p = readable(*pool_.acquire(), key, admin);
		if (!p)
			return no_such_project();
		product_id = p->id;
	} catch (const db::Error &e) {
		return db_failure("POST /api/content/projects/<key>/downloads", e);
	}
	std::vector<crow::json::wvalue> list;
	for (const std::string &sha : hashes) {
		SignedTransfer download;
		std::string error;
		if (urls_ != nullptr) {
			if (!urls_->get(blob_key(product_id, sha), download, error))
				return store_failure("POST /api/content/projects/<key>/downloads", error);
		} else {
			download = { "GET", "/api/content/projects/" + key + "/blobs/" + sha, {} };
		}
		crow::json::wvalue entry = transfer_json(download);
		entry["sha256"] = sha;
		list.push_back(std::move(entry));
	}
	crow::json::wvalue out;
	out["downloads"] = std::move(list);
	return no_store(json_reply(200, out));
}

// PUT /api/content/projects/<key>/blobs/<sha256> — the file's bytes, put in the project's store
// and held there once their SHA-256 is the one named: 200 {"sha256", "size"}; 400 invalid_sha256,
// sha256_mismatch; 403 session_required; 404 not_found; 413 too_large; 503 store_unavailable.
crow::response ContentRoutes::upload(
		const crow::request &req, const std::string &key, const std::string &sha) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (!caller.user)
		return session_required();
	if (!opennova::content::is_sha256_hex(sha))
		return json_error(
				400, "invalid_sha256", "a file is named by its SHA-256 in lower-case hex");
	if (req.body.size() > kBodyMax)
		return json_error(413, "too_large", "the file is too large to send through this program");
	if (io::sha256_hex(req.body.data(), req.body.size()) != sha)
		return json_error(400, "sha256_mismatch", "the bytes sent are not the file named");
	if (store_ == nullptr)
		return store_unavailable();
	try {
		auto db_conn = pool_.acquire();
		const auto p = hosted::find_product_by_key(*db_conn, key);
		if (!p)
			return no_such_project();
		std::string error;
		const std::string scratch = scratch_file("upload", error);
		if (scratch.empty() ||
				!write_object(*store_, blob_key(p->id, sha), scratch, req.body, error))
			return store_failure("PUT /api/content/projects/<key>/blobs/<sha256>", error);
		record_blobs(*db_conn, p->id, { { sha, static_cast<int64_t>(req.body.size()) } });
	} catch (const db::Error &e) {
		return db_failure("PUT /api/content/projects/<key>/blobs/<sha256>", e);
	}
	crow::json::wvalue out;
	out["sha256"] = sha;
	out["size"] = static_cast<int64_t>(req.body.size());
	return no_store(json_reply(200, out));
}

// POST /api/content/projects/<key>/revisions — {"parent", "message", "manifest"}: the manifest
// (its canonical text, base/content/content_manifest.h) pushed as the revision after `parent`,
// 201 {"revision", "head"}; 400 invalid_json, missing_field, invalid_field, invalid_manifest,
// unchanged, missing_blobs {"hashes"}; 403 session_required; 404 not_found; 409 stale {"head"}; 503
// store_unavailable.
crow::response ContentRoutes::push(const crow::request &req, const std::string &key) {
	static constexpr const char *kRoute = "POST /api/content/projects/<key>/revisions";
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (!caller.user)
		return session_required();
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto parent = body->integer("parent");
	const auto message = body->text("message");
	const auto manifest_text = body->text("manifest");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!parent || !message || !manifest_text)
		return json_error(400, "missing_field", "a push is {parent, message, manifest}");
	if (*parent < 0)
		return field_error(
				400, "invalid_field", "parent", "parent is a revision's number, 0 for none");
	if (!valid_message(*message))
		return field_error(400, "invalid_field", "message",
				"message is 1 to " + std::to_string(kMessageMax) +
						" bytes of UTF-8 with no control character but a line break or a tab");
	std::vector<ContentFile> files;
	std::string why;
	if (!opennova::content::decode_manifest(*manifest_text, files, why))
		return field_error(400, "invalid_manifest", "manifest", why);
	const std::string manifest_sha = opennova::content::manifest_sha256(*manifest_text);
	if (store_ == nullptr)
		return store_unavailable();
	try {
		auto db_conn = pool_.acquire();
		const auto p = hosted::find_product_by_key(*db_conn, key);
		if (!p)
			return no_such_project();
		if (p->head != *parent) {
			crow::json::wvalue stale;
			stale["error"] = "stale";
			stale["head"] = p->head;
			return json_reply(409, stale);
		}
		if (p->head > 0) {
			const auto head = find_revision(*db_conn, p->id, p->head);
			if (head && head->manifest_sha256 == manifest_sha)
				return json_error(400, "unchanged", "the manifest lists the head's files");
		}
		// The files the store does not hold yet: each one an upload put but no request recorded (an
		// upload through the store's own URL) is held once the store has it at its size.
		std::vector<StoredBlob> listed;
		listed.reserve(files.size());
		for (const ContentFile &file : files)
			listed.push_back({ file.sha256, static_cast<int64_t>(file.size) });
		std::vector<StoredBlob> found, absent;
		std::string held_error;
		if (!sort_by_store(*store_, p->id, missing_blobs(*db_conn, p->id, listed), found, absent,
					held_error))
			return store_failure(kRoute, held_error);
		record_blobs(*db_conn, p->id, found);
		if (!absent.empty()) {
			crow::json::wvalue refusal;
			refusal["error"] = "missing_blobs";
			refusal["message"] = "the store does not hold every file the manifest lists";
			std::vector<crow::json::wvalue> hashes;
			for (const StoredBlob &blob : absent)
				hashes.emplace_back(blob.sha256);
			refusal["hashes"] = std::move(hashes);
			return json_reply(400, refusal);
		}
		std::string error;
		const std::string scratch = scratch_file("manifest", error);
		if (scratch.empty() ||
				!write_object(*store_, manifest_key(p->id, manifest_sha), scratch, *manifest_text,
						error))
			return store_failure(kRoute, error);
		NewRevision revision;
		revision.author_id = caller.user->id;
		revision.created_by = caller.user->username;
		revision.message = *message;
		revision.manifest_sha256 = manifest_sha;
		revision.file_count = static_cast<int64_t>(files.size());
		revision.bytes = static_cast<int64_t>(opennova::content::total_bytes(files));
		int64_t head = 0;
		const CommitOutcome outcome = commit_revision(*db_conn, p->id, *parent, revision, head);
		if (outcome == CommitOutcome::NotFound)
			return no_such_project();
		if (outcome == CommitOutcome::Stale) {
			crow::json::wvalue stale;
			stale["error"] = "stale";
			stale["head"] = head;
			return json_reply(409, stale);
		}
		const auto pushed = find_revision(*db_conn, p->id, head);
		std::printf("[http] %s: %s r%lld by '%s', %zu file(s)\n", kRoute, p->key.c_str(),
				static_cast<long long>(head), loggable(caller.user->username).c_str(),
				files.size());
		crow::json::wvalue out;
		if (pushed)
			out["revision"] = revision_json(*pushed);
		out["head"] = head;
		return no_store(json_reply(201, out));
	} catch (const db::Error &e) {
		return db_failure(kRoute, e);
	}
}

// POST /api/admin/content/projects/<key>/releases — {"version", "revision"?, "base_revision"?}: a
// release of the game the key names that the data program builds (products/release_build.h)
// from the revision (the head when none is named) and, for an expansion's, its base game's (its
// head), its package's version `version`: 201 {"release"}, building, its release.build queued in
// the same transaction; 400 invalid_json, invalid_field, no_revision (no such revision, none
// pushed yet, or a base game with no project); 404 not_found; 503 store_unavailable.
crow::response ContentRoutes::release(const crow::request &req, const std::string &key) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (store_ == nullptr)
		return store_unavailable();
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto version = body->text("version");
	const auto revision = body->null("revision") ? std::nullopt : body->integer("revision");
	const auto base_revision =
			body->null("base_revision") ? std::nullopt : body->integer("base_revision");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!version || !valid_version(*version))
		return field_error(400, "invalid_field", "version",
				"a version is one to four numbers between dots: 0.2.0, 1.7.5.8");
	try {
		auto db_conn = pool_.acquire();
		db::Transaction tx(*db_conn);
		const auto p = hosted::find_product_by_key(*db_conn, key);
		if (!p)
			return no_such_project();
		const int64_t number = revision.value_or(p->head);
		if (number <= 0 || !find_revision(*db_conn, p->id, number))
			return json_error(400, "no_revision", "the project has no such revision");
		int64_t base_number = 0;
		if (p->parent_id) {
			auto base = hosted::find_product(*db_conn, *p->parent_id);
			if (base && base->key.empty())
				base.reset(); // a base game with no project
			base_number = base ? base_revision.value_or(base->head) : 0;
			if (!base || base_number <= 0 || !find_revision(*db_conn, base->id, base_number))
				return json_error(
						400, "no_revision", "the base game's project has no such revision");
		}
		hosted::NewBuiltRelease made;
		made.name = p->key + " " + *version;
		made.created_by = caller.user ? "admin " + caller.user->username : std::string("token");
		made.product_id = p->id;
		made.content_revision = number;
		made.base_revision = base_number;
		made.version = *version;
		const int64_t id = hosted::create_built_release(*db_conn, made);
		hosted::enqueue_release_build(*db_conn, id);
		tx.commit();
		const auto row = hosted::find_release(*db_conn, id);
		if (!row)
			return json_error(500, "db_error");
		crow::json::wvalue out;
		out["release"] = hosted::release_json(*db_conn, *row);
		return no_store(json_reply(201, out));
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/content/projects/<key>/releases", e);
	}
}

} // namespace opennova::novaworld_server::content
