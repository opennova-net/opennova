#include "data_set_routes.h"

#include "catalog_repository.h"
#include "hosted/data_set_jobs.h"
#include "hosted/data_set_files.h"
#include "hosted/data_set_repository.h"
#include "http_json.h"
#include "loggable.h"
#include "object_store.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

// The titles a base data set may speak (the gameprofile codes opennova-serve's /game takes).
constexpr std::array<const char *, 2> kTitles = { "jo", "dfx2" };

int64_t id_of(uint64_t id) {
	return id <= static_cast<uint64_t>(INT64_MAX) ? static_cast<int64_t>(id) : 0;
}

// Who made a row: the admin session's account, or the Bearer token.
std::string creator_of(const RouteAccess &caller) {
	return caller.user ? "admin " + caller.user->username : std::string("token");
}

crow::response db_failure(const char *route, const db::Error &e) {
	std::fprintf(stderr, "[http] %s failed: %s\n", route, e.what());
	return json_error(500, "db_error");
}

// {"error", "field", "message"}: which member the request got wrong.
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

// A store call that failed mid-way, logged with the store's own text (never sent).
crow::response store_failure(const char *route, const std::string &error) {
	std::fprintf(stderr, "[http] %s: the object store failed: %s\n", route, error.c_str());
	return json_error(502, "store_error", "the object store did not take the request");
}

crow::response no_such_data_set() {
	return json_error(404, "not_found", "no data set has this id");
}

crow::response no_such_tree() {
	return json_error(404, "not_found", "this data set has no tree with this id");
}

bool printable_ascii(std::string_view s) {
	return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x21 && c <= 0x7E; });
}

bool valid_display_name(std::string_view name) {
	const std::string_view trimmed = strutil::trim_view(name);
	return !trimmed.empty() && trimmed.size() == name.size() &&
			strutil::utf8_length(name) <= DataSetRoutes::kDisplayNameMax &&
			!has_control_byte(name) && !has_c1_control(name);
}

bool valid_title(std::string_view title) {
	return std::find(kTitles.begin(), kTitles.end(), title) != kTitles.end();
}

bool valid_expansion_code(std::string_view code) {
	return !code.empty() && code.size() <= DataSetRoutes::kExpansionCodeMax &&
			std::all_of(code.begin(), code.end(),
					[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool valid_updater_login(std::string_view login) {
	return !login.empty() && login.size() <= DataSetRoutes::kUpdaterLoginMax &&
			printable_ascii(login);
}

bool valid_command_file(std::string_view name) {
	return name.size() > 4 && name.size() <= DataSetRoutes::kCommandFileMax &&
			printable_ascii(name) && strutil::ends_with_icase(name, ".CMD");
}

// A multipart upload's id as the store minted it: S3's tokens and the local store's UUIDs are
// both printable and bounded.
bool valid_upload_id(std::string_view id) {
	return !id.empty() && id.size() <= 1024 && printable_ascii(id);
}

void id_or_null(crow::json::wvalue &out, const char *key, std::optional<int64_t> id) {
	if (id)
		out[key] = *id;
	else
		out[key] = nullptr;
}

// A data set as the admin's JSON, with its live trees (pruned ones left out).
crow::json::wvalue data_set_json(db::Database &db, const DataSetRow &d) {
	crow::json::wvalue out;
	out["id"] = d.id;
	out["display_name"] = d.display_name;
	out["kind"] = data_set_kind_name(d.kind);
	id_or_null(out, "parent_id", d.parent_id);
	out["expansion_code"] = d.expansion_code;
	out["title"] = d.title;
	out["game"] = d.game;
	out["updater_login"] = d.updater_login;
	out["command_file"] = d.command_file;
	out["retail_updater"] = d.retail_updater;
	std::optional<DataSetTree> current;
	if (d.current_tree_id)
		current = find_tree(db, *d.current_tree_id);
	if (current) {
		out["current_tree"]["id"] = current->id;
		out["current_tree"]["version"] = current->version_text;
		out["current_tree"]["ready_at"] = current->ready_at;
	} else {
		out["current_tree"] = nullptr;
	}
	id_or_null(out, "baseline_tree_id", d.baseline_tree_id);
	std::optional<ReleaseRow> published;
	if (d.published_release_id)
		published = find_release(db, *d.published_release_id);
	if (published) {
		out["published_release"]["id"] = published->id;
		out["published_release"]["name"] = published->name;
		out["published_release"]["sha256"] = published->sha256;
	} else {
		out["published_release"] = nullptr;
	}
	out["servers_live"] = count_live_servers_of(db, d.id);
	out["servers_outdated"] = count_outdated_servers_of(db, d.id);
	std::vector<crow::json::wvalue> trees;
	for (const DataSetTree &t : list_trees(db, d.id))
		if (t.state != TreeState::Pruned)
			trees.push_back(tree_json(db, t));
	out["trees"] = std::move(trees);
	return out;
}

// The tree `tree` of data set `id`, or none.
std::optional<DataSetTree> tree_of(db::Database &db, int64_t id, int64_t tree) {
	std::optional<DataSetTree> t = find_tree(db, tree);
	if (t && t->data_set_id != id)
		t.reset();
	return t;
}

// A JSON member read with its type checked (crow's rvalue throws on a wrong-typed read).
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

std::string tree_file_key(int64_t tree_id, std::string_view name) {
	return tree_prefix(tree_id) + std::string(name);
}

crow::json::wvalue tree_json(db::Database &db, const DataSetTree &t) {
	crow::json::wvalue out;
	out["id"] = t.id;
	out["state"] = tree_state_name(t.state);
	out["source"] = tree_source_name(t.source);
	id_or_null(out, "release_id", t.release_id);
	out["version"] = t.version_text;
	out["bytes"] = t.bytes;
	out["files"] = static_cast<int64_t>(t.files.size());
	out["missions"] = static_cast<int64_t>(t.missions.size());
	out["pinned_by"] = count_tree_pins(db, t.id);
	out["error"] = t.error;
	out["created_by"] = t.created_by;
	out["created_at"] = t.created_at;
	out["ready_at"] = t.ready_at; // "" until ready
	return out;
}

void DataSetRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/data-sets").methods("GET"_method)([this](const crow::request &req) {
		return list(req);
	});
	CROW_ROUTE(app, "/api/admin/data-sets")
			.methods("POST"_method)([this](const crow::request &req) { return create(req); });
	CROW_ROUTE(app, "/api/admin/data-sets/<uint>")
			.methods("PATCH"_method)(
					[this](const crow::request &req, uint64_t id) { return patch(req, id); });
	CROW_ROUTE(app, "/api/admin/data-sets/<uint>")
			.methods("DELETE"_method)(
					[this](const crow::request &req, uint64_t id) { return remove(req, id); });
	CROW_ROUTE(app, "/api/admin/data-sets/<uint>/trees")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return begin_tree(req, id); });
	CROW_ROUTE(app, "/api/admin/data-sets/<uint>/trees/<uint>/files/<string>/parts/<uint>")
			.methods("PUT"_method)([this](const crow::request &req, uint64_t id, uint64_t tree,
										   const std::string &name, uint64_t part) {
				return tree_file_part(req, id, tree, name, part);
			});
	CROW_ROUTE(app, "/api/admin/data-sets/<uint>/trees/<uint>/files/<string>/complete")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id, uint64_t tree,
											const std::string &name) {
				return tree_file_complete(req, id, tree, name);
			});
	CROW_ROUTE(app, "/api/admin/data-sets/<uint>/trees/<uint>/finalize")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id, uint64_t tree) {
				return tree_finalize(req, id, tree);
			});
	CROW_ROUTE(app, "/api/admin/data-sets/<uint>/trees/<uint>")
			.methods("DELETE"_method)([this](const crow::request &req, uint64_t id, uint64_t tree) {
				return tree_remove(req, id, tree);
			});
}

// GET /api/admin/data-sets — {"store" ({"kind": "s3" or "local"}, null with none), "titles"
// (what a base may speak), "games" (the lobbies, each {slug, display_name}), "data_sets"}:
// each data set {id, display_name, kind, parent_id, expansion_code, title, game, updater_login,
// command_file, retail_updater, current_tree ({id, version, ready_at} or null),
// baseline_tree_id, published_release ({id, name, sha256} or null), servers_live,
// servers_outdated, trees (its trees newest first, pruned ones left out, each as tree_json)}.
crow::response DataSetRoutes::list(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	crow::json::wvalue out;
	if (store_ != nullptr)
		out["store"]["kind"] = store_->kind();
	else
		out["store"] = nullptr;
	std::vector<crow::json::wvalue> titles;
	for (const char *title : kTitles)
		titles.push_back(crow::json::wvalue(title));
	out["titles"] = std::move(titles);
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		std::vector<crow::json::wvalue> games;
		for (const catalog::GameRow &game : catalog::list_games(*db_conn)) {
			crow::json::wvalue g;
			g["slug"] = game.slug;
			g["display_name"] = game.display_name;
			games.push_back(std::move(g));
		}
		out["games"] = std::move(games);
		std::vector<crow::json::wvalue> data_sets;
		for (const DataSetRow &d : list_data_sets(*db_conn))
			data_sets.push_back(data_set_json(*db_conn, d));
		out["data_sets"] = std::move(data_sets);
	} catch (const db::Error &e) {
		return db_failure("GET /api/admin/data-sets", e);
	}
	return no_store(json_reply(200, out));
}

// POST /api/admin/data-sets — {display_name, kind, parent_id?, expansion_code?, title?, game?,
// updater_login?, command_file?, retail_updater?}: a new base (title one of the titles, game a
// games slug) or expansion (parent_id a base, expansion_code [a-z0-9]{1,8}); the updater's
// product both or neither (a login of 1..32 printable characters, a command file of 1..15
// ending .CMD). 201 {"data_set"}; 400 invalid_json, missing_field, invalid_field (with the
// field); 409 duplicate_login, duplicate_expansion, no_such_parent.
crow::response DataSetRoutes::create(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const char *wrong = nullptr;
	const auto note = [&body, &wrong](const char *key) {
		if (body->wrong_type() && wrong == nullptr)
			wrong = key;
	};
	const auto display_name = body->text("display_name");
	note("display_name");
	const auto kind_name = body->text("kind");
	note("kind");
	const auto parent_id = body->integer("parent_id");
	note("parent_id");
	const auto expansion_code = body->text("expansion_code");
	note("expansion_code");
	const auto title = body->text("title");
	note("title");
	const auto game = body->text("game");
	note("game");
	const auto updater_login = body->text("updater_login");
	note("updater_login");
	const auto command_file = body->text("command_file");
	note("command_file");
	const auto retail_updater = body->flag("retail_updater");
	note("retail_updater");
	if (wrong != nullptr)
		return field_error(400, "invalid_field", wrong, "the member has the wrong type");
	if (!display_name)
		return field_error(400, "missing_field", "display_name", "display_name is required");
	if (!kind_name)
		return field_error(400, "missing_field", "kind", "kind is base or expansion");
	if (!valid_display_name(*display_name))
		return field_error(400, "invalid_field", "display_name",
				"display_name is 1 to 64 characters with no control characters");
	const auto kind = data_set_kind_from_name(*kind_name);
	if (!kind)
		return field_error(400, "invalid_field", "kind", "kind is base or expansion");

	NewDataSet data_set;
	data_set.display_name = *display_name;
	data_set.kind = *kind;
	data_set.created_by = creator_of(caller);
	if (*kind == DataSetKind::Base) {
		if (parent_id || expansion_code)
			return field_error(400, "invalid_field", parent_id ? "parent_id" : "expansion_code",
					"a base has no parent and no expansion code");
		if (!title)
			return field_error(400, "missing_field", "title", "a base speaks a title");
		if (!valid_title(*title))
			return field_error(400, "invalid_field", "title", "title is jo or dfx2");
		if (!game)
			return field_error(
					400, "missing_field", "game", "a base lists in a lobby (a games slug)");
		data_set.title = *title;
		data_set.game = *game;
	} else {
		if (title || game)
			return field_error(400, "invalid_field", title ? "title" : "game",
					"an expansion speaks its parent's title and lobby");
		if (!parent_id)
			return field_error(400, "missing_field", "parent_id", "an expansion has a base parent");
		if (!expansion_code)
			return field_error(400, "missing_field", "expansion_code", "an expansion has a code");
		if (!valid_expansion_code(*expansion_code))
			return field_error(400, "invalid_field", "expansion_code",
					"expansion_code is 1 to 8 of a-z and 0-9");
		data_set.parent_id = *parent_id;
		data_set.expansion_code = *expansion_code;
	}
	if (updater_login.has_value() != command_file.has_value())
		return field_error(400, "invalid_field", updater_login ? "command_file" : "updater_login",
				"updater_login and command_file come together");
	if (updater_login) {
		if (!valid_updater_login(*updater_login))
			return field_error(400, "invalid_field", "updater_login",
					"updater_login is 1 to 32 printable characters without spaces");
		if (!valid_command_file(*command_file))
			return field_error(400, "invalid_field", "command_file",
					"command_file is 1 to 15 characters ending in .CMD");
		data_set.updater_login = *updater_login;
		data_set.command_file = *command_file;
	}
	data_set.retail_updater = retail_updater.value_or(false);

	try {
		auto db_conn = pool_.acquire();
		if (*kind == DataSetKind::Base &&
				catalog::GameCatalog::load(*db_conn).by_slug(*game) == nullptr)
			return field_error(400, "invalid_field", "game", "game is no games slug");
		int64_t id = 0;
		switch (create_data_set(*db_conn, data_set, id)) {
			case DataSetOutcome::Ok:
				break;
			case DataSetOutcome::DuplicateLogin:
				return json_error(
						409, "duplicate_login", "another data set holds this updater login");
			case DataSetOutcome::DuplicateExpansion:
				return json_error(
						409, "duplicate_expansion", "the base has an expansion of this code");
			case DataSetOutcome::NoSuchParent:
				return json_error(409, "no_such_parent", "parent_id is no base data set");
			default:
				return json_error(500, "internal_error");
		}
		std::printf("[http] data set %lld '%s' (%s) created by %s\n", static_cast<long long>(id),
				loggable(data_set.display_name).c_str(), data_set_kind_name(*kind),
				loggable(data_set.created_by).c_str());
		crow::json::wvalue out;
		out["data_set"] = data_set_json(*db_conn, *find_data_set(*db_conn, id));
		return json_reply(201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/data-sets", e);
	}
}

// PATCH /api/admin/data-sets/<id> — any of {display_name, game (a base's), updater_login,
// command_file, retail_updater}, the shapes as the create's; 200 {"data_set"}; 400
// invalid_json, invalid_field; 404 not_found; 409 duplicate_login.
crow::response DataSetRoutes::patch(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const char *wrong = nullptr;
	const auto note = [&body, &wrong](const char *key) {
		if (body->wrong_type() && wrong == nullptr)
			wrong = key;
	};
	DataSetPatch patch;
	patch.display_name = body->text("display_name");
	note("display_name");
	patch.game = body->text("game");
	note("game");
	patch.updater_login = body->text("updater_login");
	note("updater_login");
	patch.command_file = body->text("command_file");
	note("command_file");
	patch.retail_updater = body->flag("retail_updater");
	note("retail_updater");
	if (wrong != nullptr)
		return field_error(400, "invalid_field", wrong, "the member has the wrong type");
	if (patch.display_name && !valid_display_name(*patch.display_name))
		return field_error(400, "invalid_field", "display_name",
				"display_name is 1 to 64 characters with no control characters");
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_data_set(*db_conn, id_of(id));
		if (!row)
			return no_such_data_set();
		if (patch.game) {
			if (row->kind != DataSetKind::Base)
				return field_error(
						400, "invalid_field", "game", "an expansion lists in its parent's lobby");
			if (catalog::GameCatalog::load(*db_conn).by_slug(*patch.game) == nullptr)
				return field_error(400, "invalid_field", "game", "game is no games slug");
		}
		const std::string login = patch.updater_login.value_or(row->updater_login);
		const std::string command_file = patch.command_file.value_or(row->command_file);
		if (login.empty() != command_file.empty())
			return field_error(400, "invalid_field",
					login.empty() ? "updater_login" : "command_file",
					"updater_login and command_file come together");
		if (!login.empty() && !valid_updater_login(login))
			return field_error(400, "invalid_field", "updater_login",
					"updater_login is 1 to 32 printable characters without spaces");
		if (!command_file.empty() && !valid_command_file(command_file))
			return field_error(400, "invalid_field", "command_file",
					"command_file is 1 to 15 characters ending in .CMD");
		switch (update_data_set(*db_conn, row->id, patch)) {
			case DataSetOutcome::Ok:
				break;
			case DataSetOutcome::NotFound:
				return no_such_data_set();
			case DataSetOutcome::DuplicateLogin:
				return json_error(
						409, "duplicate_login", "another data set holds this updater login");
			default:
				return json_error(500, "internal_error");
		}
		crow::json::wvalue out;
		out["data_set"] = data_set_json(*db_conn, *find_data_set(*db_conn, row->id));
		return json_reply(200, out);
	} catch (const db::Error &e) {
		return db_failure("PATCH /api/admin/data-sets/<id>", e);
	}
}

// DELETE /api/admin/data-sets/<id> — the data set and its trees: 404 not_found; 409 in_use
// while a live server runs it or an expansion has it as parent. Each tree's objects go
// first, here and now (its uploads in flight aborted, its prefix removed, the row marked
// pruned as it goes: the row delete cascades the tree rows, so the tree.prune job could
// never find them), then the row. 503 store_unavailable with no store or one that failed
// (the data set stays, the trees not yet removed still there). 204.
crow::response DataSetRoutes::remove(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_data_set(*db_conn, id_of(id));
		if (!row)
			return no_such_data_set();
		if (count_live_servers_of(*db_conn, row->id) > 0 ||
				!list_expansions_of(*db_conn, row->id).empty())
			return json_error(409, "in_use",
					"a live server runs this data set, or an expansion has it as parent");
		for (const DataSetTree &t : list_trees(*db_conn, row->id)) {
			if (t.state == TreeState::Pruned && !t.pruned_at.empty())
				continue;
			if (store_ == nullptr)
				return store_unavailable();
			std::string error;
			if (!store_->abort_uploads_under(tree_prefix(t.id), error) ||
					!store_->remove_prefix(tree_prefix(t.id), error)) {
				std::fprintf(stderr,
						"[http] DELETE /api/admin/data-sets/<id>: the object store kept "
						"tree %lld's objects: %s\n",
						static_cast<long long>(t.id), error.c_str());
				return json_error(
						503, "store_unavailable", "the object store did not remove the trees");
			}
			mark_tree_pruned(*db_conn, t.id);
		}
		switch (delete_data_set(*db_conn, row->id)) {
			case DataSetOutcome::Ok:
				break;
			case DataSetOutcome::NotFound:
				return no_such_data_set();
			case DataSetOutcome::InUse:
				return json_error(409, "in_use",
						"a live server runs this data set, or an expansion has it as parent");
			default:
				return json_error(500, "internal_error");
		}
		std::printf("[http] data set %lld '%s' deleted by %s\n", static_cast<long long>(row->id),
				loggable(row->display_name).c_str(), loggable(creator_of(caller)).c_str());
		return crow::response(204);
	} catch (const db::Error &e) {
		return db_failure("DELETE /api/admin/data-sets/<id>", e);
	}
}

// POST /api/admin/data-sets/<id>/trees — {"files": [{"name", "size"}]}: a new uploading tree
// and one multipart upload per file, each name canonicalised to the kind's spelling. 201
// {"tree", "uploads": [{"name", "key", "upload_id", "part_size"}]}; 400 invalid_json,
// invalid_field, invalid_files ({"missing", "unexpected"}: a required file not named, a name
// the kind lacks); 404 not_found; 409 upload_in_flight (a tree of it is uploading or
// finalizing); 503 store_unavailable (no store, or one that took no upload).
crow::response DataSetRoutes::begin_tree(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const crow::json::rvalue body = crow::json::load(req.body);
	if (!body || body.t() != crow::json::type::Object)
		return json_error(400, "invalid_json");
	if (!body.has("files") || body["files"].t() != crow::json::type::List)
		return field_error(400, "invalid_field", "files", "files is a list of {name, size}");
	std::vector<std::pair<std::string, int64_t>> files;
	for (const crow::json::rvalue &item : body["files"]) {
		std::string name;
		int64_t size = 0;
		if (item.t() != crow::json::type::Object || !member_string(item, "name", name) ||
				!member_whole(item, "size", size))
			return field_error(400, "invalid_field", "files", "files is a list of {name, size}");
		if (size <= 0 || size > kPartSize * kMaxParts)
			return field_error(
					400, "invalid_field", "files", "a file has bytes, at most 32 MiB x 10000");
		files.emplace_back(std::move(name), size);
	}
	if (files.empty())
		return field_error(400, "invalid_field", "files", "files names at least one file");
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_data_set(*db_conn, id_of(id));
		if (!row)
			return no_such_data_set();
		std::vector<std::string> names;
		std::vector<std::string> unexpected;
		std::set<std::string> seen;
		for (const auto &[name, size] : files) {
			const std::string canonical = canonical_tree_file(row->kind, row->expansion_code, name);
			if (canonical.empty()) {
				unexpected.push_back(name);
				continue;
			}
			if (!seen.insert(canonical).second)
				return field_error(400, "invalid_field", "files", "a file is named twice");
			names.push_back(canonical);
		}
		const std::vector<std::string> missing =
				missing_tree_files(row->kind, row->expansion_code, names);
		if (!missing.empty() || !unexpected.empty()) {
			crow::json::wvalue out;
			out["error"] = "invalid_files";
			out["message"] =
					"the files are not the kind's: a required one missing, or one it lacks";
			out["missing"] = missing;
			out["unexpected"] = unexpected;
			return json_reply(400, out);
		}
		if (store_ == nullptr)
			return store_unavailable();
		for (const DataSetTree &t : list_trees(*db_conn, row->id))
			if (t.state == TreeState::Uploading || t.state == TreeState::Finalizing)
				return json_error(409, "upload_in_flight",
						"a tree of this data set is uploading or finalizing; delete it or wait");
		const int64_t tree_id = create_tree(*db_conn, row->id, TreeSource::Baseline,
				TreeState::Uploading, std::nullopt, std::nullopt, creator_of(caller));
		std::vector<crow::json::wvalue> uploads;
		for (const std::string &name : names) {
			const std::string key = tree_file_key(tree_id, name);
			std::string upload_id, error;
			if (!store_->multipart_begin(key, upload_id, error)) {
				std::fprintf(stderr,
						"[http] POST /api/admin/data-sets/<id>/trees: the object store "
						"began no upload of %s: %s\n",
						key.c_str(), error.c_str());
				std::string ignored;
				store_->abort_uploads_under(tree_prefix(tree_id), ignored);
				mark_tree_pruned(*db_conn, tree_id);
				return json_error(503, "store_unavailable", "the object store took no upload");
			}
			crow::json::wvalue u;
			u["name"] = name;
			u["key"] = key;
			u["upload_id"] = upload_id;
			u["part_size"] = kPartSize;
			uploads.push_back(std::move(u));
		}
		std::printf("[http] tree %lld of data set %lld: %zu file(s) uploading, begun by %s\n",
				static_cast<long long>(tree_id), static_cast<long long>(row->id), names.size(),
				loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["tree"] = tree_json(*db_conn, *find_tree(*db_conn, tree_id));
		out["uploads"] = std::move(uploads);
		return json_reply(201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/data-sets/<id>/trees", e);
	}
}

// PUT /api/admin/data-sets/<id>/trees/<t>/files/<name>/parts/<n>?upload_id=<u> — the body
// (application/octet-stream, at most kPartSize) handed to the store as part n (1 to 10000)
// of the file's upload: 200 {"etag"} (what the completion reports for it); 400
// invalid_field (part_number, upload_id); 404 not_found (the data set, the tree, a name the
// kind lacks); 409 not_uploading; 413 too_large; 502 store_error; 503 store_unavailable.
// Crow caps no request body, so this check is the one after nginx's.
crow::response DataSetRoutes::tree_file_part(const crow::request &req, uint64_t id,
		uint64_t tree, const std::string &name, uint64_t part) {
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
		const auto row = find_data_set(*db_conn, id_of(id));
		if (!row)
			return no_such_data_set();
		const auto t = tree_of(*db_conn, row->id, id_of(tree));
		if (!t)
			return no_such_tree();
		const std::string canonical = canonical_tree_file(row->kind, row->expansion_code, name);
		if (canonical.empty())
			return json_error(404, "not_found", "this tree has no file of this name");
		if (t->state != TreeState::Uploading)
			return json_error(409, "not_uploading", "this tree is not uploading");
		if (store_ == nullptr)
			return store_unavailable();
		std::string etag, error;
		if (!store_->upload_part(tree_file_key(t->id, canonical), upload_id,
					static_cast<int>(part), req.body, etag, error))
			return store_failure(
					"PUT /api/admin/data-sets/<id>/trees/<t>/files/<name>/parts/<n>", error);
		crow::json::wvalue out;
		out["etag"] = etag;
		return no_store(json_reply(200, out));
	} catch (const db::Error &e) {
		return db_failure("PUT /api/admin/data-sets/<id>/trees/<t>/files/<name>/parts/<n>", e);
	}
}

// POST /api/admin/data-sets/<id>/trees/<t>/files/<name>/complete — {"upload_id", "parts":
// [{"part_number", "etag"}]}: the file's upload completed in the store, 200 {"completed":
// true}; 400 invalid_json, invalid_field; 404 not_found; 409 not_uploading; 502 store_error;
// 503 store_unavailable.
crow::response DataSetRoutes::tree_file_complete(
		const crow::request &req, uint64_t id, uint64_t tree, const std::string &name) {
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
				!member_string(item, "etag", part.etag) || number < 1 || number > kMaxParts ||
				part.etag.empty() || part.etag.size() > 256 || !printable_ascii(part.etag))
			return field_error(
					400, "invalid_field", "parts", "parts is a list of {part_number, etag}");
		part.number = static_cast<int>(number);
		parts.push_back(std::move(part));
	}
	if (parts.empty())
		return field_error(400, "invalid_field", "parts", "parts names at least one part");
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_data_set(*db_conn, id_of(id));
		if (!row)
			return no_such_data_set();
		const auto t = tree_of(*db_conn, row->id, id_of(tree));
		if (!t)
			return no_such_tree();
		if (t->state != TreeState::Uploading)
			return json_error(409, "not_uploading", "this tree is not uploading");
		const std::string canonical = canonical_tree_file(row->kind, row->expansion_code, name);
		if (canonical.empty())
			return field_error(400, "invalid_field", "name", "the kind takes no file of this name");
		if (store_ == nullptr)
			return store_unavailable();
		std::string error;
		if (!store_->multipart_complete(tree_file_key(t->id, canonical), upload_id, parts, error))
			return store_failure(
					"POST /api/admin/data-sets/<id>/trees/<t>/files/<name>/complete", error);
		std::printf("[http] tree %lld: %s uploaded (%zu part(s))\n", static_cast<long long>(t->id),
				canonical.c_str(), parts.size());
		crow::json::wvalue out;
		out["completed"] = true;
		return json_reply(200, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/data-sets/<id>/trees/<t>/files/<name>/complete", e);
	}
}

// POST /api/admin/data-sets/<id>/trees/<t>/finalize — the tree finalizing and its
// tree.finalize job, in one transaction: 202 {"tree"}; 404 not_found; 409 not_uploading.
crow::response DataSetRoutes::tree_finalize(const crow::request &req, uint64_t id, uint64_t tree) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_data_set(*db_conn, id_of(id));
		if (!row)
			return no_such_data_set();
		const auto t = tree_of(*db_conn, row->id, id_of(tree));
		if (!t)
			return no_such_tree();
		{
			db::Transaction tx(*db_conn);
			if (!set_tree_state(*db_conn, t->id, TreeState::Uploading, TreeState::Finalizing))
				return json_error(409, "not_uploading", "this tree is not uploading");
			enqueue_tree_finalize(*db_conn, t->id);
			tx.commit();
		}
		std::printf("[http] tree %lld of data set %lld finalizing, asked by %s\n",
				static_cast<long long>(t->id), static_cast<long long>(row->id),
				loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["tree"] = tree_json(*db_conn, *find_tree(*db_conn, t->id));
		return json_reply(202, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/data-sets/<id>/trees/<t>/finalize", e);
	}
}

// DELETE /api/admin/data-sets/<id>/trees/<t> — an uploading tree: its uploads aborted, its
// objects removed, the row pruned, 204; a ready or failed one: the row marked pruned and a
// tree.prune job in the same transaction, 202 {"tree"}; a pruned one 204 (nothing to do).
// 404 not_found; 409 current, baseline, pinned (with "servers", how many live ones) for a
// tree a data set or a live server still needs, in_progress for one the worker is
// finalizing or building; 502 store_error; 503 store_unavailable.
crow::response DataSetRoutes::tree_remove(const crow::request &req, uint64_t id, uint64_t tree) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_data_set(*db_conn, id_of(id));
		if (!row)
			return no_such_data_set();
		const auto t = tree_of(*db_conn, row->id, id_of(tree));
		if (!t)
			return no_such_tree();
		switch (t->state) {
			case TreeState::Pruned:
				return crow::response(204);
			case TreeState::Finalizing:
			case TreeState::Building:
				return json_error(409, "in_progress",
						"the worker is finalizing or building this tree; wait for it");
			case TreeState::Uploading: {
				if (store_ == nullptr)
					return store_unavailable();
				std::string error;
				if (!store_->abort_uploads_under(tree_prefix(t->id), error) ||
						!store_->remove_prefix(tree_prefix(t->id), error))
					return store_failure("DELETE /api/admin/data-sets/<id>/trees/<t>", error);
				mark_tree_pruned(*db_conn, t->id);
				std::printf("[http] tree %lld of data set %lld: the upload abandoned by %s\n",
						static_cast<long long>(t->id), static_cast<long long>(row->id),
						loggable(creator_of(caller)).c_str());
				return crow::response(204);
			}
			case TreeState::Ready:
			case TreeState::Failed:
				break;
		}
		int pinned_by = 0;
		const std::string hold = tree_hold(*db_conn, t->id, pinned_by);
		if (hold == "current")
			return json_error(409, "current", "this tree is what new starts take");
		if (hold == "baseline")
			return json_error(409, "baseline", "this tree is what a release applies to");
		if (hold == "pinned") {
			crow::json::wvalue body;
			body["error"] = "pinned";
			body["message"] = "live servers run on this tree";
			body["servers"] = pinned_by;
			return json_reply(409, body);
		}
		{
			db::Transaction tx(*db_conn);
			request_tree_prune(*db_conn, t->id);
			enqueue_tree_prune(*db_conn);
			tx.commit();
		}
		std::printf("[http] tree %lld of data set %lld pruned by %s\n",
				static_cast<long long>(t->id), static_cast<long long>(row->id),
				loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["tree"] = tree_json(*db_conn, *find_tree(*db_conn, t->id));
		return json_reply(202, out);
	} catch (const db::Error &e) {
		return db_failure("DELETE /api/admin/data-sets/<id>/trees/<t>", e);
	}
}

} // namespace opennova::novaworld_server::hosted
