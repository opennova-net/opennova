#include "project_routes.h"

#include "core/loggable.h"
#include "core/object_store.h"
#include "http_json.h"
#include "products/product_jobs.h"
#include "products/product_repository.h"
#include "products/product_routes.h"
#include "products/project_repository.h"
#include "products/release_build.h"
#include "products/release_routes.h"
#include "products/tree_files.h"
#include "program/catalog_repository.h"
#include "project_urls.h"

#include <base/io/file_io.h>
#include <base/io/base64.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/project/project_manifest.h>
#include <base/version/version.h>
#include <net/protocol/protocol.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::projects {

namespace {

using opennova::project::ProjectFile;
using hosted::ProductRow;

// A project file naming a key renamed since (or a project deleted), or a private project the
// caller may not read, reads this.
crow::response no_such_project() {
	return json_error(404, "not_found",
			"no project you may read has this key (a renamed key, a deleted project, or a private "
			"one: sign in with an account that may read it)");
}

// A push and an upload are an account's own: the Bearer token, a machine's, names no author.
crow::response session_required() {
	return json_error(403, "session_required", "a push is an account's own: sign in to push");
}

// A project the caller may read but not push to, or not change.
crow::response push_forbidden() {
	return json_error(403, "forbidden", "only its owner and NovaWorld's admins push this project");
}
crow::response manage_forbidden() {
	return json_error(
			403, "forbidden", "only its owner and NovaWorld's admins change this project");
}
crow::response release_forbidden() {
	return json_error(
			403, "forbidden", "only its owner and NovaWorld's admins release this project");
}

crow::response no_such_release() {
	return json_error(404, "not_found", "the project has no release of this id");
}

// An upload that would take an account's own projects past their stores' quota: 413
// {"error": "quota_exceeded", "used", "limit"}, the bytes they hold and the most they may.
crow::response quota_exceeded(int64_t used) {
	crow::json::wvalue out;
	out["error"] = "quota_exceeded";
	out["message"] = "an account's own projects hold at most 10 GiB together";
	out["used"] = used;
	out["limit"] = hosted::kOwnedBytesMax;
	return json_reply(413, out);
}

// The project rule's caller (project_repository.h project_rights) a read names, and a session's.
ProjectCaller project_caller(const RequestCaller &caller) {
	ProjectCaller out;
	if (caller.user)
		out.user_id = caller.user->id;
	out.admin = caller.admin();
	return out;
}
ProjectCaller project_caller(const WebSessionUser &user) {
	ProjectCaller out;
	out.user_id = user.id;
	out.admin = user.role == "admin";
	return out;
}

// Whether the account's own projects, `owner_id`'s, have room for `bytes` more (an admin's own
// always do: hosted::owned_limits_apply).
bool owner_has_room(db::Database &db, int64_t owner_id, int64_t bytes, int64_t &used) {
	if (!hosted::owned_limits_apply(db, owner_id))
		return true;
	used = owned_usage(db, owner_id).bytes;
	return bytes <= hosted::kOwnedBytesMax - used;
}

int64_t bytes_of(const std::vector<StoredBlob> &blobs) {
	int64_t bytes = 0;
	for (const StoredBlob &blob : blobs)
		bytes += blob.bytes;
	return bytes;
}

// A revision's message: UTF-8 with no control character but a line break or a tab.
bool valid_message(std::string_view message) {
	if (message.empty() || message.size() > ProjectRoutes::kMessageMax ||
			!opennova::project::is_utf8(message) || has_c1_control(message))
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

// The project `key` names: its game's or its expansion's row; none for an app's key, an app having
// no project.
std::optional<ProductRow> project_of(db::Database &db, const std::string &key) {
	auto project = hosted::find_product_by_key(db, key);
	if (!project || project->kind == hosted::ProductKind::App)
		return std::nullopt;
	return project;
}

// The project `key` names as `caller` may read it: none for an app's key, no product's, or one
// the caller may not read (project_rights), the same 404 as none.
std::optional<ProductRow> readable(
		db::Database &db, const std::string &key, const ProjectCaller &caller) {
	auto project = hosted::find_product_by_key(db, key);
	if (!project || !project_rights(*project, caller).read)
		return std::nullopt;
	return project;
}

// The files of `unrecorded` the project's store holds at their sizes (each an upload through the
// store's own URL, which no request records) into `found`, the rest into `absent`; an object the
// store holds at another size than the one named is removed (no manifest may name it, and no
// count holds it); false with `error` when the store fails. A few are asked of the store one by
// one; more by one listing of the project's files, since each store call is a process of the AWS
// CLI's (a first push of an imported game names thousands).
bool sort_by_store(ObjectStore &store, int64_t product_id,
		const std::vector<StoredBlob> &unrecorded, std::vector<StoredBlob> &found,
		std::vector<StoredBlob> &absent, std::string &error) {
	const bool listing = unrecorded.size() > ProjectRoutes::kHeadsAtMost;
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
		if (exists && size != static_cast<uint64_t>(blob.bytes) && !store.remove(object, error))
			return false;
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

crow::json::wvalue revision_json(const ProjectRevision &r) {
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

// A project as the routes answer it: {key, title (its name), kind ("game", "expansion"), game (the
// title code it runs as: a game's title, an expansion's base game's), protocol (the {code, name,
// version} that title speaks, http_json.h protocol_or_null; null for none), expansion_code (""
// for a game), visibility, lobby ({slug, display_name, protocol, gate_tag_file} of its
// game's lobby, or null), lobby_locked, base (an expansion's base game's key, or null),
// product_id (its game's), head, owner (the username of the account whose own project it is, null
// for the service's own), can_push, can_manage and can_release (what the caller may do,
// project_rights), release (its published release's {id, version, one_version (the ONE that
// built it, null for none)}, null for none), version_file (an expansion's version.txt as its
// release build writes it, base64, tree_files.h: the bytes a hosted server of the published
// release holds, which the editor's Play ships so a joiner's checksum matches; null for a game
// and for an expansion with nothing published and no line of its row's)}, and for one who may
// manage it its storage ({files, bytes}). `games` holds the game an expansion builds on, by its
// id, and `lobbies` the games table, loaded once for a request's every project. lobby_locked is
// whether the selection is fixed: a locked game with no lobby still takes one.
crow::json::wvalue project_json(db::Database &db, const ProductRow &p, const ProjectCaller &caller,
		const std::unordered_map<int64_t, ProductRow> &games, const catalog::GameCatalog &lobbies) {
	const ProjectRights rights = project_rights(p, caller);
	const auto base = p.parent_id ? games.find(*p.parent_id) : games.end();
	crow::json::wvalue out;
	out["key"] = p.key;
	out["title"] = p.display_name;
	out["kind"] = hosted::product_kind_name(p.kind);
	const std::string &title = p.kind == hosted::ProductKind::Game ? p.title
			: base != games.end()								   ? base->second.title
																   : p.title;
	out["game"] = title;
	protocol_or_null(out, "protocol", title.empty() ? nullptr : title_protocol(title));
	out["expansion_code"] = p.expansion_code;
	out["visibility"] = p.visibility;
	const ProductRow *game = p.kind == hosted::ProductKind::Game ? &p
			: base != games.end()								 ? &base->second
																 : nullptr;
	out["lobby"] = nullptr;
	out["lobby_locked"] = game && game->lobby_locked && !game->lobby.empty();
	if (game && !game->lobby.empty()) {
		if (const auto *lobby = lobbies.by_slug(game->lobby)) {
			std::string bytes, why;
			if (catalog::gate_tag_file(*lobby, bytes, why)) {
				out["lobby"]["slug"] = lobby->slug;
				out["lobby"]["display_name"] = lobby->display_name;
				protocol_or_null(out["lobby"], "protocol", protocol_by_code(lobby->protocol));
				out["lobby"]["gate_tag_file"] = io::base64_encode(
						reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
			}
		}
	}
	if (base != games.end())
		out["base"] = base->second.key;
	else
		out["base"] = nullptr;
	out["product_id"] = p.id;
	out["head"] = p.head;
	if (p.owner_id)
		out["owner"] = p.owner;
	else
		out["owner"] = nullptr;
	out["can_push"] = rights.push;
	out["can_manage"] = rights.manage;
	out["can_release"] = rights.release;
	const std::optional<hosted::ReleaseRow> published = p.published_release_id
			? hosted::find_release(db, *p.published_release_id)
			: std::nullopt;
	std::string version;
	if (published) {
		out["release"]["id"] = published->id;
		version = published->version;
		for (const hosted::ReleaseSection &s : hosted::list_sections(db, published->id))
			if (version.empty() && s.product_id == p.id)
				version = s.version;
		out["release"]["version"] = version;
		text_or_null(out["release"], "one_version", published->one_version);
	} else {
		out["release"] = nullptr;
	}
	const std::string version_file = p.version_file.empty() && version.empty()
			? std::string()
			: hosted::expansion_version_file(p, version);
	if (version_file.empty())
		out["version_file"] = nullptr;
	else
		out["version_file"] = io::base64_encode(
				reinterpret_cast<const uint8_t *>(version_file.data()), version_file.size());
	if (rights.manage) {
		const ProjectStorage storage = project_storage(db, p.id);
		out["storage"]["files"] = storage.blobs;
		out["storage"]["bytes"] = storage.bytes;
	}
	return out;
}

// Every game by its id: what a list's expansions are named by.
std::unordered_map<int64_t, ProductRow> games_by_id(db::Database &db) {
	std::unordered_map<int64_t, ProductRow> games;
	for (ProductRow &p : hosted::list_products(db))
		if (p.kind == hosted::ProductKind::Game)
			games.emplace(p.id, std::move(p));
	return games;
}

// The game `p` builds on, by its id (none for a game): what one project's reply names.
std::unordered_map<int64_t, ProductRow> base_of(db::Database &db, const ProductRow &p) {
	std::unordered_map<int64_t, ProductRow> games;
	if (p.parent_id)
		if (auto base = hosted::find_product(db, *p.parent_id))
			games.emplace(base->id, std::move(*base));
	return games;
}

// Whether a new expansion of `caller`'s own may build on `game` (ADR 0074): a game of the
// service's own, public or private (a mod of the installed Joint Operations builds on JO), or one
// of the caller's own; never another account's, so no account's mod keeps another's game from its
// delete.
bool may_build_on(const ProductRow &game, const ProjectCaller &caller) {
	return game.kind == hosted::ProductKind::Game &&
			(!game.owner_id || (caller.user_id && game.owner_id == caller.user_id));
}

// The seconds to the next UTC midnight, when an account's day of releases starts over.
int64_t seconds_to_utc_midnight() {
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	return 86400 - now % 86400;
}

// An optional text member that may also come as null (absent).
std::optional<std::string> optional_text(JsonBody &body, const char *member) {
	return body.null(member) ? std::nullopt : body.text(member);
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

RouteAccess ProjectRoutes::pusher(const crow::request &req) {
	RouteAccess caller = access_.require_user(req);
	if (caller.refusal && caller.refusal->code == 401 && access_.identify(req).token)
		caller.refusal = session_required();
	return caller;
}

std::string ProjectRoutes::scratch_file(const char *what, std::string &error) {
	if (scratch_.empty()) {
		error = "no scratch directory";
		return {};
	}
	const uint64_t n = next_scratch_.fetch_add(1);
	return io::utf8_path(scratch_.path() / (std::string(what) + "-" + std::to_string(n)));
}

void ProjectRoutes::register_routes(WebApp &app) {
	std::string error;
	scratch_ = ScratchDir::make("onnet-projects-", error);
	if (scratch_.empty())
		std::fprintf(stderr,
				"[http] WARN project routes: no scratch directory (%s); their file routes "
				"answer 503\n",
				error.c_str());

	CROW_ROUTE(app, "/api/products")
			.methods("GET"_method)([this](const crow::request &req) { return list(req); });
	CROW_ROUTE(app, "/api/products").methods("POST"_method)([this](const crow::request &req) {
		return create(req);
	});
	CROW_ROUTE(app, "/api/products/<string>")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key) {
				return project(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>")
			.methods("PATCH"_method)([this](const crow::request &req, const std::string &key) {
				return update(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>")
			.methods("DELETE"_method)([this](const crow::request &req, const std::string &key) {
				return remove(req, key);
			});
	CROW_ROUTE(app, "/api/me/projects").methods("GET"_method)([this](const crow::request &req) {
		return mine(req);
	});
	CROW_ROUTE(app, "/api/products/<string>/revisions")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key) {
				return revisions(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>/revisions/<uint>/manifest")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key,
										   uint64_t number) { return manifest(req, key, number); });
	CROW_ROUTE(app, "/api/products/<string>/blobs/<string>")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key,
										   const std::string &sha) { return blob(req, key, sha); });
	CROW_ROUTE(app, "/api/products/<string>/downloads")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return downloads(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>/missing")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return missing(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>/blobs/<string>")
			.methods("PUT"_method)(
					[this](const crow::request &req, const std::string &key,
							const std::string &sha) { return upload(req, key, sha); });
	CROW_ROUTE(app, "/api/products/<string>/revisions")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return push(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>/releases")
			.methods("GET"_method)([this](const crow::request &req, const std::string &key) {
				return releases(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>/releases")
			.methods("POST"_method)([this](const crow::request &req, const std::string &key) {
				return release(req, key);
			});
	CROW_ROUTE(app, "/api/products/<string>/releases/<uint>/publish")
			.methods("POST"_method)(
					[this](const crow::request &req, const std::string &key, uint64_t id) {
						return release_change(req, key, id, ReleaseChange::Publish);
					});
	CROW_ROUTE(app, "/api/products/<string>/releases/<uint>/retire")
			.methods("POST"_method)(
					[this](const crow::request &req, const std::string &key, uint64_t id) {
						return release_change(req, key, id, ReleaseChange::Retire);
					});
	CROW_ROUTE(app, "/api/products/<string>/releases/<uint>")
			.methods("DELETE"_method)(
					[this](const crow::request &req, const std::string &key, uint64_t id) {
						return release_change(req, key, id, ReleaseChange::Delete);
					});
	CROW_ROUTE(app, "/api/mods").methods("GET"_method)([this](const crow::request &req) {
		return mods(req);
	});
}

void ProjectRoutes::audit(
		const crow::request &req, const ProductRow &project, const WebSessionUser &user) {
	if (project.owner_id != user.id)
		access_.note_admin_actor(req, user);
}

// GET /api/products — {"projects"} by key: the public ones and the caller's own, and for an admin
// every one.
crow::response ProjectRoutes::list(const crow::request &req) {
	const ProjectCaller caller = project_caller(access_.identify(req));
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto games = games_by_id(*db_conn);
		const catalog::GameCatalog lobby_table = catalog::GameCatalog::load(*db_conn);
		std::vector<crow::json::wvalue> projects;
		for (const ProductRow &p : hosted::list_projects(*db_conn, caller.admin, caller.user_id))
			projects.push_back(project_json(*db_conn, p, caller, games, lobby_table));
		out["projects"] = std::move(projects);
	} catch (const db::Error &e) {
		return db_failure("GET /api/products", e);
	}
	return no_store(json_reply(200, out));
}

// POST /api/products — {"key", "display_name", "kind", "game"?, "base"?, "expansion_code"?,
// "visibility", "lobby"? or "new_lobby_name"?}: a project of the caller's own (its owner), on
// its own session with the CSRF header: a game (kind "game", game the title it runs as: jo, dfx2 or
// acidgambit; exactly one existing lobby or new lobby name) or an expansion (kind "expansion", base
// a game's key: one of the service's own, public or private (a mod of the installed Joint
// Operations builds on JO), or one of the caller's own; expansion_code as the admin's create takes
// it, unique across every product without case; a game named beside it must be the base's), its
// visibility as the admin's create takes it, its key 1 to kOwnedKeyMax (11) of A-Z, 0-9 and _
// without "QQ" (project_repository.h valid_owned_key). Its updater product is its key
// (updater_login KEY, command_file KEY.CMD), a game's download hosted::own_download's. A null
// optional member is an absent one. One that passes its shapes draws a token from the account's
// kCreatePerUser bucket (429). 201 {"project"}; 400 invalid_json, missing_field, invalid_field
// (each with the field); 403 too_many_projects {limit} (it owns kOwnedProjectsMax; an admin's own
// are not counted); 404 no_such_base; 409 duplicate_key (another product has the key, or its
// updater product or download name), duplicate_expansion {field: "expansion_code"} (another mod, of
// any game, has the code).
crow::response ProjectRoutes::create(const crow::request &req) {
	static constexpr const char *kRoute = "POST /api/products";
	auto caller = access_.require_user(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const WebSessionUser &user = *caller.user;
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const char *wrong = nullptr;
	const auto note = [&body, &wrong](const char *member) {
		if (body->wrong_type() && wrong == nullptr)
			wrong = member;
	};
	const auto key = body->text("key");
	note("key");
	const auto display_name = body->text("display_name");
	note("display_name");
	const auto kind_name = body->text("kind");
	note("kind");
	const auto game = optional_text(*body, "game");
	note("game");
	const auto base = optional_text(*body, "base");
	note("base");
	const auto expansion_code = optional_text(*body, "expansion_code");
	note("expansion_code");
	const auto visibility = body->text("visibility");
	note("visibility");
	const auto lobby = optional_text(*body, "lobby");
	note("lobby");
	const auto new_lobby_name = optional_text(*body, "new_lobby_name");
	note("new_lobby_name");
	if (wrong != nullptr)
		return field_error(400, "invalid_field", wrong, "the member has the wrong type");
	if (!key)
		return field_error(400, "missing_field", "key", "every project has a key");
	if (!valid_owned_key(*key))
		return field_error(400, "invalid_field", "key",
				"key is 1 to 11 of A-Z, 0-9 and _, without QQ (the update service names the "
				"project by it)");
	if (!display_name)
		return field_error(400, "missing_field", "display_name", "display_name is required");
	if (!hosted::valid_display_name(*display_name))
		return field_error(400, "invalid_field", "display_name",
				"display_name is 1 to 64 characters with no control characters");
	if (!kind_name)
		return field_error(400, "missing_field", "kind", "kind is game or expansion");
	const auto kind = hosted::product_kind_from_name(*kind_name);
	if (!kind || *kind == hosted::ProductKind::App)
		return field_error(400, "invalid_field", "kind", "kind is game or expansion");
	if (*kind == hosted::ProductKind::Game) {
		// A game's reply names no base and an empty expansion_code: either echoed back is none.
		if (base && !base->empty())
			return field_error(400, "invalid_field", "base", "a game builds on no base");
		if (expansion_code && !expansion_code->empty())
			return field_error(
					400, "invalid_field", "expansion_code", "a game has no expansion code");
		if (!game)
			return field_error(400, "missing_field", "game", "a game runs as a title");
		if (!hosted::valid_title(*game))
			return field_error(400, "invalid_field", "game", "game is a title a game may run as");
	} else {
		if (!base)
			return field_error(400, "missing_field", "base", "an expansion builds on a game");
		if (!expansion_code)
			return field_error(400, "missing_field", "expansion_code", "an expansion has a code");
		if (!hosted::valid_expansion_code(*expansion_code))
			return field_error(400, "invalid_field", "expansion_code",
					"expansion_code is 1 to 8 of a-z and 0-9");
	}
	if (!visibility)
		return field_error(400, "missing_field", "visibility", "a project is public or private");
	if (!hosted::valid_visibility(*visibility))
		return field_error(400, "invalid_field", "visibility", "visibility is public or private");
	if (*kind == hosted::ProductKind::Game) {
		if (!lobby && !new_lobby_name)
			return field_error(400, "missing_field", "lobby", "choose a lobby or name a new lobby");
		if ((lobby && new_lobby_name) || (lobby && lobby->empty()))
			return field_error(400, "invalid_field", "lobby", "choose one existing or new lobby");
		if (new_lobby_name && !hosted::valid_display_name(*new_lobby_name))
			return field_error(400, "invalid_field", "new_lobby_name",
					"a lobby name is 1 to 64 characters with no control characters");
	} else if (lobby || new_lobby_name) {
		return field_error(
				400, "invalid_field", "lobby", "an expansion uses its base game's lobby");
	}
	const auto no_such_base = [] {
		return json_error(404, "no_such_base", "no game you may build on has this key");
	};
	// A create that reaches the database draws a token (a malformed one, refused above, none).
	if (const int64_t wait = create_per_user_.take(
				"projects|" + std::to_string(user.id), RateLimiter::Clock::now()))
		return too_many_requests(wait);

	hosted::NewProduct product;
	product.display_name = *display_name;
	product.kind = *kind;
	product.key = *key;
	product.visibility = *visibility;
	product.owner_id = user.id;
	product.lobby = lobby.value_or("");
	product.new_lobby_name = new_lobby_name.value_or("");
	product.created_by = (user.role == "admin" ? "admin " : "player ") + user.username;
	// Its updater product: the key names it to the update service (ADR 0074).
	product.updater_login = *key;
	product.command_file = *key + ".CMD";
	if (*kind == hosted::ProductKind::Game)
		product.download = hosted::own_download(*key);
	try {
		auto db_conn = pool_.acquire();
		if (*kind == hosted::ProductKind::Game) {
			product.title = *game;
		} else {
			// Another account's private game is no base, as no game is.
			const auto parent = hosted::find_product_by_key(*db_conn, *base);
			if (!parent || !may_build_on(*parent, project_caller(user)))
				return no_such_base();
			if (game && *game != parent->title)
				return field_error(
						400, "invalid_field", "game", "an expansion speaks its base game's title");
			product.parent_id = parent->id;
			product.expansion_code = *expansion_code;
		}
		int64_t id = 0;
		switch (hosted::create_product(
				*db_conn, product, id, catalog::LobbyChooser{ user.id, user.role == "admin" })) {
			case hosted::ProductOutcome::Ok:
				break;
			case hosted::ProductOutcome::TooManyProjects: {
				crow::json::wvalue refusal;
				refusal["error"] = "too_many_projects";
				refusal["message"] = "an account has at most " +
						std::to_string(hosted::kOwnedProjectsMax) + " projects";
				refusal["limit"] = hosted::kOwnedProjectsMax;
				return json_reply(403, refusal);
			}
			case hosted::ProductOutcome::DuplicateKey:
				return json_error(409, "duplicate_key", "another project has this key");
			case hosted::ProductOutcome::DuplicateLogin:
			case hosted::ProductOutcome::DuplicateCommandFile:
			case hosted::ProductOutcome::DuplicateDownload:
				return json_error(409, "duplicate_key",
						"another product's updater or download already goes by this key");
			case hosted::ProductOutcome::DuplicateExpansion:
				return hosted::duplicate_expansion();
			case hosted::ProductOutcome::NoSuchParent:
				return no_such_base();
			case hosted::ProductOutcome::InvalidLobby:
				return field_error(400, "invalid_field", "lobby",
						"choose an available lobby for this game's protocol");
			default:
				return json_error(500, "internal_error");
		}
		const auto made = hosted::find_product(*db_conn, id);
		if (!made)
			return json_error(500, "db_error");
		std::printf("[http] %s: %s (%s %lld) made by '%s'\n", kRoute, made->key.c_str(),
				hosted::product_kind_name(made->kind), static_cast<long long>(id),
				loggable(user.username).c_str());
		crow::json::wvalue out;
		out["project"] = project_json(*db_conn, *made, project_caller(user),
				base_of(*db_conn, *made), catalog::GameCatalog::load(*db_conn));
		return no_store(json_reply(201, out));
	} catch (const db::Error &e) {
		return db_failure(kRoute, e);
	}
}

// GET /api/products/<key> — {"project", "head_revision" (null before the first push)};
// 404 not_found.
crow::response ProjectRoutes::project(const crow::request &req, const std::string &key) {
	const ProjectCaller caller = project_caller(access_.identify(req));
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto p = readable(*db_conn, key, caller);
		if (!p)
			return no_such_project();
		out["project"] = project_json(
				*db_conn, *p, caller, base_of(*db_conn, *p), catalog::GameCatalog::load(*db_conn));
		const auto head = p->head > 0 ? find_revision(*db_conn, p->id, p->head) : std::nullopt;
		if (head)
			out["head_revision"] = revision_json(*head);
		else
			out["head_revision"] = nullptr;
	} catch (const db::Error &e) {
		return db_failure("GET /api/products/<key>", e);
	}
	return no_store(json_reply(200, out));
}

// PATCH /api/products/<key> — any of {"display_name", "visibility", "lobby", "new_lobby_name"} (the
// shapes the admin's create takes), on its own session with the CSRF header of one who may manage
// it (its owner or an admin): 200 {"project"}; 400 invalid_json, invalid_field (with the field);
// 403 forbidden (one the caller may read but not change); 404 not_found; 409 lobby_locked once any
// release build fixes the game's lobby. Its key and updater product remain admin-managed.
crow::response ProjectRoutes::update(const crow::request &req, const std::string &key) {
	static constexpr const char *kRoute = "PATCH /api/products/<key>";
	auto caller = access_.require_user(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const char *wrong = nullptr;
	const auto note = [&body, &wrong](const char *member) {
		if (body->wrong_type() && wrong == nullptr)
			wrong = member;
	};
	hosted::ProductPatch patch;
	patch.display_name = body->text("display_name");
	note("display_name");
	patch.visibility = body->text("visibility");
	note("visibility");
	patch.lobby = optional_text(*body, "lobby");
	note("lobby");
	patch.new_lobby_name = optional_text(*body, "new_lobby_name");
	note("new_lobby_name");
	if (wrong != nullptr)
		return field_error(400, "invalid_field", wrong, "the member has the wrong type");
	if (patch.display_name && !hosted::valid_display_name(*patch.display_name))
		return field_error(400, "invalid_field", "display_name",
				"display_name is 1 to 64 characters with no control characters");
	if (patch.visibility && !hosted::valid_visibility(*patch.visibility))
		return field_error(400, "invalid_field", "visibility", "visibility is public or private");
	if (patch.new_lobby_name && !hosted::valid_display_name(*patch.new_lobby_name))
		return field_error(400, "invalid_field", "new_lobby_name",
				"a lobby name is 1 to 64 characters with no control characters");
	if ((patch.lobby && patch.new_lobby_name) || (patch.lobby && patch.lobby->empty()))
		return field_error(400, "invalid_field", "lobby", "choose one existing or new lobby");
	const ProjectCaller who = project_caller(*caller.user);
	try {
		auto db_conn = pool_.acquire();
		const auto p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).manage)
			return manage_forbidden();
		if (patch.new_lobby_name) {
			if (p->kind != hosted::ProductKind::Game)
				return field_error(
						400, "invalid_field", "lobby", "an expansion uses its base game's lobby");
			if (p->lobby_locked && !p->lobby.empty())
				return json_error(
						409, "lobby_locked", "the lobby is fixed once a release build starts");
			// A new lobby is retained service data, just as one made with a project is.
			if (const int64_t wait = create_per_user_.take(
						"projects|" + std::to_string(caller.user->id), RateLimiter::Clock::now()))
				return too_many_requests(wait);
		}
		audit(req, *p, *caller.user);
		switch (hosted::update_product(
				*db_conn, p->id, patch, catalog::LobbyChooser{ who.user_id, who.admin })) {
			case hosted::ProductOutcome::Ok:
				break;
			case hosted::ProductOutcome::NotFound:
				return no_such_project();
			case hosted::ProductOutcome::InvalidLobby:
				return field_error(400, "invalid_field", "lobby",
						"choose one available lobby for this game's protocol");
			case hosted::ProductOutcome::LobbyLocked:
				return json_error(
						409, "lobby_locked", "the lobby is fixed once a release build starts");
			default:
				return json_error(500, "internal_error");
		}
		const auto changed = hosted::find_product(*db_conn, p->id);
		if (!changed)
			return no_such_project();
		std::printf("[http] %s: %s changed by '%s'\n", kRoute, changed->key.c_str(),
				loggable(caller.user->username).c_str());
		crow::json::wvalue out;
		out["project"] = project_json(*db_conn, *changed, who, base_of(*db_conn, *changed),
				catalog::GameCatalog::load(*db_conn));
		return no_store(json_reply(200, out));
	} catch (const db::Error &e) {
		return db_failure(kRoute, e);
	}
}

// DELETE /api/products/<key> — the project, on its own session with the CSRF header of one who
// may manage it (its owner or an admin), deleted as the admin's delete takes a product
// (product_jobs.h delete_product_purged: its revisions' rows with it and, with a store, its
// objects' product.purge queued in the same transaction): 204; 403 forbidden (one the caller may
// read but not change); 404 not_found; 409 in_use while a live server runs it, has_expansion while
// an expansion builds on it.
crow::response ProjectRoutes::remove(const crow::request &req, const std::string &key) {
	static constexpr const char *kRoute = "DELETE /api/products/<key>";
	auto caller = access_.require_user(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const ProjectCaller who = project_caller(*caller.user);
	std::optional<ProductRow> p;
	try {
		auto db_conn = pool_.acquire();
		p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).manage)
			return manage_forbidden();
		audit(req, *p, *caller.user);
		// With no store there are no objects: nothing to purge.
		switch (hosted::delete_product_purged(*db_conn, p->id, store_ != nullptr)) {
			case hosted::ProductOutcome::Ok:
				break;
			case hosted::ProductOutcome::NotFound:
				return no_such_project();
			case hosted::ProductOutcome::InUse:
				return hosted::product_in_use();
			case hosted::ProductOutcome::HasExpansion:
				return hosted::product_has_expansion();
			default:
				return json_error(500, "internal_error");
		}
	} catch (const db::Error &e) {
		return db_failure(kRoute, e);
	}
	std::printf("[http] %s: %s (%lld) deleted by '%s'%s\n", kRoute, p->key.c_str(),
			static_cast<long long>(p->id), loggable(caller.user->username).c_str(),
			store_ != nullptr ? "; its objects' purge queued" : "");
	return crow::response(204);
}

// GET /api/me/projects — {"projects", "usage": {"projects", "bytes", "release_bytes"}, "limits":
// {"projects", "bytes"}, "games", "lobbies", "titles", "protocols"}: the caller's own
// projects by key (each project_json's), how many there are and the bytes their stores and their
// releases' packages hold (release_bytes the packages alone), and the most an account may have
// of each (kOwnedProjectsMax, kOwnedBytesMax); and what a new project of its may be: the games it
// may build an expansion on (POST /api/products' base: the service's own, public or private, and
// its own), each {"key", "title" (its name), "game" (the title it runs as)} by key, the lobbies its
// games may select (each {slug, display_name, protocol}: the {code, name, version} its clients
// speak), the titles a game of its may run as (hosted::game_titles) and, by each one's code, the
// protocol it speaks ("protocols": {"jo": {code, name, version}, ...}, what a new game's
// lobbies are chosen by). On its own session; 401 without one.
crow::response ProjectRoutes::mine(const crow::request &req) {
	auto caller = access_.require_user(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const ProjectCaller who = project_caller(*caller.user);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto games = games_by_id(*db_conn);
		const catalog::GameCatalog lobby_table = catalog::GameCatalog::load(*db_conn);
		std::vector<crow::json::wvalue> projects;
		for (const ProductRow &p : hosted::list_owned_projects(*db_conn, caller.user->id))
			projects.push_back(project_json(*db_conn, p, who, games, lobby_table));
		out["projects"] = std::move(projects);
		const OwnedUsage usage = owned_usage(*db_conn, caller.user->id);
		out["usage"]["projects"] = usage.projects;
		out["usage"]["bytes"] = usage.bytes;
		out["usage"]["release_bytes"] = usage.release_bytes;
		std::vector<const ProductRow *> bases;
		for (const auto &[id, game] : games)
			if (may_build_on(game, who))
				bases.push_back(&game);
		std::sort(bases.begin(), bases.end(),
				[](const ProductRow *a, const ProductRow *b) { return a->key < b->key; });
		std::vector<crow::json::wvalue> offered;
		for (const ProductRow *game : bases) {
			crow::json::wvalue g;
			g["key"] = game->key;
			g["title"] = game->display_name;
			g["game"] = game->title;
			offered.push_back(std::move(g));
		}
		out["games"] = std::move(offered);
		std::vector<crow::json::wvalue> lobbies;
		for (const catalog::GameRow &lobby : catalog::selectable_lobbies(
					 *db_conn, catalog::LobbyChooser{ who.user_id, who.admin })) {
			crow::json::wvalue item;
			item["slug"] = lobby.slug;
			item["display_name"] = lobby.display_name;
			protocol_or_null(item, "protocol", protocol_by_code(lobby.protocol));
			lobbies.push_back(std::move(item));
		}
		out["lobbies"] = std::move(lobbies);
	} catch (const db::Error &e) {
		return db_failure("GET /api/me/projects", e);
	}
	out["limits"]["projects"] = hosted::kOwnedProjectsMax;
	out["limits"]["bytes"] = hosted::kOwnedBytesMax;
	std::vector<crow::json::wvalue> titles;
	for (const char *title : hosted::game_titles()) {
		titles.push_back(crow::json::wvalue(title));
		protocol_or_null(out["protocols"], title, title_protocol(title));
	}
	out["titles"] = std::move(titles);
	return no_store(json_reply(200, out));
}

// GET /api/products/<key>/revisions?before=<n>&limit=<m> — {"revisions"}: at most `limit`
// (50, at most 200) of those numbered below `before` (0, or none: from the head), newest first;
// 400 invalid_query; 404 not_found.
crow::response ProjectRoutes::revisions(const crow::request &req, const std::string &key) {
	const ProjectCaller caller = project_caller(access_.identify(req));
	const auto before = query_whole(req, "before", 0, INT64_MAX);
	const auto limit = query_whole(req, "limit", kPageDefault, kPageMax);
	if (!before || !limit || *limit == 0)
		return json_error(400, "invalid_query",
				"before is a revision's number; limit is 1 to " + std::to_string(kPageMax));
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto p = readable(*db_conn, key, caller);
		if (!p)
			return no_such_project();
		std::vector<crow::json::wvalue> list;
		for (const ProjectRevision &r :
				list_revisions(*db_conn, p->id, *before, static_cast<int>(*limit)))
			list.push_back(revision_json(r));
		out["revisions"] = std::move(list);
	} catch (const db::Error &e) {
		return db_failure("GET /api/products/<key>/revisions", e);
	}
	return no_store(json_reply(200, out));
}

// GET /api/products/<key>/revisions/<n>/manifest — the manifest's canonical text
// (base/project/project_manifest.h), cached as immutable; 404 not_found; 503 store_unavailable.
crow::response ProjectRoutes::manifest(
		const crow::request &req, const std::string &key, uint64_t number) {
	const ProjectCaller caller = project_caller(access_.identify(req));
	if (store_ == nullptr)
		return store_unavailable();
	std::optional<ProductRow> p;
	std::optional<ProjectRevision> revision;
	try {
		auto db_conn = pool_.acquire();
		p = readable(*db_conn, key, caller);
		if (p)
			revision = find_revision(*db_conn, p->id, path_id(number));
	} catch (const db::Error &e) {
		return db_failure("GET /api/products/<key>/revisions/<n>/manifest", e);
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
		return store_failure("GET /api/products/<key>/revisions/<n>/manifest", error);
	crow::response res(200);
	res.body = std::move(text);
	res.set_header("Content-Type", "application/json");
	res.set_header("Cache-Control", immutable_cache(*p));
	res.set_header("ETag", "\"" + revision->manifest_sha256 + "\"");
	return res;
}

// GET /api/products/<key>/blobs/<sha256> — the file's bytes, cached as immutable; 400
// invalid_sha256; 404 not_found (the project, or a file its store does not hold); 503
// store_unavailable.
crow::response ProjectRoutes::blob(
		const crow::request &req, const std::string &key, const std::string &sha) {
	const ProjectCaller caller = project_caller(access_.identify(req));
	if (!opennova::project::is_sha256_hex(sha))
		return json_error(
				400, "invalid_sha256", "a file is named by its SHA-256 in lower-case hex");
	if (store_ == nullptr)
		return store_unavailable();
	std::optional<ProductRow> p;
	try {
		p = readable(*pool_.acquire(), key, caller);
	} catch (const db::Error &e) {
		return db_failure("GET /api/products/<key>/blobs/<sha256>", e);
	}
	if (!p)
		return no_such_project();
	const std::string object = blob_key(p->id, sha);
	if (urls_ != nullptr) {
		// No file passes through this program when the store signs its own URLs (ADR 0069 d6).
		SignedTransfer download;
		std::string error;
		if (!urls_->get(object, download, error))
			return store_failure("GET /api/products/<key>/blobs/<sha256>", error);
		crow::response res(302);
		res.set_header("Location", download.url);
		res.set_header("Cache-Control", "no-store");
		return res;
	}
	bool exists = false;
	uint64_t size = 0;
	std::string error;
	if (!store_->head(object, exists, size, error))
		return store_failure("GET /api/products/<key>/blobs/<sha256>", error);
	if (!exists)
		return json_error(404, "not_found", "the project's store holds no file of this SHA-256");
	if (size > kBodyMax)
		return json_error(413, "too_large", "the file is too large to send through this program");
	const std::string scratch = scratch_file("blob", error);
	std::string bytes;
	if (scratch.empty() || !read_object(*store_, object, scratch, bytes, error))
		return store_failure("GET /api/products/<key>/blobs/<sha256>", error);
	crow::response res(200);
	res.body = std::move(bytes);
	res.set_header("Content-Type", "application/octet-stream");
	res.set_header("Cache-Control", immutable_cache(*p));
	res.set_header("ETag", "\"" + sha + "\"");
	return res;
}

// POST /api/products/<key>/missing — {"blobs": [{"sha256", "size"}]} (at most 20000):
// {"missing": [{"sha256", "size", "upload": {"method", "url", "headers"}}]}, the files the
// project's store does not hold at their sizes, each with the request that uploads it; on the
// own session of one who may push it. 400 invalid_json, invalid_field; 401; 403 session_required,
// forbidden; 404 not_found; 413 quota_exceeded {used, limit} when an account's own project's
// files to upload, or the ones its store got through its own URLs since, would take its projects
// past kOwnedBytesMax together (none signed or recorded then).
crow::response ProjectRoutes::missing(const crow::request &req, const std::string &key) {
	auto caller = pusher(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
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
		if (!opennova::project::is_sha256_hex(blob.sha256) || blob.bytes < 0 ||
				static_cast<uint64_t>(blob.bytes) > opennova::project::kMaxFileBytes)
			return field_error(400, "invalid_field", "blobs",
					"each sha256 is 64 lower-case hex digits, each size 0 to 4 GiB");
		wanted.push_back(std::move(blob));
	}
	std::vector<StoredBlob> absent;
	int64_t product_id = 0;
	try {
		auto db_conn = pool_.acquire();
		const ProjectCaller who = project_caller(*caller.user);
		const auto p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).push)
			return push_forbidden();
		audit(req, *p, *caller.user);
		product_id = p->id;
		absent = missing_blobs(*db_conn, p->id, wanted);
		// A file an earlier push uploaded through the store's own URL but never committed (an
		// upload past its URL's hour, a push that failed) is held once the store has it, so a push
		// again uploads only the rest. An account's own projects hold at most kOwnedBytesMax
		// together, whoever pushes to them: no such file is recorded, and no upload signed, that
		// would take them past it.
		std::vector<StoredBlob> found;
		if (store_ != nullptr && !absent.empty()) {
			std::vector<StoredBlob> still;
			std::string error;
			if (!sort_by_store(*store_, p->id, absent, found, still, error))
				return store_failure("POST /api/products/<key>/missing", error);
			absent = std::move(still);
		}
		int64_t used = 0;
		if (p->owner_id && !(found.empty() && absent.empty()) &&
				!owner_has_room(*db_conn, *p->owner_id, bytes_of(found) + bytes_of(absent), used))
			return quota_exceeded(used);
		record_blobs(*db_conn, p->id, found);
	} catch (const db::Error &e) {
		return db_failure("POST /api/products/<key>/missing", e);
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
				return store_failure("POST /api/products/<key>/missing", error);
		} else {
			upload = { "PUT", "/api/products/" + key + "/blobs/" + blob.sha256,
				{ { "Content-Type", "application/octet-stream" } } };
		}
		entry["upload"] = transfer_json(upload);
		list.push_back(std::move(entry));
	}
	crow::json::wvalue out;
	out["missing"] = std::move(list);
	return no_store(json_reply(200, out));
}

// POST /api/products/<key>/downloads — {"hashes": [sha256]} (at most 1000): {"downloads":
// [{"sha256", "method", "url", "headers"}]}, each file's download: the store's own signed URL, or
// this program's blob route. Anyone who may read the project; 400 invalid_json, invalid_field; 404
// not_found.
crow::response ProjectRoutes::downloads(const crow::request &req, const std::string &key) {
	const ProjectCaller caller = project_caller(access_.identify(req));
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
				!opennova::project::is_sha256_hex(std::string(item.s())))
			return field_error(
					400, "invalid_field", "hashes", "each hash is 64 lower-case hex digits");
		hashes.emplace_back(item.s());
	}
	int64_t product_id = 0;
	try {
		const auto p = readable(*pool_.acquire(), key, caller);
		if (!p)
			return no_such_project();
		product_id = p->id;
	} catch (const db::Error &e) {
		return db_failure("POST /api/products/<key>/downloads", e);
	}
	std::vector<crow::json::wvalue> list;
	for (const std::string &sha : hashes) {
		SignedTransfer download;
		std::string error;
		if (urls_ != nullptr) {
			if (!urls_->get(blob_key(product_id, sha), download, error))
				return store_failure("POST /api/products/<key>/downloads", error);
		} else {
			download = { "GET", "/api/products/" + key + "/blobs/" + sha, {} };
		}
		crow::json::wvalue entry = transfer_json(download);
		entry["sha256"] = sha;
		list.push_back(std::move(entry));
	}
	crow::json::wvalue out;
	out["downloads"] = std::move(list);
	return no_store(json_reply(200, out));
}

// PUT /api/products/<key>/blobs/<sha256> — the file's bytes, put in the project's store
// and held there once their SHA-256 is the one named, on the own session of one who may push it:
// 200 {"sha256", "size"}; 400 invalid_sha256, sha256_mismatch; 401; 403 session_required,
// forbidden; 404 not_found; 413 too_large, quota_exceeded {used, limit} (a file an account's own
// projects do not hold yet that would take them past kOwnedBytesMax); 503 store_unavailable.
crow::response ProjectRoutes::upload(
		const crow::request &req, const std::string &key, const std::string &sha) {
	auto caller = pusher(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (!opennova::project::is_sha256_hex(sha))
		return json_error(
				400, "invalid_sha256", "a file is named by its SHA-256 in lower-case hex");
	if (req.body.size() > kBodyMax)
		return json_error(413, "too_large", "the file is too large to send through this program");
	if (io::sha256_hex(req.body.data(), req.body.size()) != sha)
		return json_error(400, "sha256_mismatch", "the bytes sent are not the file named");
	if (store_ == nullptr)
		return store_unavailable();
	const int64_t size = static_cast<int64_t>(req.body.size());
	try {
		auto db_conn = pool_.acquire();
		const ProjectCaller who = project_caller(*caller.user);
		const auto p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).push)
			return push_forbidden();
		audit(req, *p, *caller.user);
		int64_t used = 0;
		if (p->owner_id && !missing_blobs(*db_conn, p->id, { { sha, size } }).empty() &&
				!owner_has_room(*db_conn, *p->owner_id, size, used))
			return quota_exceeded(used);
		std::string error;
		const std::string scratch = scratch_file("upload", error);
		if (scratch.empty() ||
				!write_object(*store_, blob_key(p->id, sha), scratch, req.body, error))
			return store_failure("PUT /api/products/<key>/blobs/<sha256>", error);
		record_blobs(*db_conn, p->id, { { sha, size } });
	} catch (const db::Error &e) {
		return db_failure("PUT /api/products/<key>/blobs/<sha256>", e);
	}
	crow::json::wvalue out;
	out["sha256"] = sha;
	out["size"] = size;
	return no_store(json_reply(200, out));
}

// POST /api/products/<key>/revisions — {"parent", "message", "manifest"}: the manifest
// (its canonical text, base/project/project_manifest.h) pushed as the revision after `parent`,
// 201 {"revision", "head"}, on the own session of one who may push it (its owner or an admin);
// 400 invalid_json, missing_field, invalid_field, invalid_manifest, unchanged, missing_blobs
// {"hashes"}; 401; 403 session_required, forbidden; 404 not_found; 409 stale {"head"}; 413
// quota_exceeded {used, limit} (files its store got through its own URLs that would take an
// account's own projects past kOwnedBytesMax together, none recorded); 503 store_unavailable.
crow::response ProjectRoutes::push(const crow::request &req, const std::string &key) {
	static constexpr const char *kRoute = "POST /api/products/<key>/revisions";
	auto caller = pusher(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
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
	std::vector<ProjectFile> files;
	std::string why;
	if (!opennova::project::decode_manifest(*manifest_text, files, why))
		return field_error(400, "invalid_manifest", "manifest", why);
	for (const ProjectFile &file : files)
		if (opennova::project::is_service_managed_file(file.path))
			return field_error(400, "invalid_manifest", "manifest",
					file.path + " is NovaWorld's: a build generates gt.ssc from the project's "
								"lobby and an expansion's version.txt from its release");
	const std::string manifest_sha = opennova::project::manifest_sha256(*manifest_text);
	if (store_ == nullptr)
		return store_unavailable();
	try {
		auto db_conn = pool_.acquire();
		const ProjectCaller who = project_caller(*caller.user);
		const auto p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).push)
			return push_forbidden();
		audit(req, *p, *caller.user);
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
		for (const ProjectFile &file : files)
			listed.push_back({ file.sha256, static_cast<int64_t>(file.size) });
		std::vector<StoredBlob> found, absent;
		std::string held_error;
		if (!sort_by_store(*store_, p->id, missing_blobs(*db_conn, p->id, listed), found, absent,
					held_error))
			return store_failure(kRoute, held_error);
		int64_t used = 0;
		if (p->owner_id && !found.empty() &&
				!owner_has_room(*db_conn, *p->owner_id, bytes_of(found), used))
			return quota_exceeded(used);
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
		revision.bytes = static_cast<int64_t>(opennova::project::total_bytes(files));
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

// GET /api/products/<key>/releases — {"releases"}: the project's releases, newest first, each
// as the admin's (hosted::release_json), for one who may release it, on its own session; 401; 403
// forbidden (one the caller may read but not release); 404 not_found.
crow::response ProjectRoutes::releases(const crow::request &req, const std::string &key) {
	auto caller = access_.require_user(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const ProjectCaller who = project_caller(*caller.user);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).release)
			return release_forbidden();
		std::vector<crow::json::wvalue> list;
		for (const hosted::ReleaseRow &r : hosted::list_releases_of(*db_conn, p->id))
			list.push_back(hosted::release_json(*db_conn, r));
		out["releases"] = std::move(list);
	} catch (const db::Error &e) {
		return db_failure("GET /api/products/<key>/releases", e);
	}
	return no_store(json_reply(200, out));
}

// POST /api/products/<key>/releases — {"version", "revision"?, "base_revision"?}: a release of the
// project that the data program builds (products/release_build.h) from the revision (the head
// when none is named) and, for an expansion's, its base game's (its head), its package's version
// `version`: 201 {"release"}, building, its release.build queued in the same transaction, its
// created_by "user <name>" or "admin <name>"; on its own session with the CSRF header of one who
// may release it (its owner or an admin). An account that is no admin starts one at a time and
// kReleasesPerDay a UTC day (a deleted one still counts), and none while its own projects' usage
// is at kOwnedBytesMax. 400 invalid_json, invalid_field, no_revision (no such revision, none
// pushed yet, or no base game); 401; 403 forbidden; 404 not_found; 409 release_in_progress (one
// of its own projects' releases is building or validating); 413 quota_exceeded {used, limit}; 429
// too_many_releases {limit} with Retry-After (the seconds to the next UTC midnight); 503
// store_unavailable.
crow::response ProjectRoutes::release(const crow::request &req, const std::string &key) {
	static constexpr const char *kRoute = "POST /api/products/<key>/releases";
	auto caller = access_.require_user(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const WebSessionUser &user = *caller.user;
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
	// A release's version, as a command file's VERSION reads (Version's one rule).
	if (!version || !Version::parse(*version))
		return field_error(400, "invalid_field", "version",
				"a version is one to four numbers between dots: 0.2.0, 1.0");
	const ProjectCaller who = project_caller(user);
	try {
		auto db_conn = pool_.acquire();
		db::Transaction tx(*db_conn);
		const auto p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).release)
			return release_forbidden();
		audit(req, *p, user);
		const int64_t number = revision.value_or(p->head);
		if (number <= 0 || !find_revision(*db_conn, p->id, number))
			return json_error(400, "no_revision", "the project has no such revision");
		int64_t base_number = 0;
		if (p->parent_id) {
			const auto base = hosted::find_product(*db_conn, *p->parent_id);
			base_number = base ? base_revision.value_or(base->head) : 0;
			if (!base || base_number <= 0 || !find_revision(*db_conn, base->id, base_number))
				return json_error(
						400, "no_revision", "the base game's project has no such revision");
		}
		// An admin's start takes no limit. Another's holds its own row while its releases are
		// counted, so two starts of one account's wait on each other here.
		if (user.role != "admin") {
			db_conn->query("SELECT 1 FROM players WHERE id = $1 FOR NO KEY UPDATE;",
					{ db::BindValue(user.id) });
			if (hosted::count_owned_releases_in_flight(*db_conn, user.id) > 0)
				return json_error(409, "release_in_progress",
						"one of your releases is building; start another once it is done");
			if (p->owner_id) {
				const int64_t used = owned_usage(*db_conn, *p->owner_id).bytes;
				if (used >= hosted::kOwnedBytesMax)
					return quota_exceeded(used);
			}
			if (!hosted::take_release_start(*db_conn, user.id, kReleasesPerDay)) {
				crow::json::wvalue refusal;
				refusal["error"] = "too_many_releases";
				refusal["message"] = "an account starts at most " +
						std::to_string(kReleasesPerDay) + " releases a day (UTC)";
				refusal["limit"] = kReleasesPerDay;
				crow::response res = json_reply(429, refusal);
				res.set_header("Retry-After", std::to_string(seconds_to_utc_midnight()));
				return res;
			}
		}
		hosted::NewBuiltRelease made;
		made.name = p->key + " " + *version;
		made.created_by = (user.role == "admin" ? "admin " : "user ") + user.username;
		made.product_id = p->id;
		made.revision = number;
		made.base_revision = base_number;
		made.version = *version;
		const int64_t id = hosted::create_built_release(*db_conn, made);
		hosted::enqueue_release_build(*db_conn, id);
		tx.commit();
		const auto row = hosted::find_release(*db_conn, id);
		if (!row)
			return json_error(500, "db_error");
		std::printf("[http] %s: %s r%lld, release %lld building, started by '%s'\n", kRoute,
				p->key.c_str(), static_cast<long long>(number), static_cast<long long>(id),
				loggable(user.username).c_str());
		crow::json::wvalue out;
		out["release"] = hosted::release_json(*db_conn, *row);
		return no_store(json_reply(201, out));
	} catch (const db::Error &e) {
		return db_failure(kRoute, e);
	}
}

// POST /api/products/<key>/releases/<id>/publish, POST /api/products/<key>/releases/<id>/retire
// and DELETE /api/products/<key>/releases/<id> — one of the project's releases published,
// retired or deleted as the admin's release routes do it (release_routes.h
// publish_release_reply, retire_release_reply, delete_release_reply: 202 {"release"} or 204, and
// their refusals), on its own session with the CSRF header of one who may release it (its owner
// or an admin). An owner who is no admin publishes for approval (ADR 0074): its draft waits for
// an admin's publish (submit_release_reply: awaiting_approval, 202 {"release"}; 409 not_draft),
// and its delete withdraws one awaiting. 401; 403 forbidden; 404 not_found (no project the
// caller may read, or no release of the project's).
crow::response ProjectRoutes::release_change(
		const crow::request &req, const std::string &key, uint64_t id, ReleaseChange change) {
	auto caller = access_.require_user(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const WebSessionUser &user = *caller.user;
	const ProjectCaller who = project_caller(user);
	try {
		auto db_conn = pool_.acquire();
		const auto p = readable(*db_conn, key, who);
		if (!p)
			return no_such_project();
		if (!project_rights(*p, who).release)
			return release_forbidden();
		const auto release = hosted::find_release(*db_conn, path_id(id));
		if (!release || release->product_id != p->id)
			return no_such_release();
		audit(req, *p, user);
		const std::string actor = (user.role == "admin" ? "admin " : "user ") + user.username;
		switch (change) {
			case ReleaseChange::Publish:
				return user.role == "admin"
						? hosted::publish_release_reply(*db_conn, *release, actor)
						: hosted::submit_release_reply(*db_conn, *release, actor);
			case ReleaseChange::Retire:
				return hosted::retire_release_reply(*db_conn, *release, actor);
			case ReleaseChange::Delete:
				break;
		}
		return hosted::delete_release_reply(*db_conn, store_, *release, actor);
	} catch (const db::Error &e) {
		return db_failure("/api/products/<key>/releases/<id>", e);
	}
}

// GET /api/mods?game=<command file> — {"mods": [{"key", "code", "name", "owner", "version",
// "updater_login", "command_file"}]}: the mods a game's players may install (ADR 0074), the
// expansions with an updater product and a published release whose base game's command file is
// `game` (JOPTR.CMD, DFX2.CMD, ACIDGAMBIT.CMD, an account's own game's <KEY>.CMD; without case),
// the public ones and, with the session of their owner, its private ones, by key (code its
// expansion code, the expansion/<code>/ folder; owner its owner's username, null for the
// service's own; version its published release's). Anyone's, signed in or not; 400
// invalid_query with no game.
crow::response ProjectRoutes::mods(const crow::request &req) {
	const char *game = req.url_params.get("game");
	if (game == nullptr || *game == '\0')
		return json_error(400, "invalid_query", "game is a base game's command file: JOPTR.CMD");
	const RequestCaller caller = access_.identify(req);
	std::optional<int64_t> reader;
	if (caller.user)
		reader = caller.user->id;
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		std::vector<crow::json::wvalue> list;
		for (const hosted::ModRow &m : hosted::list_mods(*db_conn, game, reader)) {
			crow::json::wvalue mod;
			mod["key"] = m.key;
			mod["code"] = m.code;
			mod["name"] = m.name;
			if (m.owner)
				mod["owner"] = *m.owner;
			else
				mod["owner"] = nullptr;
			mod["version"] = m.version;
			mod["updater_login"] = m.updater_login;
			mod["command_file"] = m.command_file;
			list.push_back(std::move(mod));
		}
		out["mods"] = std::move(list);
	} catch (const db::Error &e) {
		return db_failure("GET /api/mods", e);
	}
	return no_store(json_reply(200, out));
}

} // namespace opennova::novaworld_server::projects
