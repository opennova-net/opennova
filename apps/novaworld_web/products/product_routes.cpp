#include "product_routes.h"

#include "core/loggable.h"
#include "core/object_store.h"
#include "core/text_util.h"
#include "hosted/model/hosted_repository.h"
#include "http_json.h"
#include "products/product_jobs.h"
#include "products/product_repository.h"
#include "products/project_repository.h"
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

crow::response no_such_product() {
	return json_error(404, "not_found", "no product has this id");
}

crow::response duplicate_key() {
	return json_error(409, "duplicate_key", "another product has this key");
}

crow::response duplicate_command_file() {
	return json_error(409, "duplicate_command_file", "another product holds this command file");
}

bool valid_updater_login(std::string_view login) {
	return !login.empty() && login.size() <= ProductRoutes::kUpdaterLoginMax &&
			printable_ascii(login);
}

bool valid_command_file(std::string_view name) {
	return name.size() > 4 && name.size() <= ProductRoutes::kCommandFileMax &&
			printable_ascii(name) && strutil::ends_with_icase(name, ".CMD");
}

// A product as the admin's JSON, with its live trees (pruned ones left out).
crow::json::wvalue product_json(db::Database &db, const ProductRow &d) {
	crow::json::wvalue out;
	out["id"] = d.id;
	out["key"] = d.key;
	out["display_name"] = d.display_name;
	out["kind"] = product_kind_name(d.kind);
	id_or_null(out, "parent_id", d.parent_id);
	out["expansion_code"] = d.expansion_code;
	out["title"] = d.title;
	out["lobby"] = d.lobby;
	out["lobby_locked"] = d.lobby_locked;
	out["updater_login"] = d.updater_login;
	out["command_file"] = d.command_file;
	out["wizard_login"] = d.wizard_login;
	out["retail"] = d.retail;
	text_or_null(out, "visibility", d.visibility);
	out["head"] = d.head;
	id_or_null(out, "owner_id", d.owner_id);
	if (d.owner_id)
		out["owner"] = d.owner;
	else
		out["owner"] = nullptr;
	out["download"] = d.download;
	out["description"] = d.description;
	std::optional<ProductTree> current;
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
	for (const ProductTree &t : list_trees(db, d.id))
		if (t.state != TreeState::Pruned)
			trees.push_back(tree_json(db, t));
	out["trees"] = std::move(trees);
	return out;
}

} // namespace

bool valid_display_name(std::string_view name) {
	const std::string_view trimmed = strutil::trim_view(name);
	return !trimmed.empty() && trimmed.size() == name.size() &&
			strutil::utf8_length(name) <= ProductRoutes::kDisplayNameMax &&
			!has_control_byte(name) && !has_c1_control(name);
}

bool valid_title(std::string_view title) {
	return std::find(kTitles.begin(), kTitles.end(), title) != kTitles.end();
}

bool valid_expansion_code(std::string_view code) {
	return !code.empty() && code.size() <= ProductRoutes::kExpansionCodeMax &&
			std::all_of(code.begin(), code.end(),
					[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool valid_visibility(std::string_view visibility) {
	return projects::visibility_from_name(visibility).has_value();
}

crow::response product_in_use() {
	return json_error(409, "in_use", "a live server runs this game");
}

crow::response product_has_expansion() {
	return json_error(409, "has_expansion", "an expansion builds on this game");
}

crow::response duplicate_expansion() {
	return field_error(409, "duplicate_expansion", "expansion_code",
			"another mod already uses this short name");
}

crow::json::wvalue tree_json(db::Database &db, const ProductTree &t) {
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

void ProductRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/products").methods("GET"_method)([this](const crow::request &req) {
		return list(req);
	});
	CROW_ROUTE(app, "/api/admin/products")
			.methods("POST"_method)([this](const crow::request &req) { return create(req); });
	CROW_ROUTE(app, "/api/admin/products/<uint>")
			.methods("PATCH"_method)(
					[this](const crow::request &req, uint64_t id) { return patch(req, id); });
	CROW_ROUTE(app, "/api/admin/products/<uint>")
			.methods("DELETE"_method)(
					[this](const crow::request &req, uint64_t id) { return remove(req, id); });
}

// GET /api/admin/products — {"store" ({"kind": "s3" or "local"}, null with none), "titles"
// (what a game may speak), "games" (the lobbies, each {slug, display_name}), "products"}:
// every product (games, expansions, then apps), each {id, key, display_name, kind, parent_id,
// expansion_code, title, lobby ("" for none), updater_login, command_file, wizard_login, retail,
// visibility ("public", "private", or null for an app, which has no project), head (its project's
// newest revision, 0 before the first push), owner_id and owner (the account whose own project it
// is, its id and username; null for the service's own), download (its file's name on the
// downloads host, "" for none), description, current_tree ({id, version, ready_at} or null),
// published_release ({id, name, sha256} or null), servers_live, servers_outdated, trees (its
// trees newest first, pruned ones left out, each as tree_json)}.
crow::response ProductRoutes::list(const crow::request &req) {
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
		for (const catalog::GameRow &game : catalog::available_games(*db_conn,
					 caller.user ? std::optional<int64_t>(caller.user->id) : std::nullopt)) {
			crow::json::wvalue g;
			g["slug"] = game.slug;
			g["display_name"] = game.display_name;
			g["title"] = game.title;
			games.push_back(std::move(g));
		}
		out["games"] = std::move(games);
		std::vector<crow::json::wvalue> products;
		for (const ProductRow &d : list_products(*db_conn))
			products.push_back(product_json(*db_conn, d));
		out["products"] = std::move(products);
	} catch (const db::Error &e) {
		return db_failure("GET /api/admin/products", e);
	}
	return no_store(json_reply(200, out));
}

// POST /api/admin/products — {display_name, kind, parent_id?, expansion_code?, title?, lobby?,
// updater_login?, command_file?, retail?, key?, visibility?}: a new product, a game (title
// one of the titles, lobby a games slug) or expansion (parent_id a game, expansion_code
// [a-z0-9]{1,8}, unique across every product without case); the updater's product both or
// neither (a login of 1..32 printable characters, a command file of 1..15 ending .CMD, each
// unique without case); its key (1..16 of A-Z, 0-9, _) and its project's visibility (public or
// private), both required. 201 {"product"}; 400 invalid_json, missing_field, invalid_field (with
// the field); 409 duplicate_login, duplicate_command_file, duplicate_key, duplicate_expansion
// {field: "expansion_code"}, no_such_parent.
crow::response ProductRoutes::create(const crow::request &req) {
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
	const auto lobby = body->text("lobby");
	note("lobby");
	const auto updater_login = body->text("updater_login");
	note("updater_login");
	const auto command_file = body->text("command_file");
	note("command_file");
	const auto retail = body->flag("retail");
	note("retail");
	const auto key = body->text("key");
	note("key");
	const auto visibility = body->text("visibility");
	note("visibility");
	if (wrong != nullptr)
		return field_error(400, "invalid_field", wrong, "the member has the wrong type");
	if (!display_name)
		return field_error(400, "missing_field", "display_name", "display_name is required");
	if (!kind_name)
		return field_error(400, "missing_field", "kind", "kind is game or expansion");
	if (!valid_display_name(*display_name))
		return field_error(400, "invalid_field", "display_name",
				"display_name is 1 to 64 characters with no control characters");
	const auto kind = product_kind_from_name(*kind_name);
	if (!kind || *kind == ProductKind::App)
		return field_error(400, "invalid_field", "kind", "kind is game or expansion");

	NewProduct product;
	product.display_name = *display_name;
	product.kind = *kind;
	product.created_by = creator_of(caller);
	if (*kind == ProductKind::Game) {
		if (parent_id || expansion_code)
			return field_error(400, "invalid_field", parent_id ? "parent_id" : "expansion_code",
					"a game has no parent and no expansion code");
		if (!title)
			return field_error(400, "missing_field", "title", "a game speaks a title");
		if (!valid_title(*title))
			return field_error(400, "invalid_field", "title", "title is jo or dfx2");
		if (!lobby)
			return field_error(
					400, "missing_field", "lobby", "a game lists in a lobby (a games slug)");
		product.title = *title;
		product.lobby = *lobby;
	} else {
		if (title || lobby)
			return field_error(400, "invalid_field", title ? "title" : "lobby",
					"an expansion speaks its parent's title and lobby");
		if (!parent_id)
			return field_error(400, "missing_field", "parent_id", "an expansion has a game for parent");
		if (!expansion_code)
			return field_error(400, "missing_field", "expansion_code", "an expansion has a code");
		if (!valid_expansion_code(*expansion_code))
			return field_error(400, "invalid_field", "expansion_code",
					"expansion_code is 1 to 8 of a-z and 0-9");
		product.parent_id = *parent_id;
		product.expansion_code = *expansion_code;
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
		product.updater_login = *updater_login;
		product.command_file = *command_file;
	}
	product.retail = retail.value_or(false);
	if (!key)
		return field_error(400, "missing_field", "key", "every product has a key");
	if (!visibility)
		return field_error(
				400, "missing_field", "visibility", "a game's project is public or private");
	if (!projects::valid_key(*key))
		return field_error(400, "invalid_field", "key", "key is 1 to 16 of A-Z, 0-9 and _");
	if (!valid_visibility(*visibility))
		return field_error(400, "invalid_field", "visibility", "visibility is public or private");
	product.key = *key;
	product.visibility = *visibility;

	try {
		auto db_conn = pool_.acquire();
		if (*kind == ProductKind::Game &&
				catalog::GameCatalog::load(*db_conn).by_slug(*lobby) == nullptr)
			return field_error(400, "invalid_field", "lobby", "lobby is no games slug");
		int64_t id = 0;
		switch (create_product(*db_conn, product, id,
				caller.user ? std::optional<int64_t>(caller.user->id) : std::nullopt)) {
			case ProductOutcome::Ok:
				break;
			case ProductOutcome::DuplicateLogin:
				return json_error(
						409, "duplicate_login", "another product holds this updater login");
			case ProductOutcome::DuplicateCommandFile:
				return duplicate_command_file();
			case ProductOutcome::DuplicateKey:
				return duplicate_key();
			case ProductOutcome::DuplicateExpansion:
				return duplicate_expansion();
			case ProductOutcome::NoSuchParent:
				return json_error(409, "no_such_parent", "parent_id is no game");
			case ProductOutcome::InvalidLobby:
				return field_error(400, "invalid_field", "lobby",
						"choose an available lobby for this game's protocol");
			default:
				return json_error(500, "internal_error");
		}
		std::printf("[http] product %lld '%s' (%s%s%s) created by %s\n",
				static_cast<long long>(id), loggable(product.display_name).c_str(),
				product_kind_name(*kind), ", project ", loggable(product.key).c_str(),
				loggable(product.created_by).c_str());
		crow::json::wvalue out;
		out["product"] = product_json(*db_conn, *find_product(*db_conn, id));
		return json_reply(201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/products", e);
	}
}

// PATCH /api/admin/products/<id> — any of {display_name, lobby (a game's), updater_login,
// command_file, retail, key, visibility (a game's or an expansion's: an app has no project)}, the
// shapes as the create's; any product's, an account's own too (never made retail). A new key
// renames the product in its row alone: the store names a project's objects by the product's id
// (content/<id>/, project_repository.h), so none moves and a push begun under the old key lands
// where the renamed project's readers look; a project file naming the old key no longer reaches
// it. 200 {"product"}; 400 invalid_json, invalid_field; 404 not_found; 409 duplicate_login,
// duplicate_command_file, duplicate_key.
crow::response ProductRoutes::patch(const crow::request &req, uint64_t id) {
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
	ProductPatch patch;
	patch.display_name = body->text("display_name");
	note("display_name");
	patch.lobby = body->text("lobby");
	note("lobby");
	patch.updater_login = body->text("updater_login");
	note("updater_login");
	patch.command_file = body->text("command_file");
	note("command_file");
	patch.retail = body->flag("retail");
	note("retail");
	patch.key = body->text("key");
	note("key");
	patch.visibility = body->text("visibility");
	note("visibility");
	if (wrong != nullptr)
		return field_error(400, "invalid_field", wrong, "the member has the wrong type");
	if (patch.display_name && !valid_display_name(*patch.display_name))
		return field_error(400, "invalid_field", "display_name",
				"display_name is 1 to 64 characters with no control characters");
	if (patch.key && !projects::valid_key(*patch.key))
		return field_error(400, "invalid_field", "key", "key is 1 to 16 of A-Z, 0-9 and _");
	if (patch.visibility && !valid_visibility(*patch.visibility))
		return field_error(400, "invalid_field", "visibility", "visibility is public or private");
	try {
		auto db_conn = pool_.acquire();
		const auto row = find_product(*db_conn, path_id(id));
		if (!row)
			return no_such_product();
		if (patch.lobby) {
			if (row->kind != ProductKind::Game)
				return field_error(
						400, "invalid_field", "lobby", "an expansion lists in its parent's lobby");
			if (catalog::GameCatalog::load(*db_conn).by_slug(*patch.lobby) == nullptr)
				return field_error(400, "invalid_field", "lobby", "lobby is no games slug");
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
		if (patch.visibility && row->kind == ProductKind::App)
			return field_error(400, "invalid_field", "visibility", "an app has no project");
		if (patch.retail.value_or(false) && row->owner_id)
			return field_error(
					400, "invalid_field", "retail", "an account's own project is no retail game");
		const std::string key = patch.key.value_or(row->key);
		const bool rename = key != row->key;
		switch (update_product(*db_conn, row->id, patch,
				caller.user ? std::optional<int64_t>(caller.user->id) : std::nullopt)) {
			case ProductOutcome::Ok:
				break;
			case ProductOutcome::NotFound:
				return no_such_product();
			case ProductOutcome::InvalidLobby:
				return field_error(400, "invalid_field", "lobby",
						"choose an available lobby for this game's protocol");
			case ProductOutcome::LobbyLocked:
				return json_error(
						409, "lobby_locked", "the lobby is fixed once a release build starts");
			case ProductOutcome::DuplicateLogin:
				return json_error(
						409, "duplicate_login", "another product holds this updater login");
			case ProductOutcome::DuplicateCommandFile:
				return duplicate_command_file();
			case ProductOutcome::DuplicateKey:
				return duplicate_key();
			default:
				return json_error(500, "internal_error");
		}
		if (rename)
			std::printf("[http] product %s (%lld) renamed %s by %s\n",
					loggable(row->key).c_str(), static_cast<long long>(row->id),
					loggable(key).c_str(), loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["product"] = product_json(*db_conn, *find_product(*db_conn, row->id));
		return json_reply(200, out);
	} catch (const db::Error &e) {
		return db_failure("PATCH /api/admin/products/<id>", e);
	}
}

// DELETE /api/admin/products/<id> — the game, any product's (an account's own too): its row, its
// trees' rows and its project's history (they cascade), and, with a store, its objects removed by
// the data program's product.purge (product_jobs.h delete_product_purged), enqueued in the
// delete's transaction: its trees' prefixes and its project's files (content/<id>/), so the route
// answers at once however many files a retail game's project holds. 404 not_found; 409 in_use
// while a live server runs it, has_expansion while an expansion builds on it. 204.
crow::response ProductRoutes::remove(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::optional<ProductRow> row;
	try {
		auto db_conn = pool_.acquire();
		row = find_product(*db_conn, path_id(id));
		if (!row)
			return no_such_product();
		// With no store there are no objects: nothing to purge.
		switch (delete_product_purged(*db_conn, row->id, store_ != nullptr)) {
			case ProductOutcome::Ok:
				break;
			case ProductOutcome::NotFound:
				return no_such_product();
			case ProductOutcome::InUse:
				return product_in_use();
			case ProductOutcome::HasExpansion:
				return product_has_expansion();
			default:
				return json_error(500, "internal_error");
		}
	} catch (const db::Error &e) {
		return db_failure("DELETE /api/admin/products/<id>", e);
	}
	std::printf("[http] product %lld '%s' deleted by %s%s\n", static_cast<long long>(row->id),
			loggable(row->display_name).c_str(), loggable(creator_of(caller)).c_str(),
			store_ != nullptr ? "; its objects' purge queued" : "");
	return crow::response(204);
}

} // namespace opennova::novaworld_server::hosted
