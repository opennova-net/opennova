#include "host_routes.h"

#include "auth.h"
#include "http_json.h"
#include "nw_udp_listener.h"

#include <net/novaworld/host_repository.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

// The 404 messages: the admin's, for a RID no live connection holds, and the
// owner's, the same reply for a free RID and for another account's listing.
constexpr const char *kNoConnection = "no live NovaWorld connection holds this RID";
constexpr const char *kNotYours = "no server of yours is listed under this RID";

uint32_t rid_of(uint64_t rid) {
	return rid <= UINT32_MAX ? static_cast<uint32_t>(rid) : 0u;
}

crow::response no_listener() {
	return json_error(503, "no_session_listener", "the NovaWorld UDP listener is not wired");
}

// A push's reply: 202 once queued for the receive thread, 409 for a RID whose
// connection is not hosting, 404 (`unknown`) for one no live connection holds.
crow::response push_reply(
		HostPushResult result, uint32_t rid, crow::json::wvalue out, const char *unknown) {
	switch (result) {
		case HostPushResult::Queued:
			out["rid"] = rid;
			out["status"] = host_push_result_name(result);
			return json_reply(202, out);
		case HostPushResult::NotHosted:
			return json_error(409, host_push_result_name(result),
					"the connection holding this RID is not hosting");
		case HostPushResult::UnknownRid:
			break;
	}
	return json_error(404, host_push_result_name(HostPushResult::UnknownRid), unknown);
}

// A listed server for its owner (GET /api/me/hosts) or an admin (GET
// /api/admin/hosts): its browser columns, the endpoint joiners are sent to and
// the address its session comes from, and the row's UTC times. The public
// /api/lobbies and /api/hosts name no owner.
crow::json::wvalue listed_host_json(const hostdb::HostRow &h) {
	crow::json::wvalue e;
	e["rid"] = h.rid;
	e["gsid"] = h.gsid;
	e["game"] = h.game;
	e["server_name"] = h.server_name;
	e["mission_name"] = h.mission_name;
	e["game_type"] = h.game_type;
	e["players"] = h.player_count;
	e["max_players"] = h.max_players;
	e["host_ip"] = h.host_ip;
	e["host_port"] = h.host_port;
	e["peer_ip"] = h.peer_ip;
	e["created_at"] = h.created_at;
	e["updated_at"] = h.updated_at;
	return e;
}

// One line per owner push, whatever its answer: the account, the RID, the
// composed line (control bytes escaped, whole: the composer caps it at 511),
// and the outcome.
void log_owner_push(const WebSessionUser &user, uint64_t rid, const char *what,
		const std::string &line, const char *outcome) {
	const std::string shown =
			line.empty() ? std::string() : " '" + loggable(line, line.size()) + "'";
	std::printf("[http] owner '%s' (id=%lld) %s rid=%llu%s -> %s\n",
			loggable(user.username).c_str(), static_cast<long long>(user.id), what,
			static_cast<unsigned long long>(rid), shown.c_str(), outcome);
}

ComposedServerCommand refused(const char *error, const std::string &message) {
	ComposedServerCommand out;
	out.refusal = json_error(400, error, message);
	return out;
}

} // namespace

ComposedServerCommand compose_server_command(const std::string &text) {
	auto body = JsonBody::parse(text);
	if (!body)
		return refused("invalid_json", "");
	const auto verb_name = body->text("verb");
	if (!verb_name)
		return refused("invalid_command", "\"verb\" (a string) is required");
	ComposedServerCommand out;
	if (!server_command_verb_from_name(*verb_name, out.verb)) {
		return refused("invalid_command", "\"verb\" names no ServerCommand verb");
	}
	ServerCommandTarget target = ServerCommandTarget::None;
	const auto target_name = body->text("target");
	if (body->wrong_type() ||
			(target_name && !server_command_target_from_name(*target_name, target))) {
		return refused(
				"invalid_command", "\"target\" is None, ByIndex, ByIpAndPort, ByName or ByPCID");
	}
	const std::vector<std::string> args =
			body->text_list("args").value_or(std::vector<std::string>{});
	if (body->wrong_type())
		return refused("invalid_command", "\"args\" is a list of strings");
	for (const std::string &arg : args) {
		if (has_control_byte(arg)) {
			return refused("invalid_command",
					"an arg holds a control byte (below 0x20, or 0x7F), which a stock host "
					"would carry into its game.cfg or its chat");
		}
	}
	const char *refusal = nullptr;
	out.cmd = server_command_text(out.verb, target, args, &refusal);
	if (out.cmd.empty())
		return refused("invalid_command", refusal != nullptr ? refusal : "");
	return out;
}

bool owner_may_send(ServerCommandVerb verb) {
	return verb != ServerCommandVerb::None && verb != ServerCommandVerb::SetMPReset;
}

void HostRoutes::register_routes(WebApp &app, NwUdpListener *nw_udp) {
	nw_udp_ = nw_udp;
	CROW_ROUTE(app, "/api/admin/hosts").methods("GET"_method)([this](const crow::request &req) {
		return admin_hosts(req);
	});
	CROW_ROUTE(app, "/api/admin/hosts/<uint>/command")
			.methods("POST"_method)([this](const crow::request &req, uint64_t rid) {
				return admin_command(req, rid);
			});
	CROW_ROUTE(app, "/api/admin/hosts/<uint>/stop")
			.methods("POST"_method)([this](const crow::request &req, uint64_t rid) {
				return admin_stop(req, rid);
			});
	CROW_ROUTE(app, "/api/me/hosts")([this](const crow::request &req) { return owner_hosts(req); });
	CROW_ROUTE(app, "/api/me/hosts/<uint>/command")
			.methods("POST"_method)([this](const crow::request &req, uint64_t rid) {
				return owner_command(req, rid);
			});
	CROW_ROUTE(app, "/api/me/hosts/<uint>/stop")
			.methods("POST"_method)([this](const crow::request &req, uint64_t rid) {
				return owner_stop(req, rid);
			});
}

// GET /api/admin/hosts — every listed server with its owner: listed_host_json
// plus "owner", {id, username} of the account whose HostKey listed it, or
// null for an anonymous listing (the HostKey named no account).
crow::response HostRoutes::admin_hosts(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::vector<crow::json::wvalue> entries;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		std::map<int64_t, std::optional<UserRecord>> owners;
		for (const auto &h : hostdb::list_hosts(*db_conn)) {
			crow::json::wvalue e = listed_host_json(h);
			std::optional<UserRecord> owner;
			if (h.host_user_id) {
				auto it = owners.find(*h.host_user_id);
				if (it == owners.end()) {
					it = owners.emplace(*h.host_user_id, get_user_by_id(*db_conn, *h.host_user_id))
								 .first;
				}
				owner = it->second;
			}
			if (owner) {
				crow::json::wvalue o;
				o["id"] = owner->id;
				o["username"] = owner->username;
				e["owner"] = std::move(o);
			} else {
				e["owner"] = nullptr;
			}
			entries.push_back(std::move(e));
		}
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] GET /api/admin/hosts failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["count"] = entries.size();
	out["hosts"] = std::move(entries);
	return json_reply(200, out);
}

// The service's statements to a listed server, pushed over that server's own
// NovaWorld UDP session (docs/net/novaworld-net-re.md "the service side").
// The admin pushes: 202 once queued for the receive thread, 400 for a body
// compose_server_command refuses (with its reason), 404 for a RID no live
// connection holds, 409 for one whose connection is not hosting, 503 when no
// NW UDP listener is wired. Admin-gated (require_admin: the Bearer token, or an
// admin session with the CSRF header); AccessMiddleware logs each with its
// actor, and the push's own line logs what went out.

// POST /api/admin/hosts/<rid>/command — a ServerCommand, any of the eighteen
// verbs. Body JSON:
//   {"verb": "SetServerName", "target": "None|ByIndex|ByIpAndPort|ByName|ByPCID",
//    "args": ["..."]}
crow::response HostRoutes::admin_command(const crow::request &req, uint64_t rid) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	ComposedServerCommand command = compose_server_command(req.body);
	if (command.refusal)
		return std::move(*command.refusal);
	if (nw_udp_ == nullptr)
		return no_listener();
	const uint32_t rid32 = rid_of(rid);
	const HostPushResult result = nw_udp_->push_server_command(rid32, command.cmd);
	// The whole line (server_command_text caps it at 511), control bytes escaped.
	std::printf("[http] POST /api/admin/hosts/%llu/command '%s' -> %s\n",
			static_cast<unsigned long long>(rid), loggable(command.cmd, command.cmd.size()).c_str(),
			host_push_result_name(result));
	crow::json::wvalue out;
	out["statement"] = "ServerCommand";
	out["cmd"] = command.cmd;
	return push_reply(result, rid32, std::move(out), kNoConnection);
}

// POST /api/admin/hosts/<rid>/stop — a ServerStopHosting with MsgCode 7,
// NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT (SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT;
// which code the retail service sent is unwitnessed): the sysop punt. A stock
// host in a NovaWorld match ends that match for everyone within 62 frames,
// goes to the main menu and resets its NovaWorld session
// (NwUdpListener::push_stop_hosting; the record's "The exit"); the server
// leaves the browser as the statement goes out. No body.
crow::response HostRoutes::admin_stop(const crow::request &req, uint64_t rid) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	if (nw_udp_ == nullptr)
		return no_listener();
	const uint32_t rid32 = rid_of(rid);
	const HostPushResult result =
			nw_udp_->push_stop_hosting(rid32, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT);
	std::printf("[http] POST /api/admin/hosts/%llu/stop -> %s\n",
			static_cast<unsigned long long>(rid), host_push_result_name(result));
	crow::json::wvalue out;
	out["statement"] = "ServerStopHosting";
	out["msg_code"] = SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT;
	return push_reply(result, rid32, std::move(out), kNoConnection);
}

// GET /api/me/hosts — the servers the logged-in account has listed now (each
// with a HostKey its client's /NWHost.dll fetch minted for it), oldest first,
// or 401: {"hosts": [...]}, each listed_host_json plus "msg" (the server's
// message) and "roster", the host's PlayerList ({slot, player_name, team,
// type}, the slot being what a player verb's ByIndex target names), read in one
// snapshot so no roster goes out beside another commit's row.
crow::response HostRoutes::owner_hosts(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal)
		return std::move(*access.refusal);
	std::vector<crow::json::wvalue> hosts;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		for (const auto &h : hostdb::list_hosts_by_owner(*db_conn, access.user->id)) {
			crow::json::wvalue e = listed_host_json(h);
			e["msg"] = h.msg;
			std::vector<crow::json::wvalue> roster;
			for (const auto &s : hostdb::list_roster(*db_conn, h.rid)) {
				crow::json::wvalue r;
				r["slot"] = s.slot;
				r["player_name"] = s.player_name;
				r["team"] = s.team;
				r["type"] = s.type;
				roster.push_back(std::move(r));
			}
			e["roster"] = std::move(roster);
			hosts.push_back(std::move(e));
		}
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] GET /api/me/hosts failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["hosts"] = std::move(hosts);
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// The owner pushes (service policy): the account that listed a server (its
// HostKey, active_hosts.host_user_id) pushes to that server alone. In order:
// require_user (401 with no live session of an active account, 403 without
// the CSRF header), then one token from the account's kOwnerPushPerUser bucket
// (429 with Retry-After), then the body (400, compose_server_command), then
// the verb (403 verb_not_allowed: owner_may_send), then the listing: 404 unless
// the row under the RID names the caller, the same reply for a free RID and
// another account's; then the push, scoped to the caller, which the listener
// checks under its lobby lock and again as the statement goes out (a changed
// owner is 404 too); 409 for the caller's listing whose connection is not
// hosting. Every request past require_user is logged with the account, the RID
// and the outcome.
template <typename Push>
crow::response HostRoutes::push_as_owner(const WebSessionUser &user, uint64_t rid, const char *what,
		const std::string &line, Push push, crow::json::wvalue out) {
	const uint32_t rid32 = rid_of(rid);
	bool owned = false;
	try {
		const auto row = hostdb::find_host_by_rid(*pool_.acquire(), rid32);
		owned = row && row->host_user_id == user.id;
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] owner %s lookup failed: %s\n", what, e.what());
		return json_error(500, "db_error");
	}
	if (!owned) {
		// Logged apart from the listener's own unknown_rid (a connection the caller does not own,
		// or none, at push time); the reply is the same.
		log_owner_push(user, rid, what, line, "no_listing_of_theirs");
		return push_reply(HostPushResult::UnknownRid, rid32, std::move(out), kNotYours);
	}
	if (nw_udp_ == nullptr)
		return no_listener();
	const HostPushResult result = push(rid32);
	log_owner_push(user, rid, what, line, host_push_result_name(result));
	return push_reply(result, rid32, std::move(out), kNotYours);
}

// POST /api/me/hosts/<rid>/command — the admin command's body and reply.
crow::response HostRoutes::owner_command(const crow::request &req, uint64_t rid) {
	auto access = access_.require_user(req);
	if (access.refusal)
		return std::move(*access.refusal);
	const WebSessionUser &user = *access.user;
	if (const int64_t wait = owner_push_per_user_.take(
				"owner|" + std::to_string(user.id), RateLimiter::Clock::now())) {
		log_owner_push(user, rid, "command", "", "rate_limited");
		return too_many_requests(wait);
	}
	ComposedServerCommand command = compose_server_command(req.body);
	if (command.refusal) {
		log_owner_push(user, rid, "command", "", "invalid_command");
		return std::move(*command.refusal);
	}
	if (!owner_may_send(command.verb)) {
		log_owner_push(user, rid, "command", command.cmd, "verb_not_allowed");
		return json_error(403, "verb_not_allowed",
				"SetMPReset is an admin's verb: it would end the server's process at its "
				"next map and stay saved in its game.cfg");
	}
	crow::json::wvalue out;
	out["statement"] = "ServerCommand";
	out["cmd"] = command.cmd;
	return push_as_owner(
			user, rid, "command", command.cmd,
			[this, &command, &user](uint32_t rid32) {
				return nw_udp_->push_server_command(rid32, command.cmd, user.id);
			},
			std::move(out));
}

// POST /api/me/hosts/<rid>/stop — the admin stop's sysop punt (MsgCode 7): a
// stock host ends its running match for everyone on the server and leaves
// NovaWorld. No body.
crow::response HostRoutes::owner_stop(const crow::request &req, uint64_t rid) {
	auto access = access_.require_user(req);
	if (access.refusal)
		return std::move(*access.refusal);
	const WebSessionUser &user = *access.user;
	if (const int64_t wait = owner_push_per_user_.take(
				"owner|" + std::to_string(user.id), RateLimiter::Clock::now())) {
		log_owner_push(user, rid, "stop", "", "rate_limited");
		return too_many_requests(wait);
	}
	crow::json::wvalue out;
	out["statement"] = "ServerStopHosting";
	out["msg_code"] = SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT;
	return push_as_owner(
			user, rid, "stop", "",
			[this, &user](uint32_t rid32) {
				return nw_udp_->push_stop_hosting(
						rid32, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT, 0, 0, user.id);
			},
			std::move(out));
}

} // namespace opennova::novaworld_server
