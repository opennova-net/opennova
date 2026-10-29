#include "console_routes.h"
#include "hosted/model/console_queue.h"
#include "hosted/model/console_configuration.h"
#include "http_json.h"
#include <base/io/cp1252.h>
#include <net/admin/admin_protocol.h>
#include <algorithm>

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

crow::response ConsoleRoutes::settings(const Caller &c) {
	std::optional<HostedServer> server;
	OfferedProduct product;
	{
		auto lease = pool_.acquire();
		server = find_server(*lease, c.server_id);
		if (!server)
			return json_error(404, "not_found");
		product = pinned_product(*lease, *server);
	}
	std::optional<crow::response> refusal;
	const auto replies = run(c, *console_command(ConsoleVerb::GameSettings), refusal);
	if (!replies)
		return std::move(*refusal);
	std::vector<crow::json::wvalue> rows;
	for (const auto &row : admin_parse_game_settings(replies->front())) {
		crow::json::wvalue e;
		e["key"] = cp1252_to_utf8(row.first);
		e["value"] = cp1252_to_utf8(row.second);
		rows.push_back(std::move(e));
	}
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["settings"] = std::move(rows);
	out["saved"] = json(settings_document(server->settings));
	out["settings_revision"] = server->settings_revision;
	out["schema"] = json(gamecfg::server_settings_schema(title_config_table(product.title), true));
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

crow::response ConsoleRoutes::configuration_command(
		const Caller &c, const crow::request &req, bool edit) {
	io::JsonValue body;
	std::string error;
	if (!io::json_parse(req.body, body, error) || !body.is_object())
		return json_error(400, "invalid_json");
	AdminCommandOptions options;
	options.actor = c.actor;
	std::string line;
	if (edit) {
		const auto *revision = body.get("settings_revision"), *patch = body.get("patch");
		if (!revision || !io::json_exact_whole_in(*revision, 1, io::kJsonSafeWholeMax) || !patch ||
				!patch->is_object())
			return json_error(
					400, "invalid_field", "settings_revision and a settings patch are required.");
		for (const char *key : { "product_id", "expansion_id", "rotation" })
			if (patch->get(key))
				return field_error(400, "invalid_field", key,
						"Use the launcher or rotation controls to change this field.");
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
		options.expected_revision = static_cast<int64_t>(revision->number);
		options.settings_patch = io::json_write(*patch);
		line = "configuration edit";
	} else {
		const auto *command = body.get("command");
		if (!command || !command->is_string() || !utf8_to_cp1252(command->string, line) ||
				line.empty() ||
				line.size() > ADMIN_SERVER_PACKET_MAX_BYTES - ADMIN_PACKET_HEADER_BYTES - 1 ||
				std::any_of(line.begin(), line.end(), [](unsigned char c) {
					return c < 32 ||
							(c == 127 || c == 129 || c == 141 || c == 143 || c == 144 || c == 157);
				}))
			return json_error(400, "invalid_command",
					"Enter one command, at most 1015 CP1252 bytes, without control characters.");
		// Refuse text that could not be represented safely in the saved cfg.
		// Unknown retail keys still reach the server and return its own usage.
		const auto patch = console_settings_patch(line, {});
		if (!patch.object.empty()) {
			auto lease = pool_.acquire();
			const auto server = find_server(*lease, c.server_id);
			if (!server)
				return json_error(404, "not_found");
			const auto product = pinned_product(*lease, *server);
			auto block = gamecfg::defaults({}, 0, 0, title_config_table(product.title));
			auto flat = io::JsonValue::make_object();
			for (const auto &m : patch.object) {
				if (m.key == "options")
					for (const auto &o : m.value.object)
						flat.set(o.key, o.value);
				else
					flat.set(m.key, m.value);
			}
			std::string field;
			if (!gamecfg::apply_server_settings(block, flat, true, {}, field, error))
				return field_error(400, "invalid_field", field.c_str(), error);
		}
	}
	return command_reply(
			c.server_id, run_console_command(pool_, c.server_id, line, options, kCommandBudget));
}

void ConsoleRoutes::register_configuration(WebApp &app) {
	CROW_ROUTE(app, "/api/me/servers/<uint>/settings")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) -> crow::response {
				auto access = access_.require_user(req);
				if (access.refusal)
					return std::move(*access.refusal);
				auto c = owner_caller(*access.user, id, false, false);
				if (c.refusal)
					return std::move(*c.refusal);
				return settings(c);
			});
	const auto handle = [this](const crow::request &req, uint64_t id, bool admin, bool edit,
								bool weapons) -> crow::response {
		auto access = admin ? access_.require_admin(req) : access_.require_user(req);
		if (access.refusal)
			return std::move(*access.refusal);
		auto c = admin ? admin_caller(access, id, !weapons)
					   : owner_caller(*access.user, id, !weapons, !weapons);
		if (c.refusal)
			return std::move(*c.refusal);
		return weapons ? weapon_settings(c) : configuration_command(c, req, edit);
	};
	CROW_ROUTE(app, "/api/me/servers/<uint>/settings")
			.methods("PATCH"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, false, true, false);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/settings")
			.methods("PATCH"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, true, true, false);
			});
	CROW_ROUTE(app, "/api/me/servers/<uint>/command")
			.methods("POST"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, false, false, false);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/command")
			.methods("POST"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, true, false, false);
			});
	CROW_ROUTE(app, "/api/me/servers/<uint>/weapons")
			.methods("GET"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, false, false, true);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/weapons")
			.methods("GET"_method)([handle](const crow::request &req, uint64_t id) {
				return handle(req, id, true, false, true);
			});
}
} //namespace opennova::novaworld_server::hosted
