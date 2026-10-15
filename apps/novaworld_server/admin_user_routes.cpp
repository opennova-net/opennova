#include "admin_user_routes.h"

#include "billing/billing_repository.h"
#include "billing/billing_routes.h"
#include "billing/entitlement.h"
#include "hosted/hosted_repository.h"
#include "hosted/hosted_routes.h"
#include "http_json.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

// The entitlement a users list row and a user page carry: {max_servers,
// source, tier (null for none)}, and on the user page its comp grant, own
// machines and whether a server may start.
crow::json::wvalue entitlement_json(const billing::Entitlement &e, bool full) {
	crow::json::wvalue out;
	out["max_servers"] = e.max_servers;
	out["source"] = e.source;
	if (e.tier > 0)
		out["tier"] = e.tier;
	else
		out["tier"] = nullptr;
	if (full) {
		out["comp_servers"] = e.comp_servers;
		out["own_machines"] = e.own_machines;
		out["can_start"] = e.can_start;
	}
	return out;
}

} // namespace

crow::json::wvalue user_to_json(db::Database &db, const UserRecord &u) {
	crow::json::wvalue e;
	e["id"]       = u.id;
	e["username"] = u.username;
	e["pcid"]     = u.pcid;
	e["nwh"]      = u.nwh;
	e["nwhandle"] = u.nwhandle;
	e["account_status"] = u.account_status;
	e["role"]     = u.role;
	e["comp_servers"] = u.comp_servers;
	e["max_servers"] = billing::entitlement_of(db, u.id, billing::unix_now()).max_servers;
	return e;
}

void AdminUserRoutes::register_routes(WebApp &app, ForgetAccount forget) {
	forget_ = std::move(forget);

	// GET /api/admin/users — list every player (no password_hash), each with
	// its hosting entitlement ({max_servers, source, tier}), its live hosted
	// servers (live_servers) and its support tier's Stripe status
	// (subscription_status, null for none).
	CROW_ROUTE(app, "/api/admin/users").methods("GET"_method)(
	    [this](const crow::request &req) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto db_conn = pool_.acquire();
		const int64_t now = billing::unix_now();
		std::vector<crow::json::wvalue> arr;
		for (const auto &u : list_users(*db_conn)) {
			crow::json::wvalue e = user_to_json(*db_conn, u);
			const billing::Entitlement entitlement = billing::entitlement_of(*db_conn, u.id, now);
			e["entitlement"] = entitlement_json(entitlement, false);
			e["live_servers"] = hosted::count_live_of(*db_conn, u.id);
			if (entitlement.status.empty())
				e["subscription_status"] = nullptr;
			else
				e["subscription_status"] = entitlement.status;
			arr.push_back(std::move(e));
		}
		crow::json::wvalue out;
		out["users"] = std::move(arr);
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// POST /api/admin/users — create. Body JSON:
	//   {username, password, pcid, nwhandle, nwh? (default "1")}
	CROW_ROUTE(app, "/api/admin/users").methods("POST"_method)(
	    [this](const crow::request &req) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		CreateUserParams p;
		p.username = body->text_or("username", "");
		p.password = body->text_or("password", "");
		p.pcid     = body->text_or("pcid",     "");
		p.nwh      = body->text_or("nwh",      "1");
		p.nwhandle = body->text_or("nwhandle", "");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		auto db_conn = pool_.acquire();
		auto result = create_user(*db_conn, p);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"]   = result.error_code;
			if (!result.error_message.empty())
				out["message"] = result.error_message;
			crow::response res(result.error_code == "db_error" ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto created = get_user_by_id(*db_conn, result.id);
		out["user"] = created ? user_to_json(*db_conn, *created) : crow::json::wvalue{};
		crow::response res(201);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// GET /api/admin/users/<id> — one player: {"user", "entitlement"
	// ({max_servers, source, tier, comp_servers, own_machines, can_start}),
	// "servers" (its hosted servers: the live ones by id, then its newest 10
	// ended), "billing" ({livemode, stripe_customer_id (null for none),
	// subscriptions: [{stripe_subscription_id, item, tier, quantity, status,
	// entitled, current_period_end, cancel_at_period_end}]})}. 404 not_found
	// for no player (a hosted server's service account included).
	CROW_ROUTE(app, "/api/admin/users/<int>").methods("GET"_method)(
	    [this](const crow::request &req, int64_t id) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		try {
			auto db_conn = pool_.acquire();
			db::ReadSnapshot snapshot(*db_conn);
			const auto user = get_user_by_id(*db_conn, id);
			if (!user || user->kind != "player") return json_error(404, "not_found", "no player account has this id");
			const int64_t now = billing::unix_now();
			crow::json::wvalue out;
			out["user"] = user_to_json(*db_conn, *user);
			out["entitlement"] = entitlement_json(billing::entitlement_of(*db_conn, id, now), true);
			std::vector<crow::json::wvalue> servers;
			for (const hosted::HostedServer &s : hosted::list_servers_of(*db_conn, id))
				servers.push_back(hosted::server_json(*db_conn, s, false));
			out["servers"] = std::move(servers);
			crow::json::wvalue billing_ids;
			const auto customer = billing::customer_of(*db_conn, id);
			billing_ids["livemode"] = customer && customer->livemode;
			if (customer)
				billing_ids["stripe_customer_id"] = customer->stripe_customer_id;
			else
				billing_ids["stripe_customer_id"] = nullptr;
			std::vector<crow::json::wvalue> subscriptions;
			for (const billing::SubscriptionRow &row : billing::subscriptions_of(*db_conn, id))
				subscriptions.push_back(billing::admin_subscription_json(row, now));
			billing_ids["subscriptions"] = std::move(subscriptions);
			out["billing"] = std::move(billing_ids);
			crow::response res = json_reply(200, out);
			res.set_header("Cache-Control", "no-store");
			return res;
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] GET /api/admin/users/%lld failed: %s\n", static_cast<long long>(id), e.what());
			return json_error(500, "db_error");
		}
	});

	// DELETE /api/admin/users/<id> — 204; 404 not_found; 409
	// active_subscription while a subscription of the account still grants
	// (billing/entitlement.h): Stripe would bill an account that is gone, so it
	// is canceled first.
	CROW_ROUTE(app, "/api/admin/users/<int>").methods("DELETE"_method)(
	    [this](const crow::request &req, int id) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto db_conn = pool_.acquire();
		if (billing::has_granting_subscription(*db_conn, id, billing::unix_now()))
			return json_error(409, "active_subscription",
					"a support tier of this account still grants; cancel it in Stripe first");
		auto result = delete_user(*db_conn, id);
		if (result.ok) forget_(id);
		crow::response res(result.ok ? 204 :
		                   result.error_code == "not_found" ? 404 : 500);
		if (!result.ok) {
			crow::json::wvalue err;
			err["error"]   = result.error_code;
			if (!result.error_message.empty())
				err["message"] = result.error_message;
			res.body = err.dump();
			res.set_header("Content-Type", "application/json");
		}
		return res;
	});

	// PUT /api/admin/users/<id> — partial update. Any field omitted is
	// left untouched. password (plaintext) triggers a fresh bcrypt hash; role
	// takes "player" or "admin" (anything else is a 400 invalid_field).
	CROW_ROUTE(app, "/api/admin/users/<int>").methods("PUT"_method)(
	    [this](const crow::request &req, int id) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		UpdateUserParams p;
		p.username           = body->text("username");
		p.password_plaintext = body->text("password");
		p.pcid               = body->text("pcid");
		p.nwh                = body->text("nwh");
		p.nwhandle           = body->text("nwhandle");
		p.account_status     = body->text("account_status");
		p.role               = body->text("role");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		auto db_conn = pool_.acquire();
		auto result = update_user(*db_conn, id, p);
		if (result.ok && update_revokes_sessions(p)) forget_(id);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"]   = result.error_code;
			if (!result.error_message.empty())
				out["message"] = result.error_message;
			crow::response res(
				result.error_code == "not_found" ? 404 :
				result.error_code == "db_error"  ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto updated = get_user_by_id(*db_conn, id);
		out["user"] = updated ? user_to_json(*db_conn, *updated) : crow::json::wvalue{};
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});

	// PUT /api/admin/users/<id>/comp-servers — {"comp_servers": 0..16}: an
	// admin's grant of hosted servers to the account (players.comp_servers; its
	// hosting entitlement is the larger of the grant and its support tier's
	// servers), answered with the account. 400 invalid_json, invalid_field (no
	// whole number, or outside 0..16) or missing_field, 404 for an unknown id and
	// for a hosted server's service account. Lowering the entitlement below what
	// the account runs stops servers (the supervisor's next pass;
	// hosted/supervisor.h).
	CROW_ROUTE(app, "/api/admin/users/<int>/comp-servers").methods("PUT"_method)(
	    [this](const crow::request &req, int64_t id) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		const auto comp_servers = body->integer("comp_servers");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		if (!comp_servers) return json_error(400, "missing_field", "comp_servers (0 to 16) is required");
		if (*comp_servers < 0 || *comp_servers > billing::kMaxCompServers)
			return json_error(400, "invalid_field", "comp_servers must be a whole number from 0 to 16");
		auto db_conn = pool_.acquire();
		const auto result = set_comp_servers(*db_conn, id, static_cast<int>(*comp_servers));
		if (!result.ok) {
			return json_error(result.error_code == "not_found" ? 404 : 500,
			                  result.error_code.c_str(), result.error_message);
		}
		const auto updated = get_user_by_id(*db_conn, id);
		crow::json::wvalue out;
		out["user"] = updated ? user_to_json(*db_conn, *updated) : crow::json::wvalue{};
		return json_reply(200, out);
	});

	// PUT /api/admin/users/<id>/game-access — per-game expansion/access gate.
	CROW_ROUTE(app, "/api/admin/users/<int>/game-access").methods("PUT"_method)(
	    [this](const crow::request &req, int id) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		UpdateGameAccessParams p;
		p.game_slug = body->text_or("game_slug", "");
		p.status    = body->text_or("status",    "active");
		p.exp_bits  = body->text_or("exp_bits",  "");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		auto db_conn = pool_.acquire();
		auto result = update_game_access(*db_conn, id, p);
		crow::json::wvalue out;
		if (!result.ok) {
			out["error"] = result.error_code;
			if (!result.error_message.empty())
				out["message"] = result.error_message;
			crow::response res(result.error_code == "not_found" ? 404 :
			                   result.error_code == "db_error"  ? 500 : 400);
			res.body = out.dump();
			res.set_header("Content-Type", "application/json");
			return res;
		}
		auto access = get_game_access(*db_conn, id, p.game_slug);
		if (access) {
			out["game_slug"] = access->game_slug;
			out["status"] = access->status;
			out["exp_bits"] = access->exp_bits;
		}
		crow::response res(200);
		res.body = out.dump();
		res.set_header("Content-Type", "application/json");
		return res;
	});
}

} // namespace opennova::novaworld_server
