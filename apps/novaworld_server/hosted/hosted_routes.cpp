#include "hosted_routes.h"

#include "auth.h"
#include "billing/entitlement.h"
#include "fleet_repository.h"
#include "http_json.h"
#include "loggable.h"
#include "server_config.h"

#include <base/io/cp1252.h>
#include <net/novaworld/host_repository.h>

#include <climits>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

void text_or_null(crow::json::wvalue &out, const char *key, const std::string &text) {
	if (text.empty())
		out[key] = nullptr;
	else
		out[key] = text;
}

// A JSON list of strings.
std::vector<crow::json::wvalue> text_list(const std::vector<std::string> &texts) {
	std::vector<crow::json::wvalue> out;
	for (const std::string &t : texts) {
		crow::json::wvalue item;
		item = t;
		out.push_back(std::move(item));
	}
	return out;
}

crow::json::wvalue settings_json(const HostedSettings &s) {
	crow::json::wvalue out;
	out["game"] = s.game;
	out["game_name"] = s.game_name;
	out["max_players"] = s.max_players;
	out["game_password"] = s.game_password;
	out["time_limit"] = s.time_limit;
	out["kill_limit"] = s.kill_limit;
	out["max_score"] = s.max_score;
	out["respawn"] = s.respawn;
	out["start_delay"] = s.start_delay;
	out["team_ff"] = s.team_ff;
	out["expansion"] = s.expansion;
	out["missions"] = text_list(s.missions);
	return out;
}

crow::json::wvalue range_json(const SettingRange &range) {
	crow::json::wvalue out;
	out["min"] = range.min;
	out["max"] = range.max;
	return out;
}

// An offered data set, as the site's start form takes it.
crow::json::wvalue data_set_json(const DataSet &d) {
	crow::json::wvalue out;
	out["game"] = d.game;
	out["display_name"] = d.display_name;
	std::vector<crow::json::wvalue> expansions;
	for (const Expansion &e : d.expansions) {
		crow::json::wvalue item;
		item["code"] = e.code;
		item["name"] = e.name;
		expansions.push_back(std::move(item));
	}
	out["expansions"] = std::move(expansions);
	out["default_missions"] = text_list(d.missions);
	crow::json::wvalue limits;
	limits["max_players"] = range_json(d.limits.max_players);
	limits["time_limit"] = range_json(d.limits.time_limit);
	limits["kill_limit"] = range_json(d.limits.kill_limit);
	limits["max_score"] = range_json(d.limits.max_score);
	limits["respawn"] = range_json(d.limits.respawn);
	limits["start_delay"] = range_json(d.limits.start_delay);
	out["limits"] = std::move(limits);
	return out;
}

// The listing a hosted server holds now (its service account's claim), or null.
void put_listing(crow::json::wvalue &out, db::Database &db, const HostedServer &s) {
	std::optional<hostdb::HostRow> row;
	if (s.service_user_id) {
		if (const auto rid = listing_rid_of(db, *s.service_user_id)) row = hostdb::find_host_by_rid(db, *rid);
	}
	if (!row) {
		out["listing"] = nullptr;
		return;
	}
	crow::json::wvalue listing;
	listing["rid"] = row->rid;
	listing["server_name"] = retail_text_to_utf8(row->server_name);
	listing["mission_name"] = retail_text_to_utf8(row->mission_name);
	listing["players"] = row->player_count;
	listing["max_players"] = row->max_players;
	listing["host_ip"] = row->host_ip;
	listing["host_port"] = row->host_port;
	out["listing"] = std::move(listing);
}

// {id, username} of an account, or null.
void put_account(crow::json::wvalue &out, const char *key, db::Database &db, std::optional<int64_t> id) {
	const auto user = id ? get_user_by_id(db, *id) : std::nullopt;
	if (!user) {
		out[key] = nullptr;
		return;
	}
	crow::json::wvalue account;
	account["id"] = user->id;
	account["username"] = user->username;
	out[key] = std::move(account);
}

// A path's server id; 0 (no server's) past int64.
int64_t server_id_of(uint64_t id) {
	return id <= static_cast<uint64_t>(INT64_MAX) ? static_cast<int64_t>(id) : 0;
}

crow::response server_reply(int code, db::Database &db, int64_t id, bool admin) {
	crow::json::wvalue out;
	if (const auto server = find_server(db, id))
		out["server"] = server_json(db, *server, admin);
	else
		out["server"] = nullptr;
	return json_reply(code, out);
}

void log_owner(const WebSessionUser &user, const char *what, const std::string &outcome) {
	std::printf("[hosted] owner '%s' (id=%lld) %s -> %s\n", loggable(user.username).c_str(),
			static_cast<long long>(user.id), what, outcome.c_str());
}

} // namespace

// A hosted server for its owner (GET /api/me/servers and /api/me/servers/<id>,
// the start and stop replies) or, with `admin`, for the fleet view: its owner,
// its service account and its machine besides. Never its admin password.
crow::json::wvalue server_json(db::Database &db, const HostedServer &s, bool admin) {
	crow::json::wvalue e;
	e["id"] = s.id;
	e["state"] = state_name(s.state);
	e["live"] = is_live(s.state);
	e["own_machine"] = s.own_machine;
	e["settings"] = settings_json(s.settings);
	// The names the site shows for its game and expansion: the catalog's, so an
	// ended server's read the same once its data set is no longer offered.
	const DataSet *data_set = find_data_set(known_data_sets(), s.settings.game);
	const Expansion *expansion = data_set ? find_expansion(*data_set, s.settings.expansion) : nullptr;
	e["game_display_name"] = data_set ? data_set->display_name : s.settings.game;
	e["expansion_display_name"] = expansion ? expansion->name : s.settings.expansion;
	e["restarts"] = s.restarts;
	text_or_null(e, "failure_reason", s.failure_reason);
	e["created_at"] = s.created_at;
	e["updated_at"] = s.updated_at;
	text_or_null(e, "running_at", s.running_at);
	text_or_null(e, "ended_at", s.ended_at);
	put_listing(e, db, s);
	if (admin) {
		put_account(e, "owner", db, s.owner_user_id);
		put_account(e, "service_account", db, s.service_user_id);
		e["provider"] = s.provider;
		e["machine_id"] = s.machine_id;
		e["machine_address"] = s.machine_address;
		e["admin_port"] = s.admin_port;
	}
	return e;
}

void HostedRoutes::configure(const ServerConfig &config, const PlacementPolicy &fleet) {
	provider_ = config.hosted_provider.empty() ? std::string("none") : config.hosted_provider;
	// main() has warned of texts that do not read (and runs no driver then).
	std::vector<DataSetConfig> configs;
	std::string error;
	data_sets_.clear();
	if (parse_data_set_configs(config.hosted_data_sets, configs, error))
		data_sets_ = offered_data_sets(provider_, configs);
	enabled_ = provider_ != "none" && !data_sets_.empty();
	cap_ = config.hosted_cap;
	fleet_ = fleet;
	std::string games;
	for (const DataSet &d : data_sets_) games += (games.empty() ? "" : ", ") + d.game;
	std::printf("[http] hosted servers: %s (provider %s, cap %d, data sets %s)\n", enabled_ ? "on" : "off",
			loggable(provider_).c_str(), cap_, games.empty() ? "(none)" : games.c_str());
}

void HostedRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/me/servers")
			.methods("GET"_method)([this](const crow::request &req) { return me_servers(req); });
	CROW_ROUTE(app, "/api/me/servers")
			.methods("POST"_method)([this](const crow::request &req) { return me_start(req); });
	CROW_ROUTE(app, "/api/me/servers/<uint>")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) { return me_server(req, id); });
	CROW_ROUTE(app, "/api/me/servers/<uint>/stop")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) { return me_stop(req, id); });
	CROW_ROUTE(app, "/api/admin/hosted")
			.methods("GET"_method)([this](const crow::request &req) { return admin_fleet(req); });
	CROW_ROUTE(app, "/api/admin/hosted/stop-all")
			.methods("POST"_method)([this](const crow::request &req) { return admin_stop_all(req); });
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/stop")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) {
				return admin_stop(req, id);
			});
}

// GET /api/me/servers — the logged-in account's hosted servers: {"enabled" (this
// service runs hosted servers), "full" (a start now would be refused
// at_capacity: every place under ONNET_HOSTED_CAP is taken, or the fleet has no
// place for a shared server and none for one on an own machine the account
// still has free), "entitlement" (its hosting
// entitlement, billing/entitlement.h: {max_servers, used (its live servers, a
// stopping one included), own_machines, own_used (those of them on own
// machines), can_start, source ("none", "tier" or "comp")}), "data_sets" (the
// games a start may name, in the site's order: {game, display_name, expansions
// [{code, name}], default_missions (the rotation a start on it takes when it
// names none, and the missions the site's rotation picker offers), limits
// {max_players, time_limit, kill_limit, max_score, respawn, start_delay: {min,
// max}}}), "servers"}: its live servers by id, oldest first, then its newest
// kEndedShown ended ones, newest first, each {id, state, live, own_machine,
// settings, game_display_name, expansion_display_name, restarts,
// failure_reason, created_at, updated_at, running_at, ended_at (UTC, null until
// set), listing ({rid, server_name, mission_name, players, max_players,
// host_ip, host_port} while listed, else null)}, without its events. 401 with no
// live session.
crow::response HostedRoutes::me_servers(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	const int64_t owner = access.user->id;
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["enabled"] = enabled_;
		const billing::Entitlement entitlement = billing::entitlement_of(*db_conn, owner, billing::unix_now());
		const int own_used = count_own_live_of(*db_conn, owner);
		bool full = enabled_ && count_live(*db_conn) >= cap_;
		if (enabled_ && !full) {
			full = !fleet_has_room(*db_conn, fleet_, false) &&
					(own_used >= entitlement.own_machines || !fleet_has_room(*db_conn, fleet_, true));
		}
		out["full"] = full;
		crow::json::wvalue granted;
		granted["max_servers"] = entitlement.max_servers;
		granted["used"] = count_live_of(*db_conn, owner);
		granted["own_machines"] = entitlement.own_machines;
		granted["own_used"] = own_used;
		granted["can_start"] = entitlement.can_start;
		granted["source"] = entitlement.source;
		out["entitlement"] = std::move(granted);
		std::vector<crow::json::wvalue> sets;
		if (enabled_) {
			for (const DataSet &d : data_sets_) sets.push_back(data_set_json(d));
		}
		out["data_sets"] = std::move(sets);
		std::vector<crow::json::wvalue> servers;
		for (const HostedServer &s : list_servers_of(*db_conn, owner, kEndedShown))
			servers.push_back(server_json(*db_conn, s, false));
		out["servers"] = std::move(servers);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/me/servers failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// GET /api/me/servers/<id> — one of the account's servers, in any state:
// {"server"}, as the list's with "events", its newest kEventsShown transitions
// oldest first ({from, to, detail, at}; the request's own has from ""). 404
// not_found for an id that is not the account's (another account's included).
crow::response HostedRoutes::me_server(const crow::request &req, uint64_t id) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto server = owned_server(*db_conn, server_id_of(id), access.user->id);
		if (!server) return json_error(404, "not_found", "this account has no hosted server with this id");
		crow::json::wvalue e = server_json(*db_conn, *server, false);
		std::vector<crow::json::wvalue> events;
		for (const HostedEvent &ev : list_events(*db_conn, server->id, kEventsShown)) {
			crow::json::wvalue j;
			j["from"] = ev.from_state;
			j["to"] = ev.to_state;
			j["detail"] = ev.detail;
			j["at"] = ev.at;
			events.push_back(std::move(j));
		}
		e["events"] = std::move(events);
		out["server"] = std::move(e);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/me/servers/%llu failed: %s\n", static_cast<unsigned long long>(id),
				e.what());
		return json_error(500, "db_error");
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// POST /api/me/servers — a new hosted server of the account's, started with the
// body's settings: {"game" (an offered data set; absent: the first),
// "game_name" (required), "max_players", "game_password", "time_limit",
// "kill_limit", "max_score", "respawn", "start_delay" (integers), "team_ff"
// (true/false), "expansion" ("" or one of the game's), "missions" (a list of
// .bms names, the first the starting map; absent: the game's default
// rotation), "own_machine" (true: on one of the machines the account's
// own-machine add-on pays for; absent: false, a shared machine)}, each absent
// one its default. In order: require_user (401, 403 without the CSRF header),
// 503 hosting_disabled when this service runs none, one token from the
// account's kStartStopPerUser bucket (429), 400 invalid_json / invalid_field (a
// mistyped field), 400 invalid_settings with the refused "field" and why
// (hosted_settings.h check_settings), then the request: 403 not_entitled for an
// account not active or with no hosting entitlement, 403 payment_past_due while
// its support tier's payment is past due, 409 server_limit with "max_servers"
// and "used" when every server it may run is live (a stopping one counts), 409
// no_own_machine when each own machine it pays for already runs one, 409
// at_capacity when ONNET_HOSTED_CAP servers are live or the fleet has no place
// for it (the supervisor's planner over the machines and the servers waiting
// before it: every slot taken and the fleet at its maximum machines); 202
// {"server"} (state requested) once written. The supervisor takes it from there.
crow::response HostedRoutes::me_start(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	const WebSessionUser &user = *access.user;
	if (!enabled_) return json_error(503, "hosting_disabled", "this NovaWorld service runs no hosted servers");
	if (const int64_t wait = start_stop_per_user_.take(
				"hosted|" + std::to_string(user.id), RateLimiter::Clock::now())) {
		log_owner(user, "start", "rate_limited");
		return too_many_requests(wait);
	}
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	HostedSettings settings;
	settings.game = data_sets_.front().game; // enabled_: at least one is offered
	if (auto v = body->text("game")) settings.game = *v;
	if (const DataSet *data_set = find_data_set(data_sets_, settings.game)) settings.missions = data_set->missions;
	if (auto v = body->text("game_name")) settings.game_name = *v;
	if (auto v = body->text("game_password")) settings.game_password = *v;
	if (auto v = body->text("expansion")) settings.expansion = *v;
	// A number past an int reads as one no range admits.
	auto number = [&body](const char *key, int &out) {
		if (const auto v = body->integer(key))
			out = (*v < INT_MIN || *v > INT_MAX) ? INT_MIN : static_cast<int>(*v);
	};
	number("max_players", settings.max_players);
	number("time_limit", settings.time_limit);
	number("kill_limit", settings.kill_limit);
	number("max_score", settings.max_score);
	number("respawn", settings.respawn);
	number("start_delay", settings.start_delay);
	if (auto v = body->flag("team_ff")) settings.team_ff = *v;
	if (auto v = body->text_list("missions")) settings.missions = *v;
	const bool own_machine = body->flag("own_machine").value_or(false);
	if (body->wrong_type()) return json_error(400, "invalid_field");
	if (const auto refused = check_settings(settings, data_sets_)) {
		crow::json::wvalue out;
		out["error"] = "invalid_settings";
		out["field"] = refused->field;
		out["message"] = refused->reason;
		log_owner(user, "start", "invalid_settings (" + refused->field + ")");
		return json_reply(400, out);
	}
	try {
		auto db_conn = pool_.acquire();
		const RequestResult result = request_server(
				*db_conn, user.id, settings, data_sets_, cap_, billing::unix_now(), own_machine, &fleet_);
		switch (result.outcome) {
			case RequestOutcome::Created:
				log_owner(user, "start",
						"server " + std::to_string(result.id) + " requested" + (own_machine ? " (own machine)" : ""));
				return server_reply(202, *db_conn, result.id, false);
			case RequestOutcome::InvalidSettings:
				// check_settings passed above, and request_server runs the same
				// check: unreachable, so a fault of the service's own.
				std::fprintf(stderr,
						"[hosted] ERROR the start request refused settings check_settings passed\n");
				log_owner(user, "start", "internal_error");
				return json_error(500, "internal_error");
			case RequestOutcome::NoSuchUser:
			case RequestOutcome::NotEntitled:
				log_owner(user, "start", "not_entitled");
				return json_error(403, "not_entitled", "hosting a server needs a support tier");
			case RequestOutcome::PastDue:
				log_owner(user, "start", "payment_past_due");
				return json_error(403, "payment_past_due",
						"the support tier's payment is past due: running servers keep running, none starts");
			case RequestOutcome::AtLimit: {
				log_owner(user, "start", "server_limit (" + std::to_string(result.used) + " of " +
								std::to_string(result.max_servers) + ")");
				crow::json::wvalue out;
				out["error"] = "server_limit";
				out["message"] = "every hosted server this account may run is live; stop one first";
				out["max_servers"] = result.max_servers;
				out["used"] = result.used;
				return json_reply(409, out);
			}
			case RequestOutcome::NoOwnMachine:
				log_owner(user, "start", "no_own_machine");
				return json_error(409, "no_own_machine", "each own machine this account pays for runs a server");
			case RequestOutcome::AtCapacity:
				log_owner(user, "start", "at_capacity");
				return json_error(409, "at_capacity", "every hosted server place is taken; try again later");
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/me/servers failed: %s\n", e.what());
	}
	return json_error(500, "db_error");
}

// POST /api/me/servers/<id>/stop — one of the account's live servers goes to
// stopping (the supervisor then asks it to stop and ends its machine).
// require_user (401, 403 without the CSRF header), the start's bucket (429),
// 404 not_found for an id that is not the account's, 409 not_live for one that
// has ended, else 202 {"server"} (a server already stopping included, and one
// the supervisor finished in between, stopped).
crow::response HostedRoutes::me_stop(const crow::request &req, uint64_t id) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	const WebSessionUser &user = *access.user;
	if (const int64_t wait = start_stop_per_user_.take(
				"hosted|" + std::to_string(user.id), RateLimiter::Clock::now())) {
		log_owner(user, "stop", "rate_limited");
		return too_many_requests(wait);
	}
	try {
		auto db_conn = pool_.acquire();
		const auto server = owned_server(*db_conn, server_id_of(id), user.id);
		if (!server) {
			log_owner(user, "stop", "not_found");
			return json_error(404, "not_found", "this account has no hosted server with this id");
		}
		if (!is_live(server->state)) {
			log_owner(user, "stop", "server " + std::to_string(server->id) + " not_live");
			return json_error(409, "not_live", "this hosted server has ended");
		}
		const bool asked = request_stop(*db_conn, server->id, "stopped by its owner");
		log_owner(user, "stop", "server " + std::to_string(server->id) + (asked ? " stopping" : " already stopping"));
		return server_reply(202, *db_conn, server->id, false);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/me/servers/%llu/stop failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
}

// GET /api/admin/hosted — the fleet: {"enabled", "provider", "cap", "live" (how
// many are live), "servers"}, the newest kFleetShown, live ones first, each as
// GET /api/me/servers' plus "owner" and "service_account" ({id, username} or
// null), "provider", "machine_id", "machine_address" and "admin_port".
crow::response HostedRoutes::admin_fleet(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["enabled"] = enabled_;
		out["provider"] = provider_;
		out["cap"] = cap_;
		out["live"] = count_live(*db_conn);
		std::vector<crow::json::wvalue> servers;
		for (const HostedServer &s : list_servers(*db_conn, kFleetShown)) servers.push_back(server_json(*db_conn, s, true));
		out["servers"] = std::move(servers);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/hosted failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// POST /api/admin/hosted/stop-all — the kill switch: every live server goes to
// stopping. 200 {"stopping": how many were asked}.
crow::response HostedRoutes::admin_stop_all(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	try {
		const size_t stopping = request_stop_all(*pool_.acquire(), "stopped by an admin (stop all)");
		std::printf("[hosted] stop all: %zu server(s) stopping\n", stopping);
		crow::json::wvalue out;
		out["stopping"] = stopping;
		return json_reply(200, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/hosted/stop-all failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/admin/hosted/<id>/stop — one server's stop: 404 not_found for no
// such server, 409 not_live for one that has ended, else 202 {"server"}.
crow::response HostedRoutes::admin_stop(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const int64_t server_id = server_id_of(id);
		const auto server = find_server(*db_conn, server_id);
		if (!server) return json_error(404, "not_found", "no hosted server has this id");
		if (!is_live(server->state)) return json_error(409, "not_live", "this hosted server has ended");
		request_stop(*db_conn, server_id, "stopped by an admin");
		return server_reply(202, *db_conn, server_id, true);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/hosted/%llu/stop failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
}

} // namespace opennova::novaworld_server::hosted
