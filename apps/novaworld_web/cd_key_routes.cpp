#include "cd_key_routes.h"

#include "auth.h"
#include "core/loggable.h"
#include "http_json.h"
#include "program/server_config.h"

#include <base/io/strutil.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

crow::json::wvalue key_json(const cdkeys::CdKey &k) {
	crow::json::wvalue out;
	out["id"] = k.id;
	out["title"] = k.title;
	out["serial"] = k.serial;
	out["wire_key"] = k.wire_key;
	out["state"] = k.state();
	if (k.account) {
		out["account"]["id"] = k.account->id;
		out["account"]["username"] = k.account->username;
	} else {
		out["account"] = nullptr;
	}
	out["created_by"] = k.created_by;
	out["created_at"] = k.created_at;
	text_or_null(out, "assigned_at", k.assigned_at);
	text_or_null(out, "revoked_at", k.revoked_at);
	out["activations"] = k.activations;
	return out;
}

crow::json::wvalue activation_json(const cdkeys::Activation &a) {
	crow::json::wvalue out;
	out["id"] = a.id;
	out["key_id"] = a.key_id;
	out["title"] = a.title;
	out["serial"] = a.serial;
	out["nwpssk"] = a.nwpssk;
	out["nwact"] = a.nwact;
	out["created_at"] = a.created_at;
	return out;
}

std::vector<crow::json::wvalue> keys_json(const std::vector<cdkeys::CdKey> &keys) {
	std::vector<crow::json::wvalue> out;
	for (const auto &k : keys)
		out.push_back(key_json(k));
	return out;
}

std::vector<crow::json::wvalue> activations_json(
		const std::vector<cdkeys::Activation> &activations) {
	std::vector<crow::json::wvalue> out;
	for (const auto &a : activations)
		out.push_back(activation_json(a));
	return out;
}

// The titles keys are issued for, as the list reply names them.
std::vector<crow::json::wvalue> titles_json() {
	std::vector<crow::json::wvalue> out;
	crow::json::wvalue dfx2;
	dfx2 = "dfx2";
	out.push_back(std::move(dfx2));
	return out;
}

// A non-negative decimal query value; nullopt when absent, `bad` set when
// present and not one.
std::optional<int> query_count(const crow::request &req, const char *name, bool &bad) {
	const char *value = req.url_params.get(name);
	if (value == nullptr || *value == '\0')
		return std::nullopt;
	const std::string text(value);
	const auto parsed = strutil::all_digits(text) ? strutil::parse_int(text) : std::nullopt;
	if (!parsed)
		bad = true;
	return parsed;
}

// Who made a key: the admin session's account, or the Bearer token.
std::string creator_of(const RouteAccess &caller) {
	return caller.user ? "admin " + caller.user->username : std::string("token");
}

crow::response db_failure(const char *route, const db::Error &e) {
	std::fprintf(stderr, "[http] %s failed: %s\n", route, e.what());
	return json_error(500, "db_error");
}

crow::response no_such_key() {
	return json_error(404, "not_found", "no CD key has this id");
}

} // namespace

void CdKeyRoutes::configure(const ServerConfig &config) {
	auto_mint_ = config.cd_key_auto_mint;
	std::printf("[cdkey] a DFX2 account's first activation %s\n",
			auto_mint_ ? "mints its key (ONNET_CD_KEY_AUTO_MINT on)"
					   : "needs a key an admin assigned (ONNET_CD_KEY_AUTO_MINT off)");
}

void CdKeyRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/cd-keys").methods("GET"_method)([this](const crow::request &req) {
		return list(req);
	});
	CROW_ROUTE(app, "/api/admin/cd-keys").methods("POST"_method)([this](const crow::request &req) {
		return generate(req);
	});
	CROW_ROUTE(app, "/api/admin/cd-keys/<uint>/assign")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return assign(req, id); });
	CROW_ROUTE(app, "/api/admin/cd-keys/<uint>/revoke")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return revoke(req, id); });
	CROW_ROUTE(app, "/api/admin/cd-keys/<uint>/activations")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) {
				return key_activations(req, id);
			});
	CROW_ROUTE(app, "/api/admin/cd-keys/<uint>/activations")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return activate(req, id); });
	CROW_ROUTE(app, "/api/admin/users/<int>/cd-keys")
			.methods("GET"_method)(
					[this](const crow::request &req, int64_t id) { return account(req, id); });
}

// GET /api/admin/cd-keys — {"total" (the keys the filter selects), "limit",
// "offset", "titles" (the titles keys are issued for), "auto_mint", "keys"}:
// each key {id, title, serial, wire_key, state, account ({id, username} or
// null), created_by, created_at, assigned_at, revoked_at (UTC, null until
// set), activations (how many machines)}. 400 invalid_field for a title no key
// is issued for, a state that is none of the three, or a limit or offset that
// is no count (a limit of 0 or past kMaxListed among them).
crow::response CdKeyRoutes::list(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	cdkeys::KeyFilter filter;
	const char *title = req.url_params.get("title");
	const char *state = req.url_params.get("state");
	const char *search = req.url_params.get("q");
	filter.title = title != nullptr ? title : "";
	filter.state = state != nullptr ? state : "";
	filter.search = search != nullptr ? search : "";
	if (!filter.title.empty() && !cdkeys::title_activates(filter.title))
		return json_error(400, "invalid_field", "no CD keys are issued for this title");
	if (filter.state == "all")
		filter.state.clear();
	if (!filter.state.empty() && filter.state != "unassigned" && filter.state != "assigned" &&
			filter.state != "revoked")
		return json_error(400, "invalid_field", "state is unassigned, assigned, revoked or all");
	bool bad = false;
	const auto limit = query_count(req, "limit", bad);
	const auto offset = query_count(req, "offset", bad);
	if (bad || (limit && (*limit < 1 || static_cast<size_t>(*limit) > kMaxListed)))
		return json_error(400, "invalid_field", "limit is 1 to 500 and offset a count");
	if (limit)
		filter.limit = static_cast<size_t>(*limit);
	if (offset)
		filter.offset = static_cast<size_t>(*offset);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		int64_t total = 0;
		out["keys"] = keys_json(cdkeys::list_keys(*db_conn, filter, &total));
		out["total"] = total;
	} catch (const db::Error &e) {
		return db_failure("GET /api/admin/cd-keys", e);
	}
	out["limit"] = filter.limit;
	out["offset"] = filter.offset;
	out["titles"] = titles_json();
	out["auto_mint"] = auto_mint_;
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// POST /api/admin/cd-keys — {"title", "count"}: `count` (1..kMaxGenerate) new
// unassigned keys for the title, 201 {"keys"}. 400 invalid_json, missing_field,
// invalid_field (a title no key is issued for, a count out of range).
crow::response CdKeyRoutes::generate(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto title = body->text("title");
	const auto count = body->integer("count");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!title || !count)
		return json_error(400, "missing_field", "title and count are required");
	if (!cdkeys::title_activates(*title))
		return json_error(400, "invalid_field", "no CD keys are issued for this title");
	if (*count < 1 || *count > static_cast<int64_t>(kMaxGenerate))
		return json_error(400, "invalid_field", "count is 1 to 100");
	try {
		const auto keys = cdkeys::generate_keys(
				*pool_.acquire(), *title, static_cast<size_t>(*count), creator_of(caller));
		std::printf("[cdkey] %zu %s key(s) generated by %s\n", keys.size(),
				loggable(*title).c_str(), loggable(creator_of(caller)).c_str());
		crow::json::wvalue out;
		out["keys"] = keys_json(keys);
		return json_reply(201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/cd-keys", e);
	}
}

// POST /api/admin/cd-keys/<id>/assign — {"username"} or {"user_id"}: the key onto
// that player account, 200 {"key"}. 404 not_found (no such key), no_such_account;
// 409 revoked, already_assigned (the key is on an account), account_has_key (the
// account holds a live key for the title); 400 invalid_json, missing_field,
// invalid_field.
crow::response CdKeyRoutes::assign(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto username = body->text("username");
	const auto user_id = body->integer("user_id");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!username && !user_id)
		return json_error(400, "missing_field", "username or user_id is required");
	try {
		auto db_conn = pool_.acquire();
		std::optional<UserRecord> user = user_id ? get_user_by_id(*db_conn, *user_id)
												 : get_user_by_username(*db_conn, *username);
		if (!user || user->kind != "player")
			return json_error(404, "no_such_account", "no player account by that name");
		switch (cdkeys::assign_key(*db_conn, path_id(id), user->id)) {
			case cdkeys::AssignOutcome::Assigned:
				break;
			case cdkeys::AssignOutcome::NoSuchKey:
				return no_such_key();
			case cdkeys::AssignOutcome::NoSuchAccount:
				return json_error(404, "no_such_account", "no player account by that name");
			case cdkeys::AssignOutcome::Revoked:
				return json_error(409, "revoked", "this key is revoked");
			case cdkeys::AssignOutcome::AlreadyAssigned:
				return json_error(409, "already_assigned", "this key is already on an account");
			case cdkeys::AssignOutcome::AccountHasKey:
				return json_error(409, "account_has_key",
						"the account already holds a live key for this title");
		}
		crow::json::wvalue out;
		out["key"] = key_json(*cdkeys::find_key(*db_conn, path_id(id)));
		return json_reply(200, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/cd-keys/<id>/assign", e);
	}
}

// POST /api/admin/cd-keys/<id>/revoke — the key activates no machine again
// (the machines it activated keep their registry value: nothing reaches them),
// 200 {"key"}. 404 not_found, 409 already_revoked.
crow::response CdKeyRoutes::revoke(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		switch (cdkeys::revoke_key(*db_conn, path_id(id))) {
			case cdkeys::RevokeOutcome::Revoked:
				break;
			case cdkeys::RevokeOutcome::NoSuchKey:
				return no_such_key();
			case cdkeys::RevokeOutcome::AlreadyRevoked:
				return json_error(409, "already_revoked", "this key is already revoked");
		}
		crow::json::wvalue out;
		out["key"] = key_json(*cdkeys::find_key(*db_conn, path_id(id)));
		return json_reply(200, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/cd-keys/<id>/revoke", e);
	}
}

// GET /api/admin/cd-keys/<id>/activations — {"key", "activations"}, oldest first,
// each {id, key_id, title, serial, nwpssk, nwact, created_at}. 404 not_found.
crow::response CdKeyRoutes::key_activations(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto key = cdkeys::find_key(*db_conn, path_id(id));
		if (!key)
			return no_such_key();
		out["key"] = key_json(*key);
		out["activations"] = activations_json(cdkeys::activations_of_key(*db_conn, key->id));
	} catch (const db::Error &e) {
		return db_failure("GET /api/admin/cd-keys/<id>/activations", e);
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// POST /api/admin/cd-keys/<id>/activations — {"nwpssk"}: a manual activation, the
// NWACT that activates the key on the machine that NWPSSK names (the value the
// game's registry CDKIID takes), recorded as that machine's. 201 {"nwact",
// "reactivated": false, "key"} for a new machine, 200 with "reactivated": true and
// the same NWACT for one the key already activated. 400 invalid_field (no
// 23-letter NWPSSK), 404 not_found, 409 revoked.
crow::response CdKeyRoutes::activate(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto nwpssk = body->text("nwpssk");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!nwpssk)
		return json_error(400, "missing_field", "nwpssk is required");
	try {
		const auto result = cdkeys::activate_key(*pool_.acquire(), path_id(id), *nwpssk);
		switch (result.outcome) {
			case cdkeys::ActivateOutcome::Activated:
			case cdkeys::ActivateOutcome::Reactivated:
				break;
			case cdkeys::ActivateOutcome::BadFingerprint:
				return json_error(400, "invalid_field", "nwpssk is the game's 23-letter NWPSSK");
			case cdkeys::ActivateOutcome::Revoked:
				return json_error(409, "revoked", "this key is revoked");
			default:
				return no_such_key();
		}
		const bool again = result.outcome == cdkeys::ActivateOutcome::Reactivated;
		crow::json::wvalue out;
		out["nwact"] = result.nwact;
		out["reactivated"] = again;
		out["key"] = result.key ? key_json(*result.key) : crow::json::wvalue{};
		return json_reply(again ? 200 : 201, out);
	} catch (const db::Error &e) {
		return db_failure("POST /api/admin/cd-keys/<id>/activations", e);
	}
}

// GET /api/admin/users/<id>/cd-keys — {"account": {id, username}, "keys" (newest
// first, revoked ones included), "activations" (every machine its keys
// activated, newest first)}. 404 not_found for no player account by that id.
crow::response CdKeyRoutes::account(const crow::request &req, int64_t user_id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto user = get_user_by_id(*db_conn, user_id);
		if (!user || user->kind != "player")
			return json_error(404, "not_found", "no player account has this id");
		out["account"]["id"] = user->id;
		out["account"]["username"] = user->username;
		out["keys"] = keys_json(cdkeys::keys_of_account(*db_conn, user->id));
		out["activations"] = activations_json(cdkeys::activations_of_account(*db_conn, user->id));
	} catch (const db::Error &e) {
		return db_failure("GET /api/admin/users/<id>/cd-keys", e);
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

std::string CdKeyRoutes::login_activation(int64_t user_id, const catalog::GameRow &game,
		const std::map<std::string, std::string> &cookies) {
	const auto cookie = [&cookies](const char *name) {
		const auto it = cookies.find(name);
		return it == cookies.end() ? std::string() : it->second;
	};
	if (user_id == 0 || !cdkeys::needs_activation(game.title, cookie("NWCDKIID")))
		return {};
	cdkeys::ActivateResult result;
	try {
		result = cdkeys::activate_account(
				*pool_.acquire(), user_id, game.title, cookie("NWPSSK"), auto_mint_);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[cdkey] activation of user %lld failed: %s\n",
				static_cast<long long>(user_id), e.what());
		return {};
	}
	const char *what = "no activation";
	switch (result.outcome) {
		case cdkeys::ActivateOutcome::Activated:
			what = result.minted ? "activated a new machine under a key minted for it"
								 : "activated a new machine";
			break;
		case cdkeys::ActivateOutcome::Reactivated:
			what = "activated a machine again";
			break;
		case cdkeys::ActivateOutcome::BadFingerprint:
			what = "no activation: the client sent no NWPSSK";
			break;
		case cdkeys::ActivateOutcome::NoKey:
			what = "no activation: the account holds no key (auto-minting is off)";
			break;
		case cdkeys::ActivateOutcome::Revoked:
			what = "no activation: the account's key is revoked";
			break;
		default:
			break;
	}
	std::printf("[cdkey] user %lld %s: %s%s\n", static_cast<long long>(user_id),
			loggable(game.title).c_str(), what,
			result.key ? (" (key " + std::to_string(result.key->id) + ")").c_str() : "");
	return result.nwact;
}

} // namespace opennova::novaworld_server
