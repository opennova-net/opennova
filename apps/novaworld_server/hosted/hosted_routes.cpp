#include "hosted_routes.h"

#include "auth.h"
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

crow::json::wvalue settings_json(const HostedSettings &s) {
	crow::json::wvalue out;
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
	std::vector<crow::json::wvalue> missions;
	for (const std::string &m : s.missions) {
		crow::json::wvalue item;
		item = m;
		missions.push_back(std::move(item));
	}
	out["missions"] = std::move(missions);
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

// A hosted server for its owner (GET /api/me/server, the start and stop
// replies) or, with `admin`, for the fleet view: its owner, its service account
// and its machine besides. Never its admin password.
crow::json::wvalue server_json(db::Database &db, const HostedServer &s, bool admin) {
	crow::json::wvalue e;
	e["id"] = s.id;
	e["state"] = state_name(s.state);
	e["live"] = is_live(s.state);
	e["settings"] = settings_json(s.settings);
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

void HostedRoutes::configure(const ServerConfig &config) {
	provider_ = config.hosted_provider.empty() ? std::string("none") : config.hosted_provider;
	enabled_ = provider_ != "none";
	cap_ = config.hosted_cap;
	default_missions_ = config.hosted_missions;
	std::printf("[http] hosted servers: %s (provider %s, cap %d)\n", enabled_ ? "on" : "off",
			loggable(provider_).c_str(), cap_);
}

void HostedRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/me/server")([this](const crow::request &req) { return me_server(req); });
	CROW_ROUTE(app, "/api/me/server/start")
			.methods("POST"_method)([this](const crow::request &req) { return me_start(req); });
	CROW_ROUTE(app, "/api/me/server/stop")
			.methods("POST"_method)([this](const crow::request &req) { return me_stop(req); });
	CROW_ROUTE(app, "/api/admin/hosted")
			.methods("GET"_method)([this](const crow::request &req) { return admin_fleet(req); });
	CROW_ROUTE(app, "/api/admin/hosted/stop-all")
			.methods("POST"_method)([this](const crow::request &req) { return admin_stop_all(req); });
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/stop")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) {
				return admin_stop(req, id);
			});
}

// GET /api/me/server — the logged-in account's hosted server: {"enabled" (this
// service runs hosted servers), "can_host" (an admin approved the account),
// "server"}, the server its live one, else its newest ended one, else null:
// {id, state, live, settings, restarts, failure_reason, created_at, updated_at,
// running_at, ended_at (UTC, null until set), listing ({rid, server_name,
// mission_name, players, max_players, host_ip, host_port} while listed, else
// null) and events, its newest kEventsShown transitions oldest first ({from,
// to, detail, at}; the request's own has from "")}. 401 with no live session.
crow::response HostedRoutes::me_server(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["enabled"] = enabled_;
		const auto user = get_user_by_id(*db_conn, access.user->id);
		out["can_host"] = user && user->can_host;
		if (const auto server = latest_server_of(*db_conn, access.user->id)) {
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
		} else {
			out["server"] = nullptr;
		}
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] GET /api/me/server failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// POST /api/me/server/start — the account's hosted server, started with the
// body's settings: {"game_name" (required), "max_players", "game_password",
// "time_limit", "kill_limit", "max_score", "respawn", "start_delay" (integers),
// "team_ff" (true/false), "expansion" ("" or "jox01"), "missions" (a list of
// .bms names, the first the starting map; absent: ONNET_HOSTED_MISSIONS)}, each
// absent one its default. In order: require_user (401, 403 without the CSRF
// header), 503 hosting_disabled when this service runs none, one token from the
// account's kStartStopPerUser bucket (429), 400 invalid_json / invalid_field (a
// mistyped field), 400 invalid_settings with the refused "field" and why
// (hosted_settings.h check_settings), then the request: 403 not_approved without
// the account's approval (players.can_host) or an active status, 409
// already_live with its "server_id" when it has a live server, 409 at_capacity
// when ONNET_HOSTED_CAP servers are live; 202 {"server"} (state requested) once
// written. The supervisor takes it from there.
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
	settings.missions = default_missions_;
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
	if (body->wrong_type()) return json_error(400, "invalid_field");
	if (const auto refused = check_settings(settings)) {
		crow::json::wvalue out;
		out["error"] = "invalid_settings";
		out["field"] = refused->field;
		out["message"] = refused->reason;
		log_owner(user, "start", "invalid_settings (" + refused->field + ")");
		return json_reply(400, out);
	}
	try {
		auto db_conn = pool_.acquire();
		const RequestResult result = request_server(*db_conn, user.id, settings, cap_);
		switch (result.outcome) {
			case RequestOutcome::Created:
				log_owner(user, "start", "server " + std::to_string(result.id) + " requested");
				return server_reply(202, *db_conn, result.id, false);
			case RequestOutcome::InvalidSettings: // check_settings refused them above already
				return json_error(400, "invalid_settings");
			case RequestOutcome::NoSuchUser:
			case RequestOutcome::NotApproved:
				log_owner(user, "start", "not_approved");
				return json_error(403, "not_approved",
						"an admin has not approved this account for a hosted server");
			case RequestOutcome::AlreadyLive: {
				log_owner(user, "start", "already_live");
				crow::json::wvalue out;
				out["error"] = "already_live";
				out["message"] = "this account's hosted server is running; stop it first";
				out["server_id"] = result.id;
				return json_reply(409, out);
			}
			case RequestOutcome::AtCapacity:
				log_owner(user, "start", "at_capacity");
				return json_error(409, "at_capacity", "every hosted server place is taken; try again later");
		}
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] POST /api/me/server/start failed: %s\n", e.what());
	}
	return json_error(500, "db_error");
}

// POST /api/me/server/stop — the account's live hosted server goes to stopping
// (the supervisor then asks it to stop and ends its machine). require_user
// (401, 403 without the CSRF header), the start's bucket (429), 404
// no_live_server, else 202 {"server"} (a server already stopping included).
crow::response HostedRoutes::me_stop(const crow::request &req) {
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
		const auto server = latest_server_of(*db_conn, user.id);
		if (!server || !is_live(server->state)) {
			log_owner(user, "stop", "no_live_server");
			return json_error(404, "no_live_server", "this account runs no hosted server");
		}
		const bool asked = request_stop(*db_conn, server->id, "stopped by its owner");
		log_owner(user, "stop", "server " + std::to_string(server->id) + (asked ? " stopping" : " already stopping"));
		return server_reply(202, *db_conn, server->id, false);
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] POST /api/me/server/stop failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// GET /api/admin/hosted — the fleet: {"enabled", "provider", "cap", "live" (how
// many are live), "servers"}, the newest kFleetShown, live ones first, each as
// GET /api/me/server's plus "owner" and "service_account" ({id, username} or
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
	} catch (const db::SqliteError &e) {
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
	} catch (const db::SqliteError &e) {
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
		const int64_t server_id = id <= static_cast<uint64_t>(INT64_MAX) ? static_cast<int64_t>(id) : 0;
		const auto server = find_server(*db_conn, server_id);
		if (!server) return json_error(404, "not_found", "no hosted server has this id");
		if (!is_live(server->state)) return json_error(409, "not_live", "this hosted server has ended");
		request_stop(*db_conn, server_id, "stopped by an admin");
		return server_reply(202, *db_conn, server_id, true);
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] POST /api/admin/hosted/%llu/stop failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
}

} // namespace opennova::novaworld_server::hosted
