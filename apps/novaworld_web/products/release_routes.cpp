#include "release_routes.h"

#include "core/loggable.h"
#include "core/object_store.h"
#include "core/text_util.h"
#include "http_json.h"
#include "jobs/job_queue.h"
#include "products/product_jobs.h"
#include "products/product_repository.h"
#include "products/release_build.h"

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

crow::response no_such_release() {
	return json_error(404, "not_found", "no release has this id");
}

crow::response built_from_project() {
	return json_error(409, "built_from_project",
			"this game's releases are built from its project, never uploaded");
}

// Whether the game `product_id` names has a project, whose releases are built (release_build.h).
bool has_project(db::Database &db, std::optional<int64_t> product_id) {
	const std::optional<ProductRow> game = product_id ? find_product(db, *product_id) : std::nullopt;
	return game && !game->key.empty();
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
	std::optional<ProductRow> game;
	if (r.product_id) {
		out["product_id"] = *r.product_id;
		game = find_product(db, *r.product_id);
	} else {
		out["product_id"] = nullptr;
	}
	std::vector<crow::json::wvalue> sections;
	for (const ReleaseSection &s : list_sections(db, r.id)) {
		crow::json::wvalue section;
		section["product_id"] = s.product_id;
		const auto product = find_product(db, s.product_id);
		section["display_name"] = product ? product->display_name : std::string();
		section["expansion"] = s.expansion;
		section["command_file"] = s.command_file;
		section["version"] = s.version;
		std::optional<ProductTree> tree;
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
	// A built release's source (ADR 0069 d7): its game's project key, the revisions built, the
	// version, and what the build's tools said; null for an uploaded package.
	if (r.content_revision > 0 && game) {
		out["source"]["project"] = game->key;
		out["source"]["revision"] = r.content_revision;
		if (r.base_revision > 0)
			out["source"]["base_revision"] = r.base_revision;
		else
			out["source"]["base_revision"] = nullptr;
	} else {
		out["source"] = nullptr;
	}
	out["version"] = r.version;
	out["build_log"] = r.build_log;
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

// POST /api/admin/releases — {"name"?, "size", "product_id"?}: an uploaded release of a game
// with no project (the runtime's, from the Publish workflow; one that names no game becomes its
// package's base section's once validated) and its package's multipart upload (key
// updates/upload-<id>.sfs), 201 {"release", "upload": {"key", "upload_id", "part_size"}}; 400
// invalid_json, missing_field, invalid_field (product_id: no game has the id); 409
// built_from_project (the game named has a project: its releases are built, and release.validate
// refuses an upload of one whatever it names); 503 store_unavailable (no store, or one that took
// no upload).
crow::response ReleaseRoutes::create(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto name = body->text("name");
	const auto size = body->integer("size");
	const auto product_id = body->null("product_id") ? std::nullopt : body->integer("product_id");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!size)
		return field_error(400, "missing_field", "size", "size (the package's bytes) is required");
	if (*size <= 0 || *size > kPartSize * kMaxParts)
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
		if (product_id) {
			if (!find_product(*db_conn, *product_id))
				return field_error(400, "invalid_field", "product_id", "no game has this id");
			if (has_project(*db_conn, product_id))
				return built_from_project();
		}
		const int64_t id =
				create_release(*db_conn, name.value_or(""), *size, creator_of(caller), product_id);
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
		out["upload"]["part_size"] = kPartSize;
		return json_reply(201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/releases", e);
	}
}

// PUT /api/admin/releases/<id>/parts/<n>?upload_id=<u> — the body (application/octet-stream,
// at most kPartSize) handed to the store as part n (1 to 10000) of the package's upload: 200
// {"etag"} (what the completion reports for it); 400 invalid_field (part_number, upload_id); 404
// not_found; 409 not_uploading, built_from_project (its game has a project); 413 too_large; 502
// store_error; 503 store_unavailable. Crow caps no request body, so this check is the one after
// nginx's.
crow::response ReleaseRoutes::part(const crow::request &req, uint64_t id, uint64_t part) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (part < 1 || part > static_cast<uint64_t>(kMaxParts))
		return field_error(400, "invalid_field", "part_number", "the part number is 1 to 10000");
	const char *upload_id = req.url_params.get("upload_id");
	if (upload_id == nullptr || !valid_upload_id(upload_id))
		return field_error(
				400, "invalid_field", "upload_id", "upload_id is the one the upload began with");
	if (req.body.size() > static_cast<size_t>(kPartSize))
		return json_error(413, "too_large", "a part is at most 32 MiB");
	try {
		auto db_conn = pool_.acquire();
		const auto release = find_release(*db_conn, path_id(id));
		if (!release)
			return no_such_release();
		if (release->status != ReleaseStatus::Uploading)
			return json_error(409, "not_uploading", "this release is not uploading");
		if (has_project(*db_conn, release->product_id))
			return built_from_project();
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
// 404 not_found; 409 not_uploading, built_from_project; 502 store_error; 503 store_unavailable.
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
				number > kMaxParts || part.etag.empty() || part.etag.size() > 256 ||
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
		if (has_project(*db_conn, release->product_id))
			return built_from_project();
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

// POST /api/admin/releases/<id>/publish — a draft published: its sections' products serve it
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
				if (enqueue_release_apply(*db_conn, release->id, s.product_id) != 0)
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

// POST /api/admin/releases/<id>/retire — a published release retired (its products serve
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
// validating or building (the data program is on it; a building one whose job ended may go); 502
// store_error; 503 store_unavailable.
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
		// A build whose job ended without settling its row (its attempts spent on a data task
		// replaced mid-build) leaves it building with no one on it: that one may go.
		if (release->status == ReleaseStatus::Building &&
				jobs::job_pending(*db_conn, release_build_key(release->id)))
			return json_error(
					409, "building", "the data program is building this release; wait for it");
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
