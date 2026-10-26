#include "data_set_routes.h"

#include "core/loggable.h"
#include "core/object_store.h"
#include "data_sets/content_repository.h"
#include "data_sets/data_set_jobs.h"
#include "data_sets/data_set_repository.h"
#include "hosted/model/hosted_repository.h"
#include "http_json.h"
#include "program/catalog_repository.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

// The titles a base data set may speak (the gameprofile codes opennova-serve's /game takes).
constexpr std::array<const char *, 2> kTitles = { "jo", "dfx2" };

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

crow::response no_such_data_set() {
	return json_error(404, "not_found", "no data set has this id");
}

crow::response duplicate_key() {
	return json_error(409, "duplicate_key", "another game's project has this key");
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

bool valid_visibility(std::string_view visibility) {
	return content::visibility_from_name(visibility).has_value();
}

void id_or_null(crow::json::wvalue &out, const char *key, std::optional<int64_t> id) {
	if (id)
		out[key] = *id;
	else
		out[key] = nullptr;
}

void text_or_null(crow::json::wvalue &out, const char *key, const std::string &text) {
	if (text.empty())
		out[key] = nullptr;
	else
		out[key] = text;
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
	out["retail_updater"] = d.retail;
	text_or_null(out, "key", d.key);
	text_or_null(out, "visibility", d.visibility);
	out["head"] = d.head;
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

} // namespace

crow::json::wvalue tree_json(db::Database &db, const DataSetTree &t) {
	crow::json::wvalue out;
	out["id"] = t.id;
	out["state"] = tree_state_name(t.state);
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
}

// GET /api/admin/data-sets — {"store" ({"kind": "s3" or "local"}, null with none), "titles"
// (what a base may speak), "games" (the lobbies, each {slug, display_name}), "data_sets"}:
// every game (bases, expansions, then the runtime), each {id, display_name, kind, parent_id,
// expansion_code, title, game, updater_login, command_file, retail_updater, key (its project's,
// or null), visibility ("public", "private", or null with no project), head (its project's newest
// revision, 0 before the first push), current_tree ({id, version, ready_at} or null),
// published_release ({id, name, sha256} or null), servers_live, servers_outdated, trees (its
// trees newest first, pruned ones left out, each as tree_json)}.
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
// updater_login?, command_file?, retail_updater?, key?, visibility?}: a new game, a base (title
// one of the titles, game a games slug) or expansion (parent_id a base, expansion_code
// [a-z0-9]{1,8}); the updater's product both or neither (a login of 1..32 printable characters,
// a command file of 1..15 ending .CMD); its project's key (1..16 of A-Z, 0-9, _) and visibility
// (public or private) both or neither (a game with a key has a project). 201 {"data_set"}; 400
// invalid_json, missing_field, invalid_field (with the field); 409 duplicate_login,
// duplicate_key, duplicate_expansion, no_such_parent.
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
	const auto key = body->text("key");
	note("key");
	const auto visibility = body->text("visibility");
	note("visibility");
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
	if (!kind || *kind == DataSetKind::Update)
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
	data_set.retail = retail_updater.value_or(false);
	if (key.has_value() != visibility.has_value())
		return field_error(400, "invalid_field", key ? "visibility" : "key",
				"key and visibility come together: a game with a key has a project");
	if (key) {
		if (!content::valid_key(*key))
			return field_error(400, "invalid_field", "key", "key is 1 to 16 of A-Z, 0-9 and _");
		if (!valid_visibility(*visibility))
			return field_error(
					400, "invalid_field", "visibility", "visibility is public or private");
		data_set.key = *key;
		data_set.visibility = *visibility;
	}

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
			case DataSetOutcome::DuplicateKey:
				return duplicate_key();
			case DataSetOutcome::DuplicateExpansion:
				return json_error(
						409, "duplicate_expansion", "the base has an expansion of this code");
			case DataSetOutcome::NoSuchParent:
				return json_error(409, "no_such_parent", "parent_id is no base data set");
			default:
				return json_error(500, "internal_error");
		}
		std::printf("[http] data set %lld '%s' (%s%s%s) created by %s\n",
				static_cast<long long>(id), loggable(data_set.display_name).c_str(),
				data_set_kind_name(*kind), data_set.key.empty() ? "" : ", project ",
				loggable(data_set.key).c_str(), loggable(data_set.created_by).c_str());
		crow::json::wvalue out;
		out["data_set"] = data_set_json(*db_conn, *find_data_set(*db_conn, id));
		return json_reply(201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/data-sets", e);
	}
}

// PATCH /api/admin/data-sets/<id> — any of {display_name, game (a base's), updater_login,
// command_file, retail_updater, key, visibility}, the shapes as the create's; after it the key
// and visibility are both set or neither (the runtime takes neither: its releases arrive from
// CI). A new key renames the project: its objects in the store move from content/<old>/ to
// content/<new>/ first. 200 {"data_set"}; 400 invalid_json, invalid_field; 404 not_found; 409
// duplicate_login, duplicate_key; 502 store_error (the objects did not move; nothing changed).
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
	patch.retail = body->flag("retail_updater");
	note("retail_updater");
	patch.key = body->text("key");
	note("key");
	patch.visibility = body->text("visibility");
	note("visibility");
	if (wrong != nullptr)
		return field_error(400, "invalid_field", wrong, "the member has the wrong type");
	if (patch.display_name && !valid_display_name(*patch.display_name))
		return field_error(400, "invalid_field", "display_name",
				"display_name is 1 to 64 characters with no control characters");
	if (patch.key && !content::valid_key(*patch.key))
		return field_error(400, "invalid_field", "key", "key is 1 to 16 of A-Z, 0-9 and _");
	if (patch.visibility && !valid_visibility(*patch.visibility))
		return field_error(400, "invalid_field", "visibility", "visibility is public or private");
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_data_set(*db_conn, path_id(id));
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
		if ((patch.key || patch.visibility) && row->kind == DataSetKind::Update)
			return field_error(400, "invalid_field", patch.key ? "key" : "visibility",
					"the runtime has no project: its releases arrive from CI");
		const std::string key = patch.key.value_or(row->key);
		if (key.empty() != patch.visibility.value_or(row->visibility).empty())
			return field_error(400, "invalid_field", key.empty() ? "key" : "visibility",
					"key and visibility come together: a game with a key has a project");
		// A rename: the project's objects copied under the new key first, so a refusal leaves the
		// old ones where its rows name them; the old ones go once the row names the new key.
		const bool rename = !row->key.empty() && key != row->key;
		if (rename) {
			if (content::find_project(*db_conn, key))
				return duplicate_key();
			std::string error;
			if (store_ != nullptr &&
					!store_->copy_prefix(
							content::project_prefix(row->key), content::project_prefix(key), error)) {
				std::fprintf(stderr,
						"[http] PATCH /api/admin/data-sets/<id>: the object store did not copy "
						"%s to %s: %s\n",
						content::project_prefix(row->key).c_str(),
						content::project_prefix(key).c_str(), error.c_str());
				return json_error(502, "store_error", "the object store did not move the project");
			}
		}
		const DataSetOutcome outcome = update_data_set(*db_conn, row->id, patch);
		if (outcome != DataSetOutcome::Ok && rename && store_ != nullptr) {
			std::string error;
			store_->remove_prefix(content::project_prefix(key), error);
		}
		switch (outcome) {
			case DataSetOutcome::Ok:
				break;
			case DataSetOutcome::NotFound:
				return no_such_data_set();
			case DataSetOutcome::DuplicateLogin:
				return json_error(
						409, "duplicate_login", "another data set holds this updater login");
			case DataSetOutcome::DuplicateKey:
				return duplicate_key();
			default:
				return json_error(500, "internal_error");
		}
		if (rename) {
			std::string error;
			if (store_ != nullptr &&
					!store_->remove_prefix(content::project_prefix(row->key), error))
				std::fprintf(stderr,
						"[http] PATCH /api/admin/data-sets/<id>: %s stays in the store: %s\n",
						content::project_prefix(row->key).c_str(), error.c_str());
			std::printf("[http] project %s of data set %lld renamed %s by %s\n",
					loggable(row->key).c_str(), static_cast<long long>(row->id),
					loggable(key).c_str(), loggable(creator_of(caller)).c_str());
		}
		crow::json::wvalue out;
		out["data_set"] = data_set_json(*db_conn, *find_data_set(*db_conn, row->id));
		return json_reply(200, out);
	} catch (const db::Error &e) {
		return db_failure("PATCH /api/admin/data-sets/<id>", e);
	}
}

// DELETE /api/admin/data-sets/<id> — the game: its trees, its project (its history and files)
// and the row. 404 not_found; 409 in_use while a live server runs it, has_expansion while an
// expansion builds on it. Each tree's objects go first, here and now (its prefix removed, the row
// marked pruned as it goes: the row delete cascades the tree rows, so the tree.prune job could
// never find them), then the row, then the project's objects (content/<key>/; ones the store
// keeps are logged). 503 store_unavailable with no store or one that failed while a tree has
// objects (the data set stays, the trees not yet removed still there). 204.
crow::response DataSetRoutes::remove(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const auto in_use = [] {
		return json_error(409, "in_use", "a live server runs this game");
	};
	const auto has_expansion = [] {
		return json_error(409, "has_expansion", "an expansion builds on this game");
	};
	std::optional<DataSetRow> row;
	try {
		auto db_conn = pool_.acquire();
		row = find_data_set(*db_conn, path_id(id));
		if (!row)
			return no_such_data_set();
		if (count_live_servers_of(*db_conn, row->id) > 0)
			return in_use();
		if (!list_expansions_of(*db_conn, row->id).empty())
			return has_expansion();
		for (const DataSetTree &t : list_trees(*db_conn, row->id)) {
			if (t.state == TreeState::Pruned && !t.pruned_at.empty())
				continue;
			std::string error;
			if (store_ == nullptr || !store_->remove_prefix(tree_prefix(t.id), error)) {
				std::fprintf(stderr,
						"[http] DELETE /api/admin/data-sets/<id>: the object store kept "
						"tree %lld's objects: %s\n",
						static_cast<long long>(t.id), store_ ? error.c_str() : "no store");
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
				return in_use();
			case DataSetOutcome::HasExpansion:
				return has_expansion();
			default:
				return json_error(500, "internal_error");
		}
	} catch (const db::Error &e) {
		return db_failure("DELETE /api/admin/data-sets/<id>", e);
	}
	std::string error;
	if (!row->key.empty() && store_ != nullptr &&
			!store_->remove_prefix(content::project_prefix(row->key), error))
		std::fprintf(stderr, "[http] DELETE /api/admin/data-sets/<id>: %s stays in the store: %s\n",
				content::project_prefix(row->key).c_str(), error.c_str());
	std::printf("[http] data set %lld '%s' deleted by %s\n", static_cast<long long>(row->id),
			loggable(row->display_name).c_str(), loggable(creator_of(caller)).c_str());
	return crow::response(204);
}

} // namespace opennova::novaworld_server::hosted
