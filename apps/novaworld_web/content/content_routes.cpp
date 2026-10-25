#include "content_routes.h"

#include "content_urls.h"
#include "core/loggable.h"
#include "core/object_store.h"
#include "data_sets/content_repository.h"
#include "http_json.h"

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

crow::response db_failure(const char *route, const db::Error &e) {
	std::fprintf(stderr, "[http] %s failed: %s\n", route, e.what());
	return json_error(500, "db_error");
}

crow::response store_unavailable() {
	return json_error(503, "store_unavailable", "this NovaWorld service has no object store");
}

crow::response store_failure(const char *route, const std::string &error) {
	std::fprintf(stderr, "[http] %s: the object store failed: %s\n", route, error.c_str());
	return json_error(502, "store_error", "the object store did not take the request");
}

crow::response no_such_project() {
	return json_error(404, "not_found", "no content project has this key");
}

crow::response field_error(
		int code, const char *error, const char *field, const std::string &message) {
	crow::json::wvalue body;
	body["error"] = error;
	body["field"] = field;
	body["message"] = message;
	return json_reply(code, body);
}

// A push and an upload are an admin's own: the Bearer token, a machine's, names no author.
crow::response session_required() {
	return json_error(403, "session_required", "a push is an admin's own: sign in to push");
}

// A project's key: 1 to 16 of A-Z, 0-9 and _.
bool valid_key(std::string_view key) {
	if (key.empty() || key.size() > 16)
		return false;
	for (const char c : key)
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	return true;
}

bool valid_title(std::string_view title) {
	return !title.empty() && strutil::utf8_length(title) <= ContentRoutes::kTitleMax &&
			opennova::content::is_utf8(title) && !has_control_byte(title) && !has_c1_control(title);
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
std::string immutable_cache(const ContentProject &project) {
	return project.visibility == Visibility::Public ? "public, max-age=31536000, immutable"
													: "private, max-age=31536000, immutable";
}

// The project `key` names as a caller (`admin` or not) may read it: none for a private one asked
// for by anyone but an admin.
std::optional<ContentProject> readable(db::Database &db, const std::string &key, bool admin) {
	auto project = find_project(db, key);
	if (!project || (project->visibility == Visibility::Private && !admin))
		return std::nullopt;
	return project;
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

// A project as the routes answer it: {key, title, visibility, retail, base (its base project's
// key, or null), data_set_id (or null), head, can_push}, and for an admin its storage ({files,
// bytes}). `keys` maps every project's id to its key.
crow::json::wvalue project_json(db::Database &db, const ContentProject &p, bool admin,
		const std::unordered_map<int64_t, std::string> &keys) {
	crow::json::wvalue out;
	out["key"] = p.key;
	out["title"] = p.title;
	out["visibility"] = visibility_name(p.visibility);
	out["retail"] = p.retail;
	const auto base = p.base_project_id ? keys.find(*p.base_project_id) : keys.end();
	if (base != keys.end())
		out["base"] = base->second;
	else
		out["base"] = nullptr;
	if (p.data_set_id)
		out["data_set_id"] = *p.data_set_id;
	else
		out["data_set_id"] = nullptr;
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
	for (const ContentProject &p : list_projects(db, true))
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
	CROW_ROUTE(app, "/api/admin/content/projects")
			.methods("POST"_method)([this](const crow::request &req) { return create(req); });
	CROW_ROUTE(app, "/api/admin/content/projects/<string>")
			.methods("PATCH"_method)([this](const crow::request &req, const std::string &key) {
				return patch(req, key);
			});
	CROW_ROUTE(app, "/api/admin/content/projects/<string>")
			.methods("DELETE"_method)([this](const crow::request &req, const std::string &key) {
				return remove(req, key);
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
		for (const ContentProject &p : list_projects(*db_conn, admin))
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
	std::optional<ContentProject> p;
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
					*store_, manifest_key(p->key, revision->manifest_sha256), scratch, text, error))
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
	std::optional<ContentProject> p;
	try {
		p = readable(*pool_.acquire(), key, admin);
	} catch (const db::Error &e) {
		return db_failure("GET /api/content/projects/<key>/blobs/<sha256>", e);
	}
	if (!p)
		return no_such_project();
	const std::string object = blob_key(p->key, sha);
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
	try {
		auto db_conn = pool_.acquire();
		const auto p = find_project(*db_conn, key);
		if (!p)
			return no_such_project();
		absent = missing_blobs(*db_conn, p->id, wanted);
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
			if (!urls_->put(blob_key(key, blob.sha256), blob.sha256, upload, error))
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
	try {
		if (!readable(*pool_.acquire(), key, admin))
			return no_such_project();
	} catch (const db::Error &e) {
		return db_failure("POST /api/content/projects/<key>/downloads", e);
	}
	std::vector<crow::json::wvalue> list;
	for (const std::string &sha : hashes) {
		SignedTransfer download;
		std::string error;
		if (urls_ != nullptr) {
			if (!urls_->get(blob_key(key, sha), download, error))
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
		const auto p = find_project(*db_conn, key);
		if (!p)
			return no_such_project();
		std::string error;
		const std::string scratch = scratch_file("upload", error);
		if (scratch.empty() ||
				!write_object(*store_, blob_key(p->key, sha), scratch, req.body, error))
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
		const auto p = find_project(*db_conn, key);
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
		// upload through the store's own URL) is held once the store has it at its size. A few are
		// asked of the store one by one; more by one listing of the project's files, since each
		// store call is a process of the AWS CLI's (a first push of a retail game names thousands).
		std::vector<StoredBlob> listed;
		listed.reserve(files.size());
		for (const ContentFile &file : files)
			listed.push_back({ file.sha256, static_cast<int64_t>(file.size) });
		const std::vector<StoredBlob> unrecorded = missing_blobs(*db_conn, p->id, listed);
		std::unordered_map<std::string, uint64_t> stored; // key -> size, when listed
		if (unrecorded.size() > kHeadsAtMost) {
			std::vector<StoredObject> objects;
			std::string error;
			if (!store_->list(project_prefix(p->key) + "blobs/", objects, error))
				return store_failure(kRoute, error);
			for (const StoredObject &object : objects)
				stored.emplace(object.key, object.size);
		}
		std::vector<StoredBlob> found;
		std::vector<std::string> absent;
		for (const StoredBlob &blob : unrecorded) {
			const std::string object = blob_key(p->key, blob.sha256);
			bool exists = false;
			uint64_t size = 0;
			if (unrecorded.size() > kHeadsAtMost) {
				const auto at = stored.find(object);
				exists = at != stored.end();
				size = exists ? at->second : 0;
			} else {
				std::string error;
				if (!store_->head(object, exists, size, error))
					return store_failure(kRoute, error);
			}
			if (exists && size == static_cast<uint64_t>(blob.bytes))
				found.push_back(blob);
			else
				absent.push_back(blob.sha256);
		}
		if (!absent.empty()) {
			crow::json::wvalue refusal;
			refusal["error"] = "missing_blobs";
			refusal["message"] = "the store does not hold every file the manifest lists";
			std::vector<crow::json::wvalue> hashes;
			for (const std::string &sha : absent)
				hashes.emplace_back(sha);
			refusal["hashes"] = std::move(hashes);
			return json_reply(400, refusal);
		}
		record_blobs(*db_conn, p->id, found);
		std::string error;
		const std::string scratch = scratch_file("manifest", error);
		if (scratch.empty() ||
				!write_object(*store_, manifest_key(p->key, manifest_sha), scratch, *manifest_text,
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

// POST /api/admin/content/projects — {"key", "title", "visibility", "retail"?, "data_set_id"?,
// "base"? (its base game's project's key)}: 201 {"project"}; 400 invalid_json, missing_field,
// invalid_field, no_such_data_set, no_such_base; 409 duplicate_key, data_set_taken.
crow::response ContentRoutes::create(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto key = body->text("key");
	const auto title = body->text("title");
	const auto visibility = body->text("visibility");
	const auto retail = body->flag("retail");
	const auto data_set_id =
			body->null("data_set_id") ? std::nullopt : body->integer("data_set_id");
	const auto base = body->null("base") ? std::nullopt : body->text("base");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!key || !title || !visibility)
		return json_error(400, "missing_field", "a project is {key, title, visibility}");
	if (!valid_key(*key))
		return field_error(400, "invalid_field", "key", "key is 1 to 16 of A-Z, 0-9 and _");
	if (!valid_title(*title))
		return field_error(400, "invalid_field", "title",
				"title is 1 to 64 characters with no control characters");
	const auto seen = visibility_from_name(*visibility);
	if (!seen)
		return field_error(400, "invalid_field", "visibility", "visibility is public or private");
	NewProject project;
	project.key = *key;
	project.title = *title;
	project.visibility = *seen;
	project.retail = retail.value_or(false);
	project.data_set_id = data_set_id;
	project.created_by = caller.user ? "admin " + caller.user->username : std::string("token");
	try {
		auto db_conn = pool_.acquire();
		if (base) {
			const auto base_project = find_project(*db_conn, *base);
			if (!base_project)
				return json_error(400, "no_such_base", "no content project has the base's key");
			project.base_project_id = base_project->id;
		}
		int64_t id = 0;
		switch (create_project(*db_conn, project, id)) {
			case ProjectOutcome::Ok:
				break;
			case ProjectOutcome::DuplicateKey:
				return json_error(409, "duplicate_key", "another content project has this key");
			case ProjectOutcome::DataSetTaken:
				return json_error(
						409, "data_set_taken", "another content project updates this data set");
			case ProjectOutcome::NoSuchDataSet:
				return json_error(400, "no_such_data_set", "no data set has this id");
			case ProjectOutcome::NoSuchBase:
				return json_error(400, "no_such_base", "no content project has the base's key");
			case ProjectOutcome::NotFound:
			case ProjectOutcome::InUse:
				return json_error(500, "db_error");
		}
		const auto made = find_project_by_id(*db_conn, id);
		if (!made)
			return json_error(500, "db_error");
		crow::json::wvalue out;
		out["project"] = project_json(*db_conn, *made, true, project_keys(*db_conn));
		return no_store(json_reply(201, out));
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/content/projects", e);
	}
}

// PATCH /api/admin/content/projects/<key> — {"title"?, "visibility"?}: 200 {"project"}; 400
// invalid_json, invalid_field; 404 not_found.
crow::response ContentRoutes::patch(const crow::request &req, const std::string &key) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto title = body->text("title");
	const auto visibility = body->text("visibility");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	ProjectPatch changes;
	if (title) {
		if (!valid_title(*title))
			return field_error(400, "invalid_field", "title",
					"title is 1 to 64 characters with no control characters");
		changes.title = *title;
	}
	if (visibility) {
		const auto seen = visibility_from_name(*visibility);
		if (!seen)
			return field_error(
					400, "invalid_field", "visibility", "visibility is public or private");
		changes.visibility = *seen;
	}
	try {
		auto db_conn = pool_.acquire();
		const auto p = find_project(*db_conn, key);
		if (!p || update_project(*db_conn, p->id, changes) != ProjectOutcome::Ok)
			return no_such_project();
		const auto changed = find_project_by_id(*db_conn, p->id);
		if (!changed)
			return no_such_project();
		crow::json::wvalue out;
		out["project"] = project_json(*db_conn, *changed, true, project_keys(*db_conn));
		return no_store(json_reply(200, out));
	} catch (const db::Error &e) {
		return db_failure("PATCH /api/admin/content/projects/<key>", e);
	}
}

// DELETE /api/admin/content/projects/<key> — 204, the project's rows gone and then its objects
// (content/<key>/); 404 not_found; 409 has_expansion (an expansion's project builds on it).
crow::response ContentRoutes::remove(const crow::request &req, const std::string &key) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const auto p = find_project(*db_conn, key);
		if (!p)
			return no_such_project();
		switch (delete_project(*db_conn, p->id)) {
			case ProjectOutcome::Ok:
				break;
			case ProjectOutcome::InUse:
				return json_error(
						409, "has_expansion", "an expansion's content project builds on this one");
			default:
				return no_such_project();
		}
	} catch (const db::Error &e) {
		return db_failure("DELETE /api/admin/content/projects/<key>", e);
	}
	std::string error;
	if (store_ != nullptr && !store_->remove_prefix(project_prefix(key), error))
		std::fprintf(stderr,
				"[http] DELETE /api/admin/content/projects/%s: its objects stay in the store: %s\n",
				loggable(key).c_str(), error.c_str());
	return crow::response(204);
}

} // namespace opennova::novaworld_server::content
