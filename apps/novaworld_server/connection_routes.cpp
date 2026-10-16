#include "connection_routes.h"

#include "auth.h"
#include "host_routes.h"
#include "http_json.h"
#include "loggable.h"
#include "nw_udp_listener.h"

#include <base/io/cp1252.h>
#include <net/novaworld/connection/manager.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

const char *connection_state_name(ConnectionState s) {
	switch (s) {
		case ConnectionState::Handshaking:
			return "handshaking";
		case ConnectionState::Active:
			return "active";
		case ConnectionState::Closing:
			return "closing";
	}
	return "unknown";
}

crow::json::wvalue account_json(const UserRecord &user, const char *via) {
	crow::json::wvalue a;
	a["id"] = user.id;
	a["username"] = user.username;
	a["via"] = via;
	return a;
}

} // namespace

int64_t unix_ms_of_steady(uint64_t steady_ms, uint64_t steady_now_ms, int64_t unix_now_ms) {
	const uint64_t age = steady_now_ms > steady_ms ? steady_now_ms - steady_ms : 0;
	return unix_now_ms - static_cast<int64_t>(age);
}

void ConnectionRoutes::register_routes(
		WebApp &app, ConnectionManager &manager, NwUdpListener *nw_udp) {
	manager_ = &manager;
	nw_udp_ = nw_udp;
	CROW_ROUTE(app, "/api/admin/connections")
			.methods("GET"_method)([this](const crow::request &req) { return list(req); });
	CROW_ROUTE(app, "/api/admin/connections/<uint>/end")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return end(req, id); });
}

// GET /api/admin/connections — {"count", "heartbeat_timeout_ms", "connections"}:
// each connection's registry id, state, the protocol name (pn) and NA its
// hello and auth carried (identity), its address, and when it connected and
// was last heard from (created_ms, last_seen_ms) as Unix epoch ms; "account",
// who the session is, and "host", the server it lists, from its lobby state.
// The account is the listing's owner while it hosts (listing_owner, "via":
// "host_key"), else the account whose live retail login its latest Cookie list
// named (LobbyState::login_session_tag, "via": "login"), else null. The host
// is null unless it hosts: {rid, server_name, mission_name (UTF-8, as every
// list of listed servers sends a host's text), players, max_players, listed
// (false while it waits out of the browser for a HostKey naming an account),
// hosted_server_id (null for a listing that is no live hosted server)}.
crow::response ConnectionRoutes::list(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	using namespace std::chrono;
	// The registry's clock and the wall clock, read together.
	const uint64_t steady_now = static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
	const int64_t unix_now = static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
	const auto snapshot = manager_->registry().snapshot();
	std::unordered_map<PeerAddr, LobbyState, PeerAddrHash> lobbies;
	if (nw_udp_ != nullptr) {
		for (auto &l : nw_udp_->snapshot_lobby())
			lobbies.emplace(l.peer, std::move(l.lobby));
	}
	std::vector<crow::json::wvalue> entries;
	entries.reserve(snapshot.size());
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot read(*db_conn);
		for (const auto &c : snapshot) {
			crow::json::wvalue e;
			e["id"] = c.id;
			e["state"] = connection_state_name(c.state);
			e["pn"] = c.pn;
			e["identity"] = c.identity;
			e["created_ms"] = unix_ms_of_steady(c.created_ms, steady_now, unix_now);
			e["last_seen_ms"] = unix_ms_of_steady(c.last_seen_ms, steady_now, unix_now);
			e["addr"] = peer_addr_to_string(c.addr);
			e["account"] = nullptr;
			e["host"] = nullptr;
			const auto lobby = lobbies.find(c.addr);
			if (lobby != lobbies.end()) {
				const LobbyState &l = lobby->second;
				if (l.hosting) {
					const ListingOwner listing = listing_owner(*db_conn, l.host_user_id);
					crow::json::wvalue host;
					host["rid"] = l.rid;
					host["server_name"] = retail_text_to_utf8(l.server_name);
					host["mission_name"] = retail_text_to_utf8(l.mission_name);
					host["players"] = l.player_count;
					host["max_players"] = l.max_players;
					host["listed"] = !l.awaiting_host_key;
					if (listing.hosted_server_id)
						host["hosted_server_id"] = *listing.hosted_server_id;
					else
						host["hosted_server_id"] = nullptr;
					e["host"] = std::move(host);
					if (listing.owner)
						e["account"] = account_json(*listing.owner, "host_key");
				}
				if (e["account"].t() == crow::json::type::Null && !l.login_session_tag.empty()) {
					if (const auto user =
									get_user_by_login_session_tag(*db_conn, l.login_session_tag))
						e["account"] = account_json(*user, "login");
				}
			}
			entries.push_back(std::move(e));
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/connections failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["count"] = snapshot.size();
	out["heartbeat_timeout_ms"] = manager_->heartbeat_timeout_ms();
	out["connections"] = std::move(entries);
	return json_reply(200, out);
}

// POST /api/admin/connections/<id>/end — {"addr": "<ip:port>"}, the address the
// admin's list showed beside the id (a client-supplied CI can be another
// session's id once the first ends, G.7, so the pair names the session). 202
// {"status": "queued", "id", "addr"} once the receive thread has it to end
// (NwUdpListener::end_session); 400 invalid_json, invalid_field, or
// missing_field without the address; 404 not_found for an id no connection
// holds; 409 connection_changed when the id names another address now, hosting
// (with the listing's "rid": POST /api/admin/hosts/<rid>/stop ends it) and
// not_lobby (a game connection, not a NovaWorld session); 503 with no NW UDP
// listener. Admin-gated like every /api/admin route; AccessMiddleware logs it
// with its actor and status, and this line names the session.
crow::response ConnectionRoutes::end(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const std::optional<std::string> addr = body->text("addr");
	if (body->wrong_type())
		return json_error(400, "invalid_field");
	if (!addr || addr->empty())
		return json_error(400, "missing_field", "addr is required");
	if (nw_udp_ == nullptr)
		return json_error(503, "no_session_listener", "the NovaWorld UDP listener is not wired");
	std::optional<Connection> conn;
	if (id <= UINT32_MAX)
		conn = manager_->registry().find(static_cast<uint32_t>(id));
	if (!conn)
		return json_error(404, "not_found", "no NovaWorld session has this id");
	const std::string at = peer_addr_to_string(conn->addr);
	if (at != *addr)
		return json_error(409, "connection_changed", "this id names another session now");
	uint32_t rid = 0;
	const EndSessionResult result = nw_udp_->end_session(conn->addr, &rid);
	std::printf("[http] admin ends NovaWorld session id=%u addr=%s identity='%s' -> %s\n", conn->id,
			at.c_str(), loggable(conn->identity).c_str(), end_session_result_name(result));
	switch (result) {
		case EndSessionResult::Queued: {
			crow::json::wvalue out;
			out["status"] = end_session_result_name(result);
			out["id"] = conn->id;
			out["addr"] = at;
			return json_reply(202, out);
		}
		case EndSessionResult::Hosting: {
			crow::json::wvalue out;
			out["error"] = end_session_result_name(result);
			out["message"] = "the session hosts a listed server: stop the server instead";
			out["rid"] = rid;
			return json_reply(409, out);
		}
		case EndSessionResult::NotLobby:
			break;
	}
	return json_error(409, end_session_result_name(EndSessionResult::NotLobby),
			"a game connection, not a NovaWorld session");
}

} // namespace opennova::novaworld_server
