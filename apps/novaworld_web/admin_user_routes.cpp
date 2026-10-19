#include "admin_user_routes.h"

#include "billing/billing_repository.h"
#include "billing/billing_routes.h"
#include "billing/entitlement.h"
#include "billing/tiers.h"
#include "hosted/address_routes.h"
#include "hosted/hosted_routes.h"
#include "hosted/model/address_repository.h"
#include "hosted/model/hosted_repository.h"
#include "http_json.h"

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

// The entitlement a users list row and a user page carry: {max_servers (its
// shared servers and its dedicated machines together), shared_servers,
// own_machines (its dedicated machines), comp_own_machines (the admin's grant
// of them), source (its shared servers'), tier (null for none), comp_servers
// (the admin's grant of shared servers: with a tier too, the pages show
// both)}, and on the user page the servers its tier alone grants
// (tier_servers, 0 for none) and whether a server may start.
crow::json::wvalue entitlement_json(const billing::Entitlement &e, bool full) {
	crow::json::wvalue out;
	out["max_servers"] = e.max_servers;
	out["shared_servers"] = e.shared_servers;
	out["own_machines"] = e.own_machines;
	out["comp_own_machines"] = e.comp_own_machines;
	out["source"] = e.source;
	if (e.tier > 0)
		out["tier"] = e.tier;
	else
		out["tier"] = nullptr;
	out["comp_servers"] = e.comp_servers;
	if (full) {
		out["tier_servers"] = billing::kTierServers[e.tier];
		out["can_start"] = e.can_start;
	}
	return out;
}

// A user row as JSON with its entitlement already read: its comp grants and
// the servers it may run.
crow::json::wvalue user_json(const UserRecord &u, const billing::Entitlement &entitlement) {
	crow::json::wvalue e;
	e["id"]       = u.id;
	e["username"] = u.username;
	e["pcid"]     = u.pcid;
	e["nwh"]      = u.nwh;
	e["nwhandle"] = u.nwhandle;
	e["account_status"] = u.account_status;
	e["role"]     = u.role;
	e["comp_servers"] = entitlement.comp_servers;
	e["comp_own_machines"] = entitlement.comp_own_machines;
	e["max_servers"] = entitlement.max_servers;
	// Read-only on every admin route: the account's own address, null for none.
	if (u.email.empty())
		e["email"] = nullptr;
	else
		e["email"] = u.email;
	return e;
}

} // namespace

crow::json::wvalue user_to_json(db::Database &db, const UserRecord &u) {
	return user_json(u, billing::entitlement_of(db, u.id, billing::unix_now()));
}

void AdminUserRoutes::register_routes(WebApp &app, ForgetAccount forget) {
	forget_ = std::move(forget);

	// GET /api/admin/users — list every player (no password_hash), each with
	// its hosting entitlement ({max_servers, shared_servers, own_machines,
	// comp_own_machines, source, tier, comp_servers}), its
	// live hosted servers (live_servers) and its support tier's Stripe status
	// (subscription_status, null for none). Five reads whatever the count:
	// the accounts, every subscription, every comp grant, every comp address
	// and every live server.
	CROW_ROUTE(app, "/api/admin/users").methods("GET"_method)(
	    [this](const crow::request &req) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const int64_t now = billing::unix_now();
		const auto subscriptions = billing::subscriptions_by_account(*db_conn);
		const auto grants = billing::comp_grants_by_account(*db_conn);
		const auto comp_addresses = billing::comp_addresses_by_account(*db_conn);
		static const std::vector<billing::SubscriptionRow> kNoSubscriptions;
		std::map<int64_t, int> live;
		for (const hosted::HostedServer &s : hosted::list_live(*db_conn)) {
			if (s.owner_user_id) ++live[*s.owner_user_id];
		}
		std::vector<crow::json::wvalue> arr;
		for (const auto &u : list_users(*db_conn)) {
			billing::AccountFacts facts;
			facts.kind = u.kind;
			facts.account_status = u.account_status;
			if (const auto granted = grants.find(u.id); granted != grants.end()) {
				facts.comp_servers = granted->second.servers;
				facts.comp_own_machines = granted->second.own_machines;
			}
			if (const auto addresses = comp_addresses.find(u.id); addresses != comp_addresses.end())
				facts.comp_addresses = addresses->second;
			const auto rows = subscriptions.find(u.id);
			const billing::Entitlement entitlement =
					billing::entitlement_from(facts, rows == subscriptions.end() ? kNoSubscriptions : rows->second, now);
			crow::json::wvalue e = user_json(u, entitlement);
			e["entitlement"] = entitlement_json(entitlement, false);
			const auto owned = live.find(u.id);
			e["live_servers"] = owned == live.end() ? 0 : owned->second;
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
	//   {username, password, pcid, nwhandle, nwh? (default "1"), email? (none)}
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
		p.email    = body->text_or("email",    "");
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
	// ({max_servers, shared_servers, source, tier, comp_servers,
	// comp_own_machines, tier_servers, own_machines, can_start}), "capacity" (the service's sold
	// capacity against what the fleet sells, billing/billing_routes.h
	// sold_capacity_json),
	// "servers" (its hosted servers: the live ones by id, then its newest 10
	// ended), "addresses" (its static addresses, the held ones first:
	// hosted/address_routes.h address_json, with the admin's fields), "billing" ({livemode, stripe_customer_id (null for none),
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
			out["capacity"] = billing::sold_capacity_json(*db_conn, sellable_, now);
			std::vector<crow::json::wvalue> servers;
			for (const hosted::HostedServer &s : hosted::list_servers_of(*db_conn, id))
				servers.push_back(hosted::server_json(*db_conn, s, false, address_zone_));
			out["servers"] = std::move(servers);
			std::vector<crow::json::wvalue> addresses;
			for (const hosted::HostedAddress &a : hosted::addresses_of(*db_conn, id))
				addresses.push_back(hosted::address_json(*db_conn, a, address_zone_, true));
			out["addresses"] = std::move(addresses);
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
	// active_subscription while a subscription of the account still grants (a
	// support tier or an add-on; billing/entitlement.h), and 409 checkout_open
	// while a checkout of the account holds its reservation
	// (billing/billing_repository.h "A checkout's hold": its session may still
	// be paid, or be paid and not synced yet): Stripe would bill an account that
	// is gone, so its subscriptions end first. The checks and the delete hold
	// the hosting-start lock a checkout's judgement and reservation take
	// (database.h kHostedStartLock), so no checkout reserves for an account
	// being deleted.
	CROW_ROUTE(app, "/api/admin/users/<int>").methods("DELETE"_method)(
	    [this](const crow::request &req, int64_t id) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		MutationResult result;
		try {
			auto db_conn = pool_.acquire();
			{
				db::Transaction tx(*db_conn);
				db_conn->exec("SELECT pg_advisory_xact_lock($1);", { db::BindValue(db::kHostedStartLock) });
				if (billing::has_granting_subscription(*db_conn, id, billing::unix_now()))
					return json_error(409, "active_subscription",
							"a subscription of this account still grants (a support tier or an add-on); cancel it in "
							"Stripe first");
				if (!billing::live_reservations_of(*db_conn, id).empty())
					return json_error(409, "checkout_open",
							"a checkout of this account may still be paid; try again once it ends");
				result = delete_user(*db_conn, id);
				if (result.ok) tx.commit();
			}
			if (result.ok) forget_(*db_conn, id);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] DELETE /api/admin/users/%lld failed: %s\n", static_cast<long long>(id), e.what());
			return json_error(500, "db_error");
		}
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
	    [this](const crow::request &req, int64_t id) {
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
		if (result.ok && update_revokes_sessions(p)) forget_(*db_conn, id);
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

	// PUT /api/admin/users/<id>/comp — {"servers": 0..16, "own_machines":
	// 0..16}: an admin's grants to the account outside any subscription
	// (comp_grants; billing/entitlement.h: its shared servers are the larger of
	// the grant of servers and its support tier's, and each dedicated machine
	// granted is one more server on a machine of its own, added to the paid
	// ones), both set at once, answered with the
	// account, its entitlement and the sold capacity (sold_capacity_json: a grant
	// counts as sold, and may take it past what the fleet sells, which the
	// page warns of; logged). 400 invalid_json, invalid_field (no whole
	// number, or outside 0..16) or missing_field, 404 for an unknown id and for
	// a hosted server's service account. Lowering the shared servers below what
	// the account runs on shared machines stops its newest there; lowering its
	// dedicated machines below its servers on them moves the newest of those to
	// a shared machine while the account has a shared server free and the
	// fleet room, and stops the rest (the supervisor's next pass;
	// hosted/supervisor.h).
	CROW_ROUTE(app, "/api/admin/users/<int>/comp").methods("PUT"_method)(
	    [this](const crow::request &req, int64_t id) {
		auto caller = access_.require_admin(req);
		if (caller.refusal) return std::move(*caller.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		const auto servers = body->integer("servers");
		const auto own_machines = body->integer("own_machines");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		if (!servers || !own_machines)
			return json_error(400, "missing_field",
					"servers (shared servers) and own_machines (dedicated machines), 0 to 16 each, are required");
		if (*servers < 0 || *servers > billing::kMaxCompServers || *own_machines < 0 ||
				*own_machines > billing::kMaxCompOwnMachines)
			return json_error(400, "invalid_field", "servers and own_machines are whole numbers from 0 to 16");
		try {
			auto db_conn = pool_.acquire();
			billing::CompGrants grants;
			grants.servers = static_cast<int>(*servers);
			grants.own_machines = static_cast<int>(*own_machines);
			if (!billing::set_comp_grants(*db_conn, id, grants))
				return json_error(404, "not_found", "no player account has this id");
			std::printf("[http] an admin set account %lld's comp grants: %d shared server(s), %d dedicated machine(s)\n",
					static_cast<long long>(id), grants.servers, grants.own_machines);
			const auto updated = get_user_by_id(*db_conn, id);
			crow::json::wvalue out;
			out["user"] = updated ? user_to_json(*db_conn, *updated) : crow::json::wvalue{};
			out["entitlement"] = entitlement_json(billing::entitlement_of(*db_conn, id, billing::unix_now()), true);
			out["capacity"] = billing::sold_capacity_json(*db_conn, sellable_, billing::unix_now());
			const hosted::Capacity sold = hosted::sold_now(*db_conn, billing::unix_now());
			if (hosted::sold_exceeds_capacity(sold, sellable_))
				std::fprintf(stderr,
						"[http] WARN account %lld's comp grants take the sold capacity past what the fleet sells "
						"(shared %lld of %lld, own machines %lld of %lld, addresses %lld of %lld)\n",
						static_cast<long long>(id), static_cast<long long>(sold.shared),
						static_cast<long long>(sellable_.shared), static_cast<long long>(sold.own),
						static_cast<long long>(sellable_.own), static_cast<long long>(sold.addresses),
						static_cast<long long>(sellable_.addresses));
			return json_reply(200, out);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] PUT /api/admin/users/%lld/comp failed: %s\n", static_cast<long long>(id),
					e.what());
			return json_error(500, "db_error");
		}
	});

	// PUT /api/admin/users/<id>/game-access — per-game expansion/access gate.
	CROW_ROUTE(app, "/api/admin/users/<int>/game-access").methods("PUT"_method)(
	    [this](const crow::request &req, int64_t id) {
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
