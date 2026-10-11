#include "host_routes.h"

#include "auth.h"
#include "http_json.h"
#include "nw_udp_listener.h"

#include <base/io/cp1252.h>
#include <net/novaworld/host_repository.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
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

// Whether UTF-8 `text` holds a C1 control (U+0080..U+009F, 0xC2 then 0x80..0x9F).
bool has_c1_control(std::string_view text) {
	for (size_t i = 0; i + 1 < text.size(); ++i) {
		const auto lead = static_cast<uint8_t>(text[i]);
		const auto next = static_cast<uint8_t>(text[i + 1]);
		if (lead == 0xC2 && next >= 0x80 && next <= 0x9F)
			return true;
	}
	return false;
}

// "U+1F600" for each codepoint, comma-separated (the first four, then "and more").
std::string codepoint_names(const std::u32string &codepoints) {
	std::string out;
	for (size_t i = 0; i < codepoints.size() && i < 4; ++i) {
		char name[16];
		std::snprintf(name, sizeof(name), "U+%04X", static_cast<unsigned>(codepoints[i]));
		if (!out.empty())
			out += ", ";
		out += name;
	}
	if (codepoints.size() > 4)
		out += " and more";
	return out;
}

// A listed server for its owner (GET /api/me/hosts) or an admin (GET
// /api/admin/hosts): its browser columns, the endpoint joiners are sent to and
// the address its session comes from, and the row's UTC times. The public
// /api/lobbies and /api/hosts name no owner. A host's text comes in its own code
// page (a stock host's cp1252), so it goes out as UTF-8 (retail_text_to_utf8).
crow::json::wvalue listed_host_json(const hostdb::HostRow &h) {
	crow::json::wvalue e;
	e["rid"] = h.rid;
	e["gsid"] = h.gsid;
	e["game"] = h.game;
	e["server_name"] = retail_text_to_utf8(h.server_name);
	e["mission_name"] = retail_text_to_utf8(h.mission_name);
	e["game_type"] = retail_text_to_utf8(h.game_type);
	e["players"] = h.player_count;
	e["max_players"] = h.max_players;
	e["host_ip"] = h.host_ip;
	e["host_port"] = h.host_port;
	e["peer_ip"] = h.peer_ip;
	e["created_at"] = h.created_at;
	e["updated_at"] = h.updated_at;
	return e;
}

// A composed Cmd line (the host's cp1252 bytes) as UTF-8, for a reply or a log.
std::string line_text(const std::string &cmd) {
	return cp1252_to_utf8(cmd);
}

// One line per owner push, whatever its answer: the account, the RID, the
// composed line (as UTF-8, control bytes escaped, whole: the composer caps it
// at 511 bytes), and the outcome.
void log_owner_push(const WebSessionUser &user, uint64_t rid, const char *what,
		const std::string &line, const char *outcome) {
	const std::string text = line_text(line);
	const std::string shown =
			text.empty() ? std::string() : " '" + loggable(text, text.size()) + "'";
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
	// Each arg in the host's code page, the bytes its reader keeps.
	std::vector<std::string> host_args;
	host_args.reserve(args.size());
	for (const std::string &arg : args) {
		if (has_control_byte(arg) || has_c1_control(arg)) {
			return refused("invalid_command",
					"an arg holds a control byte (below 0x20, 0x7F, or a C1 control U+0080 to "
					"U+009F), which a stock host would carry into its game.cfg or its chat");
		}
		std::string bytes;
		std::u32string unstorable;
		if (!utf8_to_cp1252(arg, bytes, &unstorable)) {
			if (!is_valid_utf8(arg))
				return refused("invalid_command", "an arg is not UTF-8");
			return refused("invalid_command",
					"an arg holds " + codepoint_names(unstorable) +
							", which the host's code page (cp1252) has no character for");
		}
		host_args.push_back(std::move(bytes));
	}
	// The widths a stock host keeps of a name and a message, counted in its own bytes.
	if (!host_args.empty()) {
		const size_t kept = out.verb == ServerCommandVerb::SetServerName
				? SERVER_COMMAND_NAME_CAP - 1
				: out.verb == ServerCommandVerb::SetServerMsg ? SERVER_COMMAND_MSG_CAP - 1
															  : 0;
		if (kept != 0 && host_args[0].size() > kept) {
			return refused("invalid_command",
					"the text is " + std::to_string(host_args[0].size()) +
							" characters in the host's code page (cp1252); a stock host keeps " +
							std::to_string(kept));
		}
	}
	const char *refusal = nullptr;
	out.cmd = server_command_text(out.verb, target, host_args, &refusal);
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
	// The whole line (server_command_text caps it at 511 bytes) as UTF-8, control bytes escaped.
	const std::string text = line_text(command.cmd);
	std::printf("[http] POST /api/admin/hosts/%llu/command '%s' -> %s\n",
			static_cast<unsigned long long>(rid), loggable(text, text.size()).c_str(),
			host_push_result_name(result));
	crow::json::wvalue out;
	out["statement"] = "ServerCommand";
	out["cmd"] = text;
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
// snapshot so no roster goes out beside another commit's row. A listing whose
// row still names the account but whose claim is gone (a reset, a ban or a
// delete since its last write) is left out, as the push routes refuse it.
crow::response HostRoutes::owner_hosts(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal)
		return std::move(*access.refusal);
	std::vector<crow::json::wvalue> hosts;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		for (const auto &h : hostdb::list_hosts_by_owner(*db_conn, access.user->id)) {
			if (!hostdb::listing_owned_by(*db_conn, h.rid, access.user->id))
				continue;
			crow::json::wvalue e = listed_host_json(h);
			e["msg"] = retail_text_to_utf8(h.msg);
			std::vector<crow::json::wvalue> roster;
			for (const auto &s : hostdb::list_roster(*db_conn, h.rid)) {
				crow::json::wvalue r;
				r["slot"] = s.slot;
				r["player_name"] = retail_text_to_utf8(s.player_name);
				r["team"] = retail_text_to_utf8(s.team);
				r["type"] = retail_text_to_utf8(s.type);
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
// HostKey's live claim, which active_hosts.host_user_id follows) pushes to that
// server alone. In order: require_user (401 with no live session of an active
// account, 403 without the CSRF header), then one token from the account's
// kOwnerPushPerUser bucket (429 with Retry-After), then the body (400,
// compose_server_command), then the verb (403 verb_not_allowed:
// owner_may_send), then the listing: 404 unless the RID is listed with one of
// the caller's keys claimed for it (hostdb::listing_owned_by), the same reply
// for a free RID and another account's; then the push, scoped to the caller,
// which the listener checks against the connection's own owner under its
// lobby lock and again as the statement goes out (a changed owner is 404
// too); 409 for the caller's listing whose connection is not hosting. Every
// request past require_user is logged with the account, the RID and the
// outcome.
template <typename Push>
crow::response HostRoutes::push_as_owner(const WebSessionUser &user, uint64_t rid, const char *what,
		const std::string &line, Push push, crow::json::wvalue out) {
	const uint32_t rid32 = rid_of(rid);
	bool owned = false;
	try {
		owned = hostdb::listing_owned_by(*pool_.acquire(), rid32, user.id);
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
				"SetMPReset is an admin's verb: it would end the server's process the next "
				"time it creates a session (a map change creates none), and it stays saved "
				"in its game.cfg");
	}
	crow::json::wvalue out;
	out["statement"] = "ServerCommand";
	out["cmd"] = line_text(command.cmd);
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
