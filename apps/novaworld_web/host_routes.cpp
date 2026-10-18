#include "host_routes.h"

#include "auth.h"
#include "core/loggable.h"
#include "hosted/hosted_repository.h"
#include "http_json.h"

#include <base/io/cp1252.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

using lobbydb::PushResult;
using lobbydb::push_result_name;

// The 404 messages: the admin's, for a RID no live connection holds, and the
// owner's, the same reply for a free RID and for another account's listing.
constexpr const char *kNoConnection = "no live NovaWorld connection holds this RID";
constexpr const char *kNotYours = "no server of yours is listed under this RID";

uint32_t rid_of(uint64_t rid) {
	return rid <= UINT32_MAX ? static_cast<uint32_t>(rid) : 0u;
}

// A push's reply: 202 once queued for the lobby, 409 for a RID whose
// connection is not hosting, 404 (`unknown`) for one no live connection holds.
crow::response push_reply(
		PushResult result, uint32_t rid, crow::json::wvalue out, const char *unknown) {
	switch (result) {
		case PushResult::Queued:
			out["rid"] = rid;
			out["status"] = push_result_name(result);
			return json_reply(202, out);
		case PushResult::NotHosted:
			return json_error(409, push_result_name(result),
					"the connection holding this RID is not hosting");
		case PushResult::UnknownRid:
			break;
	}
	return json_error(404, push_result_name(PushResult::UnknownRid), unknown);
}

// Crow's JSON reader decodes each \uXXXX escape on its own, so an escaped character past
// U+FFFF ("\ud83d\ude00") arrives as its two surrogate halves, each spelled as a 3-byte
// sequence (0xED 0xA0..0xAF xx, then 0xED 0xB0..0xBF xx), which is not UTF-8. This joins each
// such pair into the character's 4-byte UTF-8; an unpaired half is left as it is, for the
// UTF-8 check to refuse.
std::string join_surrogate_pairs(std::string_view text) {
	const auto at = [&text](size_t i) { return static_cast<uint8_t>(text[i]); };
	const auto half = [&](size_t i, uint8_t lo, uint8_t hi) {
		return at(i) == 0xED && at(i + 1) >= lo && at(i + 1) <= hi && (at(i + 2) & 0xC0) == 0x80;
	};
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	while (i < text.size()) {
		if (i + 6 <= text.size() && half(i, 0xA0, 0xAF) && half(i + 3, 0xB0, 0xBF)) {
			const char32_t high = 0xD000 | ((at(i + 1) & 0x3F) << 6) | (at(i + 2) & 0x3F);
			const char32_t low = 0xD000 | ((at(i + 4) & 0x3F) << 6) | (at(i + 5) & 0x3F);
			utf8_append(out, 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00));
			i += 6;
			continue;
		}
		out.push_back(text[i]);
		++i;
	}
	return out;
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
// /api/lobbies names no owner. A host's text comes in its own code page (a
// stock host's cp1252), so it goes out as UTF-8 (retail_text_to_utf8).
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

// A hosted server's listing (ADR 0053, proposed): its stop is the hosted
// server's, the supervisor's own act (the row to stopping, then the
// supervisor's ServerStopHosting push and the machine's end), never a bare push
// here: that would end the server's session, and the supervisor would read the
// exit as a crash and restart it. 202 {"rid", "hosted_server_id", "state"}, the
// hosted server's state after the request (also when it was stopping already).
crow::response stop_hosted(db::Database &db, uint32_t rid, int64_t server_id, const char *detail) {
	hosted::request_stop(db, server_id, detail);
	const auto server = hosted::find_server(db, server_id);
	crow::json::wvalue out;
	out["rid"] = rid;
	out["hosted_server_id"] = server_id;
	out["state"] = server ? hosted::state_name(server->state) : hosted::state_name(hosted::State::Stopping);
	out["status"] = "hosted_stop";
	return json_reply(202, out);
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
	for (const std::string &json_arg : args) {
		const std::string arg = join_surrogate_pairs(json_arg);
		if (has_control_byte(arg) || has_c1_control(arg)) {
			return refused("invalid_command",
					"an arg holds a control byte (below 0x20, 0x7F, or a C1 control U+0080 to "
					"U+009F), which a stock host would carry into its game.cfg or its chat");
		}
		std::string bytes;
		std::u32string unstorable;
		if (!utf8_to_cp1252(arg, bytes, &unstorable)) {
			if (!is_valid_utf8(arg)) {
				return refused("invalid_command",
						"an arg is not UTF-8 (bytes in another encoding, or an unpaired "
						"surrogate escape)");
			}
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

ListingOwner listing_owner(db::Database &db, std::optional<int64_t> host_user_id) {
	ListingOwner out;
	if (!host_user_id)
		return out;
	// A service account stands for its hosted server's owner only while the
	// server is live (the one rule, hosted_repository.h).
	std::optional<int64_t> owner_id = host_user_id;
	if (const auto hosted_server = hosted::hosted_server_of_account(db, *host_user_id);
			hosted_server && hosted::is_live(hosted_server->state)) {
		out.hosted_server_id = hosted_server->server_id;
		owner_id = hosted_server->owner_user_id;
	}
	if (owner_id)
		out.owner = get_user_by_id(db, *owner_id);
	return out;
}

void HostRoutes::register_routes(WebApp &app) {
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
// plus "owner", {id, username} of the account whose HostKey listed it (for a
// live hosted server's listing, held by its service account, the hosted
// server's owner: hosted::hosted_server_of_account), or null for an anonymous
// listing (the HostKey named no account) and a hosted one whose owner is gone,
// and "hosted_server_id", the live hosted server a listing is, else null.
crow::response HostRoutes::admin_hosts(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::vector<crow::json::wvalue> entries;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		for (const auto &h : hostdb::list_hosts(*db_conn)) {
			crow::json::wvalue e = listed_host_json(h);
			const ListingOwner listing = listing_owner(*db_conn, h.host_user_id);
			if (listing.hosted_server_id)
				e["hosted_server_id"] = *listing.hosted_server_id;
			else
				e["hosted_server_id"] = nullptr;
			if (listing.owner) {
				crow::json::wvalue o;
				o["id"] = listing.owner->id;
				o["username"] = listing.owner->username;
				e["owner"] = std::move(o);
			} else {
				e["owner"] = nullptr;
			}
			entries.push_back(std::move(e));
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/hosts failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["count"] = entries.size();
	out["hosts"] = std::move(entries);
	return json_reply(200, out);
}

// The service's statements to a listed server, pushed over that server's own
// NovaWorld UDP session (docs/net/novaworld-net-re.md "the service side") by
// the lobby that holds it, which takes them as commands (lobby_repository.h).
// The admin pushes: 202 once queued for the lobby, 400 for a body
// compose_server_command refuses (with its reason), 404 for a RID no live
// connection holds, 409 for one whose connection is not hosting.
// Admin-gated (require_admin: the Bearer token, or an admin session with the
// CSRF header); AccessMiddleware logs each with its actor, and the push's own
// line logs what was queued.

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
	const uint32_t rid32 = rid_of(rid);
	PushResult result = PushResult::UnknownRid;
	try {
		result = lobbydb::queue_server_command(*pool_.acquire(), rid32, command.cmd);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/hosts/%llu/command failed: %s\n",
				static_cast<unsigned long long>(rid), e.what());
		return json_error(500, "db_error");
	}
	// The whole line (server_command_text caps it at 511 bytes) as UTF-8, control bytes escaped.
	const std::string text = line_text(command.cmd);
	std::printf("[http] POST /api/admin/hosts/%llu/command '%s' -> %s\n",
			static_cast<unsigned long long>(rid), loggable(text, text.size()).c_str(),
			push_result_name(result));
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
// (lobbydb::queue_stop_hosting; the record's "The exit"); the server
// leaves the browser as the statement goes out. No body. On a hosted server's
// listing (its service account's) it is that hosted server's stop instead
// (stop_hosted).
crow::response HostRoutes::admin_stop(const crow::request &req, uint64_t rid) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const uint32_t rid32 = rid_of(rid);
	PushResult result = PushResult::UnknownRid;
	try {
		auto db_conn = pool_.acquire();
		const auto row = hostdb::find_host_by_rid(*db_conn, rid32);
		if (row && row->host_user_id) {
			// A live hosted server's listing stops as the hosted server; any other
			// (an ended one's lingering listing included) takes the plain push.
			const auto hosted_server = hosted::hosted_server_of_account(*db_conn, *row->host_user_id);
			if (hosted_server && hosted::is_live(hosted_server->state))
				return stop_hosted(*db_conn, rid32, hosted_server->server_id, "stopped by an admin from its listing");
		}
		result = lobbydb::queue_stop_hosting(*db_conn, rid32, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/hosts/%llu/stop failed: %s\n",
				static_cast<unsigned long long>(rid), e.what());
		return json_error(500, "db_error");
	}
	std::printf("[http] POST /api/admin/hosts/%llu/stop -> %s\n",
			static_cast<unsigned long long>(rid), push_result_name(result));
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
// delete since its last write) is left out, as the push routes refuse it. The
// account's hosted servers' listings, held by their service accounts, are its
// too (hosted::listing_accounts_of, the one rule), each with
// "hosted_server_id" (null on a listing of the account's own).
crow::response HostRoutes::owner_hosts(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal)
		return std::move(*access.refusal);
	struct Owned {
		hostdb::HostRow row;
		std::optional<int64_t> hosted_server_id;
	};
	std::vector<Owned> owned;
	std::vector<crow::json::wvalue> hosts;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		for (const int64_t account : hosted::listing_accounts_of(*db_conn, access.user->id)) {
			std::optional<int64_t> hosted_server_id;
			if (account != access.user->id) {
				if (const auto hosted_server = hosted::hosted_server_of_account(*db_conn, account))
					hosted_server_id = hosted_server->server_id;
			}
			for (auto &h : hostdb::list_hosts_by_owner(*db_conn, account)) {
				if (hostdb::listing_owned_by(*db_conn, h.rid, account))
					owned.push_back({ std::move(h), hosted_server_id });
			}
		}
		std::sort(owned.begin(), owned.end(), [](const Owned &a, const Owned &b) {
			return a.row.created_at != b.row.created_at ? a.row.created_at < b.row.created_at
														: a.row.rid < b.row.rid;
		});
		for (const Owned &o : owned) {
			const hostdb::HostRow &h = o.row;
			crow::json::wvalue e = listed_host_json(h);
			if (o.hosted_server_id)
				e["hosted_server_id"] = *o.hosted_server_id;
			else
				e["hosted_server_id"] = nullptr;
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
	} catch (const db::Error &e) {
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
// the caller's keys claimed for it, or one of the caller's hosted servers'
// service accounts' keys (hosted::owned_listing_account, the one rule), the
// same reply for a free RID and another account's; then the push, scoped to
// that account, which the queue checks against the session's own owner (a
// session another account owns is 404 too), and the lobby again as the
// statement goes out, in its memory and by the live claim (a push whose owner
// changed in between is dropped there); 409 for the caller's listing whose
// connection is not hosting. On a
// live hosted server's listing the stop is the hosted server's stop instead
// (stop_hosted), and a command to one that is stopping is refused (409
// server_stopping: it is being ended, and a stop is the supervisor's). Every
// request past require_user is logged with the account, the RID and the
// outcome.
template <typename Push>
crow::response HostRoutes::push_as_owner(const WebSessionUser &user, uint64_t rid, OwnerPush kind,
		const std::string &line, Push push, crow::json::wvalue out) {
	const char *what = kind == OwnerPush::Stop ? "stop" : "command";
	const uint32_t rid32 = rid_of(rid);
	PushResult result = PushResult::UnknownRid;
	try {
		auto db_conn = pool_.acquire();
		const std::optional<int64_t> account = hosted::owned_listing_account(*db_conn, rid32, user.id);
		const auto hosted_server = account && *account != user.id
				? hosted::hosted_server_of_account(*db_conn, *account)
				: std::nullopt;
		if (hosted_server && hosted::is_live(hosted_server->state)) {
			if (kind == OwnerPush::Stop) {
				log_owner_push(user, rid, what, line, "hosted_stop");
				return stop_hosted(*db_conn, rid32, hosted_server->server_id,
						"stopped by its owner from its listing");
			}
			if (hosted_server->state == hosted::State::Stopping) {
				log_owner_push(user, rid, what, line, "server_stopping");
				return json_error(409, "server_stopping", "this hosted server is being stopped");
			}
		}
		if (!account) {
			// Logged apart from the queue's own unknown_rid (a session the caller does not own,
			// or none, at push time); the reply is the same.
			log_owner_push(user, rid, what, line, "no_listing_of_theirs");
			return push_reply(PushResult::UnknownRid, rid32, std::move(out), kNotYours);
		}
		result = push(*db_conn, rid32, *account);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] owner %s failed: %s\n", what, e.what());
		return json_error(500, "db_error");
	}
	log_owner_push(user, rid, what, line, push_result_name(result));
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
			user, rid, OwnerPush::Command, command.cmd,
			[&command](db::Database &db, uint32_t rid32, int64_t account) {
				return lobbydb::queue_server_command(db, rid32, command.cmd, account);
			},
			std::move(out));
}

// POST /api/me/hosts/<rid>/stop — the admin stop's sysop punt (MsgCode 7): a
// stock host ends its running match for everyone on the server and leaves
// NovaWorld. No body. On a hosted server's listing it is the hosted server's
// stop (stop_hosted: 202 with its "hosted_server_id" and "state").
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
			user, rid, OwnerPush::Stop, "",
			[](db::Database &db, uint32_t rid32, int64_t account) {
				return lobbydb::queue_stop_hosting(
						db, rid32, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT, 0, 0, account);
			},
			std::move(out));
}

} // namespace opennova::novaworld_server
