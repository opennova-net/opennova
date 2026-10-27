#include "settings_routes.h"

#include "billing/billing_routes.h"
#include "core/clock.h"
#include "hosted/model/service_settings.h"
#include "hosted/model/sold_capacity.h"
#include "http_json.h"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace opennova::novaworld_server {

namespace {

crow::json::wvalue fleet_json(const hosted::PlacementPolicy &policy) {
	crow::json::wvalue out;
	out["max_shared"] = policy.max_shared;
	out["shared_slots"] = policy.shared_slots;
	out["max_own"] = policy.max_own;
	out["max_addresses"] = policy.max_addresses;
	return out;
}

void number_or_null(crow::json::wvalue &out, const char *key, const std::optional<int> &value) {
	if (value)
		out[key] = *value;
	else
		out[key] = nullptr;
}

crow::json::wvalue settings_json(db::Database &db, const hosted::PlacementPolicy &boot) {
	const hosted::ServiceSettings settings = hosted::read_service_settings(db);
	const hosted::PlacementPolicy live = hosted::live_placement(boot, settings);
	crow::json::wvalue out;
	out["subscriptions_enabled"] = settings.subscriptions_enabled;
	out["fleet"] = fleet_json(live);
	out["boot"] = fleet_json(boot);
	crow::json::wvalue set;
	number_or_null(set, "max_shared", settings.max_shared);
	number_or_null(set, "shared_slots", settings.shared_slots);
	number_or_null(set, "max_own", settings.max_own);
	number_or_null(set, "max_addresses", settings.max_addresses);
	out["set"] = std::move(set);
	out["addresses_available"] = boot.max_addresses > 0;
	out["capacity"] = billing::sold_capacity_json(db, hosted::sellable_of(live), wall_now_s());
	return out;
}

// A maximum: a whole number, or null for the boot value. False when it is neither.
bool maximum(JsonBody &body, const char *key, std::optional<int> &out) {
	if (body.null(key)) {
		out.reset();
		return true;
	}
	const auto value = body.integer(key);
	if (!value || *value < 0 || *value > 1000)
		return false;
	out = static_cast<int>(*value);
	return true;
}

} // namespace

void SettingsRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/settings").methods("GET"_method)([this](const crow::request &req) {
		return get(req);
	});
	CROW_ROUTE(app, "/api/admin/settings").methods("PUT"_method)([this](const crow::request &req) {
		return put(req);
	});
}

// GET /api/admin/settings — the header's shape. no-store.
crow::response SettingsRoutes::get(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	try {
		auto db_conn = pool_.acquire();
		return no_store(json_reply(200, settings_json(*db_conn, fleet_)));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/settings failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// PUT /api/admin/settings — the header's body; answered with the GET shape.
crow::response SettingsRoutes::put(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	for (const char *key :
			{ "subscriptions_enabled", "max_shared", "shared_slots", "max_own", "max_addresses" })
		if (!body->has(key))
			return json_error(400, "missing_field",
					"subscriptions_enabled, max_shared, shared_slots, max_own and max_addresses "
					"are required");
	hosted::ServiceSettings settings;
	const auto subscriptions = body->flag("subscriptions_enabled");
	if (!subscriptions)
		return json_error(400, "invalid_field", "subscriptions_enabled is true or false");
	settings.subscriptions_enabled = *subscriptions;
	if (!maximum(*body, "max_shared", settings.max_shared) ||
			!maximum(*body, "shared_slots", settings.shared_slots) ||
			!maximum(*body, "max_own", settings.max_own) ||
			!maximum(*body, "max_addresses", settings.max_addresses))
		return json_error(400, "invalid_field",
				"each maximum is a whole number, or null for the deployment's");
	if (const std::string why = hosted::check_service_settings(settings, fleet_); !why.empty())
		return json_error(400, "invalid_field", why);
	try {
		auto db_conn = pool_.acquire();
		hosted::write_service_settings(*db_conn, settings);
		const hosted::PlacementPolicy live = hosted::live_placement(fleet_, settings);
		std::printf("[http] an admin set the service settings: subscriptions %s, %d shared "
					"machine(s) of %d slots, "
					"%d dedicated machine(s), %d static address(es)\n",
				settings.subscriptions_enabled ? "on" : "off", live.max_shared, live.shared_slots,
				live.max_own, live.max_addresses);
		return no_store(json_reply(200, settings_json(*db_conn, fleet_)));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] PUT /api/admin/settings failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

} // namespace opennova::novaworld_server
