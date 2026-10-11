#pragma once

// The listed servers' /api routes: who lists what (GET /api/admin/hosts, GET
// /api/me/hosts) and the ServerCommand / ServerStopHosting pushes over a
// server's own NovaWorld session (NwUdpListener::push_server_command /
// push_stop_hosting), for an admin (POST /api/admin/hosts/<rid>/command and
// /stop, require_admin) and for the account that listed the server (POST
// /api/me/hosts/<rid>/command and /stop, require_user; service policy). Both
// families compose a command through compose_server_command, the one rule for
// what line reaches a host. The HttpListener owns one HostRoutes over its Crow
// app, beside its WebAccess (README "Pushing statements to a listed server",
// "Listed servers and owners").

#include "rate_limiter.h"
#include "web_access.h"

#include <net/napi/session.h>
#include <net/novaworld/db/sqlite.h>

#include <crow.h>

#include <cstdint>
#include <optional>
#include <string>

namespace opennova::novaworld_server {

class NwUdpListener;

// A push route's ServerCommand body, {"verb", "target", "args"}, composed:
// `verb` is "SetServerName" and the rest of the eighteen (case-insensitive),
// `target` None (or absent), ByIndex, ByIpAndPort, ByName or ByPCID, `args` a
// list of strings (absent: none). The line comes from server_command_text, so
// one the host's reader would drop (a verb/target pairing it ignores, fewer
// args than the verb's token-count gate, a double quote or a NUL, past 511
// characters) is refused with the composer's reason; so is an arg holding a
// control byte (has_control_byte), the service's own input rule: the stock
// reader carries such a byte faithfully inside quotes (String_TokenizeQuotedToArray
// copies a quoted run as it is), but SetServerName / SetServerMsg save the token
// into the host's game.cfg unescaped (Game_SaveConfig @0x54C490, `game_name =
// "%s"\n`; ours engine/formats/gamecfg/game_cfg_write.cpp), so a CR or LF
// would inject a line into a third party's config, and the chat verbs would
// relay the byte to every player.
struct ComposedServerCommand {
	ServerCommandVerb verb = ServerCommandVerb::None;
	std::string cmd; // the composed Cmd line, when accepted
	// Set: the 400 the route answers, pushing nothing.
	std::optional<crow::response> refusal;
};
ComposedServerCommand compose_server_command(const std::string &body);

// The verbs the account that listed a server may push to it (service policy):
// every ServerCommand verb acts on that server alone, so all of them but
// SetMPReset. SetMPReset stores the host's mpreset word in its game.cfg, and a
// nonzero word ends the host process at its next session create
// (inmatch::create_session's ProcessExit, D-NET-385), a change saved past the
// run that the owner's page could neither show nor undo once the server left
// the list; it stays an admin's verb. The stop (ServerStopHosting) is the
// owner's way to end the hosting.
bool owner_may_send(ServerCommandVerb verb);

class HostRoutes {
public:
	// Each owner push (command or stop), per account: 8 at once, then one every
	// 2 s, so a player can neither flood their server's chat nor the service.
	static constexpr RateLimiter::Params kOwnerPushPerUser{ 8, 1.0 / 2, 10000 };

	HostRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	HostRoutes(const HostRoutes &) = delete;
	HostRoutes &operator=(const HostRoutes &) = delete;

	// Registers the six routes on `app`. `nw_udp` is the push channel, which
	// must outlive the routes; null answers every push 503.
	void register_routes(WebApp &app, NwUdpListener *nw_udp);

private:
	crow::response admin_hosts(const crow::request &req);
	crow::response admin_command(const crow::request &req, uint64_t rid);
	crow::response admin_stop(const crow::request &req, uint64_t rid);
	crow::response owner_hosts(const crow::request &req);
	crow::response owner_command(const crow::request &req, uint64_t rid);
	crow::response owner_stop(const crow::request &req, uint64_t rid);
	// An owner's push once the body is composed: 404 unless the listing under
	// `rid` is `user`'s (hostdb::listing_owned_by: listed, with one of the
	// account's keys claimed for the RID), then the push scoped to that account,
	// which the listener checks against the connection (NwUdpListener's `owner`).
	// `push` is the statement's push for the 32-bit RID; `line` names it in the
	// log.
	template <typename Push>
	crow::response push_as_owner(const WebSessionUser &user, uint64_t rid, const char *what,
			const std::string &line, Push push, crow::json::wvalue out);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	NwUdpListener *nw_udp_ = nullptr;
	RateLimiter owner_push_per_user_{ kOwnerPushPerUser };
};

} // namespace opennova::novaworld_server
