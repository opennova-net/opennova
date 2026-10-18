#include "address_routes.h"

#include "auth.h"
#include "billing/entitlement.h"
#include "hosted/address_jobs.h"
#include "hosted/address_names.h"
#include "hosted/hosted_repository.h"
#include "hosted/slots.h"
#include "http_json.h"
#include "loggable.h"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

crow::response disabled() {
	return json_error(503, "addresses_disabled", "this NovaWorld service sells no static address");
}

// A naming's refusal as its reply, or nullopt for Created.
std::optional<crow::response> refusal_of(const NameResult &r) {
	switch (r.outcome) {
		case NameOutcome::Created: return std::nullopt;
		case NameOutcome::InvalidName: return json_error(400, "invalid_name", r.why);
		case NameOutcome::NameTaken: return json_error(409, "name_taken", "another static address has this name");
		case NameOutcome::NoAddress:
			return json_error(409, "no_address", "this account has no static address left to name; buy one first");
		case NameOutcome::AddressLimit:
			return json_error(409, "address_limit", "this account has a static address for every own machine it has");
		case NameOutcome::NoSuchAccount: return json_error(404, "not_found", "no active player account has this id");
	}
	return json_error(500, "internal_error");
}

void put_time(crow::json::wvalue &out, const char *key, const std::string &at) { text_or_null(out, key, at); }

} // namespace

crow::json::wvalue address_json(db::Database &db, const HostedAddress &a, const std::string &zone, bool admin) {
	crow::json::wvalue out;
	out["id"] = a.id;
	out["name"] = a.name;
	out["hostname"] = address_hostname(a.name, zone);
	out["state"] = address_state_name(a.state);
	text_or_null(out, "public_ip", a.public_ip);
	// An own machine has one slot: its servers list at slot 0's game port.
	if (a.public_ip.empty())
		out["port"] = nullptr;
	else
		out["port"] = slot_game_port(0);
	out["source"] = a.source;
	text_or_null(out, "stripe_subscription_id", a.stripe_subscription_id);
	const std::optional<int64_t> server_id = server_of_address(db, a.id);
	const auto server = server_id ? find_server(db, *server_id) : std::nullopt;
	if (server) {
		crow::json::wvalue s;
		s["id"] = server->id;
		s["game_name"] = server->settings.game_name;
		s["state"] = state_name(server->state);
		out["server"] = std::move(s);
	} else {
		out["server"] = nullptr;
	}
	text_or_null(out, "reason", a.reason);
	put_time(out, "created_at", a.created_at);
	put_time(out, "ready_at", a.ready_at);
	put_time(out, "lapsed_at", a.lapsed_at);
	put_time(out, "released_at", a.released_at);
	if (admin) {
		const auto owner = a.owner_user_id ? get_user_by_id(db, *a.owner_user_id) : std::nullopt;
		if (owner) {
			out["owner"]["id"] = owner->id;
			out["owner"]["username"] = owner->username;
		} else {
			out["owner"] = nullptr;
		}
		text_or_null(out, "allocation_id", a.allocation_id);
		text_or_null(out, "dns_name", a.dns_name);
		out["dns_state"] = a.dns_state;
		text_or_null(out, "lapse_cause", a.lapse_cause);
	}
	return out;
}

void AddressRoutes::configure(bool on, std::string zone, int sellable) {
	on_ = on;
	zone_ = std::move(zone);
	sellable_ = sellable;
}

void AddressRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/me/addresses")
			.methods("GET"_method)([this](const crow::request &req) { return me_addresses(req); });
	CROW_ROUTE(app, "/api/me/addresses").methods("POST"_method)([this](const crow::request &req) { return me_name(req); });
	CROW_ROUTE(app, "/api/admin/addresses")
			.methods("GET"_method)([this](const crow::request &req) { return admin_addresses(req); });
	CROW_ROUTE(app, "/api/admin/addresses/<uint>")
			.methods("PUT"_method)([this](const crow::request &req, uint64_t id) { return admin_rename(req, id); });
	CROW_ROUTE(app, "/api/admin/addresses/<uint>/release")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) { return admin_release(req, id); });
	CROW_ROUTE(app, "/api/admin/users/<uint>/addresses")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) { return admin_comp(req, id); });
}

// GET /api/me/addresses — require_user; {"enabled" (the service sells static
// addresses), "zone", "unnamed" (how many of the account's address
// subscriptions still wait for a name), "addresses" (the account's, the held
// ones first: address_json)}. Cache-Control: no-store.
crow::response AddressRoutes::me_addresses(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["enabled"] = on_;
		out["zone"] = zone_;
		out["unnamed"] = static_cast<int64_t>(
				unnamed_address_subscriptions(*db_conn, access.user->id, billing::unix_now()).size());
		std::vector<crow::json::wvalue> list;
		for (const HostedAddress &a : addresses_of(*db_conn, access.user->id))
			list.push_back(address_json(*db_conn, a, zone_, false));
		out["addresses"] = std::move(list);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/me/addresses failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// POST /api/me/addresses — {"name"}: the account's next address subscription
// (its oldest unnamed one) named, and its address allocated by a worker
// (address.allocate): 201 {"address"} (allocating). require_user (401, 403
// without the CSRF header), 503 addresses_disabled, 400 invalid_json,
// invalid_field, invalid_name (address_names.h, its reason), 404 not_found (no
// active account), 409 name_taken, no_address (none of its subscriptions is
// unnamed).
crow::response AddressRoutes::me_name(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	if (!on_) return disabled();
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const std::string name = body->text_or("name", "");
	if (body->wrong_type()) return json_error(400, "invalid_field", "name is text");
	try {
		auto db_conn = pool_.acquire();
		NameResult named;
		{
			db::Transaction tx(*db_conn);
			named = name_subscription_address(*db_conn, access.user->id, name, billing::unix_now());
			if (auto refused = refusal_of(named)) return std::move(*refused);
			enqueue_address_allocate(*db_conn, named.id);
			tx.commit();
		}
		std::printf("[hosted] owner '%s' (id=%lld) named static address %lld '%s'\n",
				loggable(access.user->username).c_str(), static_cast<long long>(access.user->id),
				static_cast<long long>(named.id), loggable(name, 40).c_str());
		crow::json::wvalue out;
		out["address"] = address_json(*db_conn, *find_address(*db_conn, named.id), zone_, false);
		return json_reply(201, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/me/addresses failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// GET /api/admin/addresses — require_admin; {"enabled", "zone", "held" (the
// addresses the cloud may hold now: allocating, ready, lapsed, releasing),
// "sellable" (ONNET_FLEET_MAX_ADDRESSES as the fleet sells them), "addresses"
// (every one, the held first: address_json with the admin's fields), "orphans"
// (the addresses fleet.sweep found the cloud holding that no held row names:
// {allocation_id, public_ip, address_id, seen_at})}.
crow::response AddressRoutes::admin_addresses(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["enabled"] = on_;
		out["zone"] = zone_;
		out["held"] = count_held_addresses(*db_conn);
		out["sellable"] = sellable_;
		std::vector<crow::json::wvalue> list;
		for (const HostedAddress &a : list_addresses(*db_conn)) list.push_back(address_json(*db_conn, a, zone_, true));
		out["addresses"] = std::move(list);
		std::vector<crow::json::wvalue> orphans;
		for (const AddressOrphan &o : address_orphans(*db_conn)) {
			crow::json::wvalue e;
			e["allocation_id"] = o.allocation_id;
			e["public_ip"] = o.public_ip;
			e["address_id"] = o.address_id;
			e["seen_at"] = o.seen_at;
			orphans.push_back(std::move(e));
		}
		out["orphans"] = std::move(orphans);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/addresses failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// PUT /api/admin/addresses/<id> — {"name"}: the address renamed (any state: a
// released one's frees its old name); a ready one's record moves to the new
// name (address.allocate). require_admin; 400 invalid_json, invalid_field,
// invalid_name; 404; 409 name_taken; 200 {"address"}.
crow::response AddressRoutes::admin_rename(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const std::string name = body->text_or("name", "");
	if (body->wrong_type()) return json_error(400, "invalid_field", "name is text");
	try {
		auto db_conn = pool_.acquire();
		std::string was;
		{
			db::Transaction tx(*db_conn);
			const auto before = find_address(*db_conn, path_id(id));
			if (!before) return json_error(404, "not_found", "no static address has this id");
			was = before->name;
			if (auto refused = refusal_of(rename_address(*db_conn, before->id, name))) return std::move(*refused);
			if (before->state == AddressState::Ready || before->state == AddressState::Allocating)
				enqueue_address_allocate(*db_conn, before->id);
			tx.commit();
		}
		std::printf("[hosted] an admin renamed static address %lld '%s' to '%s'\n", static_cast<long long>(id),
				loggable(was, 40).c_str(), loggable(name, 40).c_str());
		crow::json::wvalue out;
		out["address"] = address_json(*db_conn, *find_address(*db_conn, path_id(id)), zone_, true);
		return json_reply(200, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] PUT /api/admin/addresses/%llu failed: %s\n", static_cast<unsigned long long>(id),
				e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/admin/addresses/<id>/release — {"confirm": "<its name>"}: the
// address lapsed by an admin (its server placed again without it, then it is
// released, whatever pays for it: a subscription is canceled in Stripe, not
// here). require_admin; 400 invalid_json, invalid_field, confirm_mismatch (the
// name typed is not the address's); 404; 409 not_releasable (lapsed,
// releasing or released already); 202 {"address"}.
crow::response AddressRoutes::admin_release(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const std::string confirm = body->text_or("confirm", "");
	if (body->wrong_type()) return json_error(400, "invalid_field", "confirm is text");
	try {
		auto db_conn = pool_.acquire();
		const auto a = find_address(*db_conn, path_id(id));
		if (!a) return json_error(404, "not_found", "no static address has this id");
		if (confirm != a->name)
			return json_error(400, "confirm_mismatch", "type the address's name to release it");
		if (!lapse_address(*db_conn, a->id, kLapseAdmin, "released by an admin"))
			return json_error(409, "not_releasable", "this address is on its way out already");
		std::printf("[hosted] an admin released static address %lld '%s'\n", static_cast<long long>(a->id),
				loggable(a->name, 40).c_str());
		crow::json::wvalue out;
		out["address"] = address_json(*db_conn, *find_address(*db_conn, a->id), zone_, true);
		return json_reply(202, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/addresses/%llu/release failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/admin/users/<id>/addresses — {"name"}: an admin's comp address for
// the account (counted toward its entitlement and the sold capacity, at most
// its own machines), allocated by a worker: 201 {"address"}. require_admin;
// 503 addresses_disabled; 400 invalid_json, invalid_field, invalid_name; 404
// not_found (no active player); 409 name_taken, address_limit.
crow::response AddressRoutes::admin_comp(const crow::request &req, uint64_t user_id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	if (!on_) return disabled();
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const std::string name = body->text_or("name", "");
	if (body->wrong_type()) return json_error(400, "invalid_field", "name is text");
	try {
		auto db_conn = pool_.acquire();
		NameResult named;
		{
			db::Transaction tx(*db_conn);
			named = add_comp_address(*db_conn, path_id(user_id), name, billing::unix_now());
			if (auto refused = refusal_of(named)) return std::move(*refused);
			enqueue_address_allocate(*db_conn, named.id);
			tx.commit();
		}
		std::printf("[hosted] an admin gave account %llu the comp static address %lld '%s'\n",
				static_cast<unsigned long long>(user_id), static_cast<long long>(named.id), loggable(name, 40).c_str());
		crow::json::wvalue out;
		out["address"] = address_json(*db_conn, *find_address(*db_conn, named.id), zone_, true);
		return json_reply(201, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/users/%llu/addresses failed: %s\n",
				static_cast<unsigned long long>(user_id), e.what());
		return json_error(500, "db_error");
	}
}

} // namespace opennova::novaworld_server::hosted
