#include "release_routes.h"

#include "core/loggable.h"
#include "core/object_store.h"
#include "data_sets/data_set_jobs.h"
#include "data_sets/data_set_repository.h"
#include "data_sets/data_set_routes.h"
#include "http_json.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

std::string creator_of(const RouteAccess &caller) {
	return caller.user ? "admin " + caller.user->username : std::string("token");
}

crow::response db_failure(const char *route, const db::Error &e) {
	std::fprintf(stderr, "[http] %s failed: %s\n", route, e.what());
	return json_error(500, "db_error");
}

crow::response field_error(
		int code, const char *error, const char *field, const std::string &message) {
	crow::json::wvalue body;
	body["error"] = error;
	body["field"] = field;
	body["message"] = message;
	return json_reply(code, body);
}

crow::response store_unavailable() {
	return json_error(503, "store_unavailable", "this NovaWorld service has no object store");
}

crow::response store_failure(const char *route, const std::string &error) {
	std::fprintf(stderr, "[http] %s: the object store failed: %s\n", route, error.c_str());
	return json_error(502, "store_error", "the object store did not take the request");
}

crow::response no_such_release() {
	return json_error(404, "not_found", "no release has this id");
}

bool printable_ascii(std::string_view s) {
	return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x21 && c <= 0x7E; });
}

bool valid_upload_id(std::string_view id) {
	return !id.empty() && id.size() <= 1024 && printable_ascii(id);
}

bool member_string(const crow::json::rvalue &object, const char *key, std::string &out) {
	if (!object.has(key) || object[key].t() != crow::json::type::String)
		return false;
	out = std::string(object[key].s());
	return true;
}

bool member_whole(const crow::json::rvalue &object, const char *key, int64_t &out) {
	if (!object.has(key) || object[key].t() != crow::json::type::Number ||
			object[key].nt() == crow::json::num_type::Floating_point)
		return false;
	out = object[key].i();
	return true;
}

} // namespace

crow::json::wvalue release_json(db::Database &db, const ReleaseRow &r) {
	crow::json::wvalue out;
	out["id"] = r.id;
	out["status"] = release_status_name(r.status);
	out["name"] = r.name;
	out["sha256"] = r.sha256;
	out["bytes"] = r.bytes;
	out["package_product"] = r.package_product;
	out["error"] = r.error;
	std::vector<crow::json::wvalue> sections;
	for (const ReleaseSection &s : list_sections(db, r.id)) {
		crow::json::wvalue section;
		section["data_set_id"] = s.data_set_id;
		const auto data_set = find_data_set(db, s.data_set_id);
		section["display_name"] = data_set ? data_set->display_name : std::string();
		section["expansion"] = s.expansion;
		section["command_file"] = s.command_file;
		section["version"] = s.version;
		std::optional<DataSetTree> tree;
		if (s.tree_id)
			tree = find_tree(db, *s.tree_id);
		if (tree) {
			section["tree"]["id"] = tree->id;
			section["tree"]["state"] = tree_state_name(tree->state);
		} else {
			section["tree"] = nullptr;
		}
		sections.push_back(std::move(section));
	}
	out["sections"] = std::move(sections);
	out["created_by"] = r.created_by;
	out["created_at"] = r.created_at;
	out["published_at"] = r.published_at; // "" until published, and again once retired
	out["retired_at"] = r.retired_at; // "" until retired
	return out;
}

void ReleaseRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/releases").methods("GET"_method)([this](const crow::request &req) {
		return list(req);
	});
	CROW_ROUTE(app, "/api/admin/releases").methods("POST"_method)([this](const crow::request &req) {
		return create(req);
	});
	CROW_ROUTE(app, "/api/admin/releases/<uint>/parts/<uint>")
			.methods("PUT"_method)([this](const crow::request &req, uint64_t id, uint64_t number) {
				return part(req, id, number);
			});
	CROW_ROUTE(app, "/api/admin/releases/<uint>/complete")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return complete(req, id); });
	CROW_ROUTE(app, "/api/admin/releases/<uint>/publish")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return publish(req, id); });
	CROW_ROUTE(app, "/api/admin/releases/<uint>/retire")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return retire(req, id); });
	CROW_ROUTE(app, "/api/admin/releases/<uint>")
			.methods("DELETE"_method)(
					[this](const crow::request &req, uint64_t id) { return remove(req, id); });
}

// GET /api/admin/releases — {"releases"}, newest first, each as release_json.
crow::response ReleaseRoutes::list(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		std::vector<crow::json::wvalue> releases;
		for (const ReleaseRow &r : list_releases(*db_conn))
			releases.push_back(release_json(*db_conn, r));
		out["releases"] = std::move(releases);
	} catch (const db::Error &e) {
		return db_failure("GET /api/admin/releases", e);
	}
	return no_store(json_reply(200, out));
}

// POST /api/admin/releases — {"name"?, "size"}: a release uploading and its package's
// multipart upload (key updates/upload-<id>.sfs), 201 {"release", "upload": {"key",
// "upload_id", "part_size"}}; 400 invalid_json, missing_field, invalid_field; 503
// store_unavailable (no store, or one that took no upload).
crow::response ReleaseRoutes::create(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto name = body->text("name");
	const auto size = body->integer("size");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!size)
		return field_error(400, "missing_field", "size", "size (the package's bytes) is required");
	if (*size <= 0 || *size > DataSetRoutes::kPartSize * DataSetRoutes::kMaxParts)
		return field_error(400, "invalid_field", "size",
				"size is the package's bytes, at most 32 MiB x 10000");
	if (name &&
			(strutil::utf8_length(*name) > kNameMax || has_control_byte(*name) ||
					has_c1_control(*name)))
		return field_error(400, "invalid_field", "name",
				"name is at most 64 characters with no control characters");
	if (store_ == nullptr)
		return store_unavailable();
	try {
		auto db_conn = pool_.acquire();
		const int64_t id = create_release(*db_conn, name.value_or(""), *size, creator_of(caller));
		const std::string key = release_upload_key(id);
		std::string upload_id, error;
		if (!store_->multipart_begin(key, upload_id, error)) {
			std::fprintf(stderr,
					"[http] POST /api/admin/releases: the object store began no upload of %s: %s\n",
					key.c_str(), error.c_str());
			delete_release(*db_conn, id);
			return json_error(503, "store_unavailable", "the object store took no upload");
		}
		std::printf("[http] release %lld '%s' (%lld bytes) uploading, begun by %s\n",
				static_cast<long long>(id), loggable(name.value_or("")).c_str(),
				static_cast<long long>(*size), loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["release"] = release_json(*db_conn, *find_release(*db_conn, id));
		out["upload"]["key"] = key;
		out["upload"]["upload_id"] = upload_id;
		out["upload"]["part_size"] = DataSetRoutes::kPartSize;
		return json_reply(201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/releases", e);
	}
}

// PUT /api/admin/releases/<id>/parts/<n>?upload_id=<u> — the body (application/octet-stream,
// at most DataSetRoutes::kPartSize) handed to the store as part n (1 to 10000) of the
// package's upload, as the trees' part route does: 200 {"etag"} (what the completion reports
// for it); 400 invalid_field (part_number, upload_id); 404 not_found; 409 not_uploading; 413
// too_large; 502 store_error; 503 store_unavailable. Crow caps no request body, so this check
// is the one after nginx's.
crow::response ReleaseRoutes::part(const crow::request &req, uint64_t id, uint64_t part) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (part < 1 || part > static_cast<uint64_t>(DataSetRoutes::kMaxParts))
		return field_error(400, "invalid_field", "part_number", "the part number is 1 to 10000");
	const char *upload_id = req.url_params.get("upload_id");
	if (upload_id == nullptr || !valid_upload_id(upload_id))
		return field_error(
				400, "invalid_field", "upload_id", "upload_id is the one the upload began with");
	if (req.body.size() > static_cast<size_t>(DataSetRoutes::kPartSize))
		return json_error(413, "too_large", "a part is at most 32 MiB");
	try {
		auto db_conn = pool_.acquire();
		const auto release = find_release(*db_conn, path_id(id));
		if (!release)
			return no_such_release();
		if (release->status != ReleaseStatus::Uploading)
			return json_error(409, "not_uploading", "this release is not uploading");
		if (store_ == nullptr)
			return store_unavailable();
		std::string etag, error;
		if (!store_->upload_part(release_upload_key(release->id), upload_id,
					static_cast<int>(part), req.body, etag, error))
			return store_failure("PUT /api/admin/releases/<id>/parts/<n>", error);
		crow::json::wvalue out;
		out["etag"] = etag;
		return no_store(json_reply(200, out));
	} catch (const db::Error &e) {
		return db_failure("PUT /api/admin/releases/<id>/parts/<n>", e);
	}
}

// POST /api/admin/releases/<id>/complete — {"upload_id", "parts": [{"part_number", "etag"}]}:
// the package's upload completed in the store, then the release validating with its
// release.validate job in one transaction, 202 {"release"}; 400 invalid_json, invalid_field;
// 404 not_found; 409 not_uploading; 502 store_error; 503 store_unavailable.
crow::response ReleaseRoutes::complete(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const crow::json::rvalue body = crow::json::load(req.body);
	if (!body || body.t() != crow::json::type::Object)
		return json_error(400, "invalid_json");
	std::string upload_id;
	if (!member_string(body, "upload_id", upload_id) || !valid_upload_id(upload_id))
		return field_error(
				400, "invalid_field", "upload_id", "upload_id is the one the upload began with");
	if (!body.has("parts") || body["parts"].t() != crow::json::type::List)
		return field_error(400, "invalid_field", "parts", "parts is a list of {part_number, etag}");
	std::vector<UploadPart> parts;
	for (const crow::json::rvalue &item : body["parts"]) {
		UploadPart part;
		int64_t number = 0;
		if (item.t() != crow::json::type::Object || !member_whole(item, "part_number", number) ||
				!member_string(item, "etag", part.etag) || number < 1 ||
				number > DataSetRoutes::kMaxParts || part.etag.empty() || part.etag.size() > 256 ||
				!printable_ascii(part.etag))
			return field_error(
					400, "invalid_field", "parts", "parts is a list of {part_number, etag}");
		part.number = static_cast<int>(number);
		parts.push_back(std::move(part));
	}
	if (parts.empty())
		return field_error(400, "invalid_field", "parts", "parts names at least one part");
	try {
		auto db_conn = pool_.acquire();
		const auto release = find_release(*db_conn, path_id(id));
		if (!release)
			return no_such_release();
		if (release->status != ReleaseStatus::Uploading)
			return json_error(409, "not_uploading", "this release is not uploading");
		if (store_ == nullptr)
			return store_unavailable();
		std::string error;
		if (!store_->multipart_complete(release_upload_key(release->id), upload_id, parts, error))
			return store_failure("POST /api/admin/releases/<id>/complete", error);
		{
			db::Transaction tx(*db_conn);
			if (!set_release_status(
						*db_conn, release->id, ReleaseStatus::Uploading, ReleaseStatus::Validating))
				return json_error(409, "not_uploading", "this release is not uploading");
			enqueue_release_validate(*db_conn, release->id);
			tx.commit();
		}
		std::printf("[http] release %lld uploaded (%zu part(s)), validating; completed by %s\n",
				static_cast<long long>(release->id), parts.size(),
				loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["release"] = release_json(*db_conn, *find_release(*db_conn, release->id));
		return json_reply(202, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/releases/<id>/complete", e);
	}
}

// POST /api/admin/releases/<id>/publish — a draft published: its sections' data sets serve it
// to the updater (the release they named before is retired), and one release.apply job per
// section, in one transaction; 202 {"release"}; 404 not_found; 409 not_draft.
crow::response ReleaseRoutes::publish(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const auto release = find_release(*db_conn, path_id(id));
		if (!release)
			return no_such_release();
		size_t applies = 0;
		{
			db::Transaction tx(*db_conn);
			if (!publish_release(*db_conn, release->id))
				return json_error(
						409, "not_draft", "only a draft (a validated release) is published");
			for (const ReleaseSection &s : list_sections(*db_conn, release->id))
				if (enqueue_release_apply(*db_conn, release->id, s.data_set_id) != 0)
					++applies;
			tx.commit();
		}
		std::printf("[http] release %lld '%s' published by %s: %zu apply job(s)\n",
				static_cast<long long>(release->id), loggable(release->name).c_str(),
				loggable(creator_of(caller)).c_str(), applies);
		crow::json::wvalue out;
		out["release"] = release_json(*db_conn, *find_release(*db_conn, release->id));
		return json_reply(202, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/releases/<id>/publish", e);
	}
}

// POST /api/admin/releases/<id>/retire — a published release retired (its data sets serve
// none), 202 {"release"}; 404 not_found; 409 not_published.
crow::response ReleaseRoutes::retire(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const auto release = find_release(*db_conn, path_id(id));
		if (!release)
			return no_such_release();
		if (!retire_release(*db_conn, release->id))
			return json_error(409, "not_published", "only a published release is retired");
		std::printf("[http] release %lld '%s' retired by %s\n", static_cast<long long>(release->id),
				loggable(release->name).c_str(), loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["release"] = release_json(*db_conn, *find_release(*db_conn, release->id));
		return json_reply(202, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/releases/<id>/retire", e);
	}
}

// DELETE /api/admin/releases/<id> — a draft, invalid, retired or uploading (abandoned)
// release: its package objects removed (the upload key, which an invalid one keeps, with
// the upload in flight aborted, and the validated key once it has a sha; a missing object
// is no failure), the row gone, 204; 404 not_found; 409 published (retire it first),
// validating (the data program is on it); 502 store_error; 503 store_unavailable.
crow::response ReleaseRoutes::remove(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const auto release = find_release(*db_conn, path_id(id));
		if (!release)
			return no_such_release();
		if (release->status == ReleaseStatus::Published)
			return json_error(409, "published", "retire this release before deleting it");
		if (release->status == ReleaseStatus::Validating)
			return json_error(
					409, "validating", "the data program is validating this release; wait for it");
		if (store_ == nullptr)
			return store_unavailable();
		std::string error;
		if (!store_->abort_uploads_under(release_upload_key(release->id), error) ||
				!store_->remove(release_upload_key(release->id), error) ||
				(!release->sha256.empty() &&
						!store_->remove(release_package_key(release->sha256), error)))
			return store_failure("DELETE /api/admin/releases/<id>", error);
		if (!delete_release(*db_conn, release->id))
			return no_such_release();
		std::printf("[http] release %lld '%s' (%s) deleted by %s\n",
				static_cast<long long>(release->id), loggable(release->name).c_str(),
				release_status_name(release->status), loggable(creator_of(caller)).c_str());
		return crow::response(204);
	} catch (const db::Error &e) {
		return db_failure("DELETE /api/admin/releases/<id>", e);
	}
}

} // namespace opennova::novaworld_server::hosted
