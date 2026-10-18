#include "connection_routes.h"

#include "auth.h"
#include "core/loggable.h"
#include "host_routes.h"
#include "http_json.h"

#include <base/io/cp1252.h>
#include <net/novaworld/lobby_repository.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

crow::json::wvalue account_json(const UserRecord &user, const char *via) {
	crow::json::wvalue a;
	a["id"] = user.id;
	a["username"] = user.username;
	a["via"] = via;
	return a;
}

} // namespace

void ConnectionRoutes::register_routes(WebApp &app, uint64_t heartbeat_timeout_ms) {
	heartbeat_timeout_ms_ = heartbeat_timeout_ms;
	CROW_ROUTE(app, "/api/admin/connections")
			.methods("GET"_method)([this](const crow::request &req) { return list(req); });
	CROW_ROUTE(app, "/api/admin/connections/<uint>/end")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return end(req, id); });
	CROW_ROUTE(app, "/api/admin/lobbies").methods("GET"_method)([this](const crow::request &req) {
		return lobbies(req);
	});
	CROW_ROUTE(app, "/api/admin/lobbies/<uint>/drain")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return drain(req, id); });
}

// GET /api/admin/connections — {"count", "heartbeat_timeout_ms", "connections"}:
// each session's row id (session_id: what the end route takes, unique across
// the instances), the lobby instance holding it (lobby_id), its registry id
// (id: unique within its instance alone), state, the protocol name (pn) and NA its hello
// and auth carried (identity), its address, and when it connected and was last
// heard from (created_ms, last_seen_ms; the lobby writes the last within ten
// seconds) as Unix epoch ms; "account", who the session is, and "host", the
// server it lists. The account is the listing's owner while it hosts
// (listing_owner, "via": "host_key"), else the account whose live retail login
// its latest Cookie list named (the row's login_session_tag, "via": "login"),
// else null. The host is null unless it hosts: {rid, server_name, mission_name
// (UTF-8, as every list of listed servers sends a host's text), players,
// max_players, listed (false while it waits out of the browser for a HostKey
// naming an account), hosted_server_id (null for a listing that is no live
// hosted server)}. All of it from the lobby's session rows, in one snapshot.
crow::response ConnectionRoutes::list(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::vector<crow::json::wvalue> entries;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot read(*db_conn);
		for (const lobbydb::SessionRow &s : lobbydb::list_sessions(*db_conn)) {
			crow::json::wvalue e;
			e["session_id"] = s.id;
			e["lobby_id"] = s.lobby_id;
			e["id"] = s.connection_id;
			e["state"] = s.state;
			e["pn"] = s.pn;
			e["identity"] = s.identity;
			e["created_ms"] = s.connected_ms;
			e["last_seen_ms"] = s.last_seen_ms;
			e["addr"] = lobbydb::session_addr(s);
			e["account"] = nullptr;
			e["host"] = nullptr;
			if (s.hosting) {
				const ListingOwner listing = listing_owner(*db_conn, s.host_user_id);
				crow::json::wvalue host;
				host["rid"] = s.rid;
				host["server_name"] = retail_text_to_utf8(s.server_name);
				host["mission_name"] = retail_text_to_utf8(s.mission_name);
				host["players"] = s.players;
				host["max_players"] = s.max_players;
				host["listed"] = s.listed;
				if (listing.hosted_server_id)
					host["hosted_server_id"] = *listing.hosted_server_id;
				else
					host["hosted_server_id"] = nullptr;
				e["host"] = std::move(host);
				if (listing.owner)
					e["account"] = account_json(*listing.owner, "host_key");
			}
			if (e["account"].t() == crow::json::type::Null && !s.login_session_tag.empty()) {
				if (const auto user = get_user_by_login_session_tag(*db_conn, s.login_session_tag))
					e["account"] = account_json(*user, "login");
			}
			entries.push_back(std::move(e));
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/connections failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["count"] = entries.size();
	out["heartbeat_timeout_ms"] = heartbeat_timeout_ms_;
	out["connections"] = std::move(entries);
	return json_reply(200, out);
}

// POST /api/admin/connections/<session id>/end — the session the admin's list
// names by its row's id (session_id: unique across the lobby instances and never
// reused, where a registry id and an address are one instance's alone). No
// body. 202 {"status": "queued", "session_id", "id", "addr"} once the end is
// queued for the lobby instance holding the session
// (lobbydb::queue_end_session); 404 not_found for a session that ended; 409
// hosting (with the listing's "rid": POST /api/admin/hosts/<rid>/stop ends it).
// Admin-gated like every /api/admin route; AccessMiddleware logs it with its
// actor and status, and this line names the session.
crow::response ConnectionRoutes::end(const crow::request &req, uint64_t session_id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	lobbydb::EndSession ended;
	if (session_id <= static_cast<uint64_t>(INT64_MAX)) {
		try {
			ended = lobbydb::queue_end_session(*pool_.acquire(), static_cast<int64_t>(session_id));
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] POST /api/admin/connections/%llu/end failed: %s\n",
					static_cast<unsigned long long>(session_id), e.what());
			return json_error(500, "db_error");
		}
	}
	const char *outcome = lobbydb::end_session_result_name(ended.result);
	if (ended.result == lobbydb::EndSession::Result::NotFound)
		return json_error(404, outcome, "no NovaWorld session has this id");
	const lobbydb::SessionRow &s = ended.session;
	const std::string at = lobbydb::session_addr(s);
	std::printf("[http] admin ends NovaWorld session %lld (lobby %lld) id=%u addr=%s identity='%s' "
				"-> %s\n",
			static_cast<long long>(s.id), static_cast<long long>(s.lobby_id), s.connection_id,
			at.c_str(), loggable(s.identity).c_str(), outcome);
	crow::json::wvalue out;
	if (ended.result == lobbydb::EndSession::Result::Hosting) {
		out["error"] = outcome;
		out["message"] = "the session hosts a listed server: stop the server instead";
		out["rid"] = s.rid;
		return json_reply(409, out);
	}
	out["status"] = outcome;
	out["session_id"] = s.id;
	out["id"] = s.connection_id;
	out["addr"] = at;
	return json_reply(202, out);
}

// GET /api/admin/lobbies — {"lobbies"}: every lobby instance (one per lobby
// process), by id: its endpoint (the gate's udpnovaworld, the lobby address
// every instance announces), its state ("active" counts as taking clients,
// "draining" does not), when it
// started and was last seen (Unix epoch ms; a running lobby moves it every few
// seconds, and one unseen for five minutes is reaped with its rows), and its sessions.
crow::response ConnectionRoutes::lobbies(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::vector<crow::json::wvalue> entries;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot read(*db_conn);
		std::map<int64_t, int64_t> sessions;
		for (const auto &r : db_conn->query("SELECT lobby_id, COUNT(*) FROM lobby_sessions "
											"WHERE lobby_id IS NOT NULL GROUP BY lobby_id;"))
			sessions[r.as_int(0).value_or(0)] = r.as_int(1).value_or(0);
		for (const lobbydb::LobbyInstance &i : lobbydb::list_instances(*db_conn)) {
			crow::json::wvalue e;
			e["id"] = i.id;
			e["endpoint"] = i.endpoint;
			e["state"] = i.state;
			e["started_ms"] = i.started_ms;
			e["last_seen_ms"] = i.last_seen_ms;
			const auto it = sessions.find(i.id);
			e["sessions"] = it == sessions.end() ? int64_t{ 0 } : it->second;
			entries.push_back(std::move(e));
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/lobbies failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["lobbies"] = std::move(entries);
	return json_reply(200, out);
}

// POST /api/admin/lobbies/<id>/drain — no gate counts the instance as taking
// clients from the gates' next read on (a few seconds: they hand the lobby
// address while another instance does, and no one with none); the sessions it
// holds stay until they leave. Which lobby task a client reaches is the load
// balancer's, which a task leaves by its stop (its target's deregistration).
// 200 {"status": "draining", "id"}; 404 not_found for no such instance.
crow::response ConnectionRoutes::drain(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	bool drained = false;
	if (id <= static_cast<uint64_t>(INT64_MAX)) {
		try {
			drained = lobbydb::set_instance_state(
					*pool_.acquire(), static_cast<int64_t>(id), "draining");
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] POST /api/admin/lobbies/%llu/drain failed: %s\n",
					static_cast<unsigned long long>(id), e.what());
			return json_error(500, "db_error");
		}
	}
	if (!drained)
		return json_error(404, "not_found", "no lobby instance has this id");
	std::printf("[http] admin drains lobby instance %llu\n", static_cast<unsigned long long>(id));
	crow::json::wvalue out;
	out["status"] = "draining";
	out["id"] = static_cast<int64_t>(id);
	return json_reply(200, out);
}

} // namespace opennova::novaworld_server
