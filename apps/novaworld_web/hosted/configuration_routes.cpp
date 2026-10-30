#include "console_routes.h"
#include "hosted/model/console_queue.h"
#include "http_json.h"
#include <base/io/cp1252.h>

namespace opennova::novaworld_server::hosted {
namespace {
crow::json::wvalue json(const io::JsonValue &v) {
	return crow::json::wvalue(crow::json::load(io::json_write(v)));
}

// A running server keeps its pinned catalog even after a newer release is published.
OfferedProduct pinned_product(db::Database &db, const HostedServer &server) {
	OfferedProduct p;
	p.id = server.settings.product_id;
	if (const auto product = find_product(db, p.id)) {
		p.title = product->title;
		p.lobby = product->lobby;
		p.display_name = product->display_name;
		const auto tree_id = server.base_tree_id ? server.base_tree_id : product->current_tree_id;
		if (tree_id)
			if (const auto tree = find_tree(db, *tree_id)) {
				p.missions = tree->missions;
				p.weapons = tree->weapons;
			}
	}
	if (server.settings.expansion_id) {
		OfferedExpansion e;
		e.id = *server.settings.expansion_id;
		if (const auto product = find_product(db, e.id)) {
			const auto tree_id =
					server.expansion_tree_id ? server.expansion_tree_id : product->current_tree_id;
			if (tree_id)
				if (const auto tree = find_tree(db, *tree_id)) {
					e.missions = tree->missions;
					e.weapons = tree->weapons;
				}
		}
		p.expansions.push_back(std::move(e));
	}
	return p;
}

crow::response command_reply(int64_t id, const AdminCommandResult &result) {
	crow::json::wvalue out;
	out["server_id"] = id;
	out["outcome"] = admin_command_outcome_name(result.outcome);
	out["sent"] = result.sent;
	out["delivery_unknown"] = result.delivery_unknown;
	out["configuration_saved"] = result.configuration_saved;
	out["settings_revision"] = result.settings_revision;
	std::vector<std::string> replies;
	for (const auto &reply : result.replies)
		replies.push_back(cp1252_to_utf8(reply));
	out["replies"] = std::move(replies);
	int status = 200;
	if (result.outcome != AdminCommandOutcome::Answered) {
		status = result.outcome == AdminCommandOutcome::Refused	  ? 409
				: result.outcome == AdminCommandOutcome::TimedOut ? 504
																  : 503;
		out["error"] = result.delivery_unknown ? "delivery_unknown"
											   : admin_command_outcome_name(result.outcome);
		out["message"] = result.delivery_unknown
				? "The result is uncertain. Reload saved settings and live state before retrying."
				: result.outcome == AdminCommandOutcome::Refused
				? "Configuration changed; reload before applying this edit."
				: "The server could not complete the command.";
	}
	return no_store(json_reply(status, out));
}
} //namespace

// The saved configuration, read from the database alone: no admin line, so it reads while the
// server boots, restarts or has no link (the panel shows the saved settings; the live
// GAMESETTINGS reply it once fetched was never shown, and carried the passwords).
crow::response ConsoleRoutes::settings(db::Database &db, const HostedServer &server) {
	const OfferedProduct product = pinned_product(db, server);
	crow::json::wvalue out;
	out["server_id"] = server.id;
	out["saved"] = json(settings_document(server.settings));
	out["settings_revision"] = server.settings_revision;
	out["schema"] = json(hosted_settings_schema(title_config_table(product.title)));
	const auto &weapons = !product.expansions.empty() && !product.expansions.front().weapons.empty()
			? product.expansions.front().weapons
			: product.weapons;
	out["weapons"] = crow::json::wvalue(crow::json::load(encode_tree_weapons(weapons)));
	return no_store(json_reply(200, out));
}

crow::response ConsoleRoutes::weapon_settings(const Caller &c) {
	AdminCommandOptions options;
	options.actor = c.actor;
	options.record = false;
	return command_reply(c.server_id,
			run_console_command(pool_, c.server_id, "WEAPON LIST", options, kCommandBudget));
}

crow::response ConsoleRoutes::configuration_edit(const Caller &c, const crow::request &req) {
	io::JsonValue body;
	std::string error;
	if (!io::json_parse(req.body, body, error) || !body.is_object())
		return json_error(400, "invalid_json");
	AdminCommandOptions options;
	options.actor = c.actor;
	const auto *revision = body.get("settings_revision"), *patch = body.get("patch");
	if (!revision || !io::json_exact_whole_in(*revision, 1, io::kJsonSafeWholeMax) || !patch ||
			!patch->is_object())
		return json_error(
				400, "invalid_field", "settings_revision and a settings patch are required.");
	for (const char *key : { "product_id", "expansion_id", "rotation" })
		if (patch->get(key))
			return field_error(400, "invalid_field", key,
					"Use the launcher or rotation controls to change this field.");
	{
		auto lease = pool_.acquire();
		const auto server = find_server(*lease, c.server_id);
		if (!server)
			return json_error(404, "not_found");
		if (server->settings_revision != static_cast<int64_t>(revision->number))
			return json_error(
					409, "settings_changed", "Reload settings before applying this edit.");
		HostedSettings desired = server->settings;
		std::string field;
		if (!read_settings_document(*patch, desired, field, error))
			return field_error(400, "invalid_field", field.c_str(), error);
		// Check edited fields independently of a previously cleared live rotation.
		const auto product = pinned_product(*lease, *server);
		if (desired.missions.empty() && !product.missions.empty())
			desired.missions.push_back(product.missions.front().file);
		if (const auto why = check_settings(desired, { product }))
			return field_error(400, "invalid_field", why->field.c_str(), why->reason);
	}
	options.expected_revision = static_cast<int64_t>(revision->number);
	options.settings_patch = io::json_write(*patch);
	return command_reply(c.server_id,
			run_console_command(pool_, c.server_id, "configuration edit", options, kCommandBudget));
}

void ConsoleRoutes::register_configuration(WebApp &app) {
	// GET .../settings: the owner's server (404 for another account's) or any for an admin,
	// in any state; an owner's read draws one token from the account's read bucket.
	CROW_ROUTE(app, "/api/me/servers/<uint>/settings")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) -> crow::response {
				auto access = access_.require_user(req);
				if (access.refusal)
					return std::move(*access.refusal);
				if (const int64_t wait = owner_reads_.take(
							"console|" + std::to_string(access.user->id), RateLimiter::Clock::now()))
					return too_many_requests(wait);
				try {
					auto lease = pool_.acquire();
					const auto server = owned_server(*lease, path_id(id), access.user->id);
					if (!server)
						return json_error(
								404, "not_found", "this account has no hosted server with this id");
					return settings(*lease, *server);
				} catch (const db::Error &e) {
					std::fprintf(stderr, "[http] hosted settings read failed: %s\n", e.what());
					return json_error(500, "db_error");
				}
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/settings")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) -> crow::response {
				auto access = access_.require_admin(req);
				if (access.refusal)
					return std::move(*access.refusal);
				try {
					auto lease = pool_.acquire();
					const auto server = find_server(*lease, path_id(id));
					if (!server)
						return json_error(404, "not_found", "no hosted server has this id");
					return settings(*lease, *server);
				} catch (const db::Error &e) {
					std::fprintf(stderr, "[http] hosted settings read failed: %s\n", e.what());
					return json_error(500, "db_error");
				}
			});
	const auto handle = [this](const crow::request &req, uint64_t id, bool admin,
								bool weapons) -> crow::response {
		auto access = admin ? access_.require_admin(req) : access_.require_user(req);
		if (access.refusal)
			return std::move(*access.refusal);
		auto c = admin ? admin_caller(access, id, !weapons)
					   : owner_caller(*access.user, id, !weapons, !weapons);
		if (c.refusal)
			return std::move(*c.refusal);
		return weapons ? weapon_settings(c) : configuration_edit(c, req);
	};
	CROW_ROUTE(app, "/api/me/servers/<uint>/settings")
			.methods("PATCH"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, false, false);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/settings")
			.methods("PATCH"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, true, false);
			});
	CROW_ROUTE(app, "/api/me/servers/<uint>/weapons")
			.methods("GET"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, false, true);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/weapons")
			.methods("GET"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, true, true);
			});
}
} //namespace opennova::novaworld_server::hosted
