#pragma once

// The hosted servers' console routes (ADR 0053 d5, proposed; README "Hosted
// servers"): the site's players, rotation, chat and console pages run the
// curated commands (console_commands.h) on the server's admin link
// (admin_link.h). The owner's routes act on the caller's own live server
// (resolved by its session, never named in the path): GET
// /api/me/server/players, POST .../players/<slot>/punt, /ban, /swap and /kill,
// GET .../missions, GET and POST .../rotation (the entries; one appended), POST
// .../rotation/<index>/remove and /next, POST .../rotation/cycle, GET and POST
// .../chat, and GET .../console. The admin's are the same under
// /api/admin/hosted/<id>/..., plus GET .../settings. Every reply is JSON; a
// refusal is an error code with our own message, never the server's text.
//
// In order: require_user (401 with no live session, 403 without the CSRF
// header on a POST) or require_admin; the arguments (400, before anything is
// read); the server: the owner's live one (404 no_live_server) or the id (404
// not_found, 409 not_live), running (409 server_stopping while it stops, 409
// server_not_running before it runs); for an owner, one token from the
// account's read or write bucket (429); then the link (503 admin_unavailable
// with none up, 503 admin_busy with its queue full, 504 admin_timeout past the
// 3 s a request has for all its commands), and the command's own outcome
// (console_outcome). Behind ONNET_API_PROXY_ONLY like every /api route.

#include "admin_link.h"
#include "console_commands.h"
#include "hosted_repository.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/sqlite.h>

#include <crow.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

class ConsoleRoutes {
public:
	// The owner's reads (the pages poll them), per account: 30 at once, then two
	// a second.
	static constexpr RateLimiter::Params kOwnerReadPerUser{ 30, 2.0, 10000 };
	// The owner's commands that change the server, per account: 8 at once, then
	// one every 2 s, as the owner's NovaWorld pushes (HostRoutes::kOwnerPushPerUser).
	static constexpr RateLimiter::Params kOwnerWritePerUser{ 8, 1.0 / 2, 10000 };
	// What one request may wait for all its commands on the link.
	static constexpr std::chrono::milliseconds kCommandBudget{ 3000 };
	// GET .../console's lines: the newest 200 by default, kConsoleKeep at most.
	static constexpr size_t kConsoleLinesShown = 200;

	ConsoleRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	ConsoleRoutes(const ConsoleRoutes &) = delete;
	ConsoleRoutes &operator=(const ConsoleRoutes &) = delete;

	// Registers the routes on `app`. `links` (which must outlive the routes) is
	// the service's admin links; null answers every command 503.
	void register_routes(WebApp &app, AdminLinks *links);

private:
	// Who a request acts for, on which server, and until when its commands may
	// wait; or the refusal.
	struct Caller {
		std::optional<crow::response> refusal;
		int64_t server_id = 0;
		std::string actor; // "owner" or "admin"
		std::string who;   // the caller in the log
		std::chrono::steady_clock::time_point deadline;
	};
	// The owner's live running server, a token drawn from its write bucket (a
	// command that changes the server) or its read bucket.
	Caller owner_caller(const WebSessionUser &user, bool write);
	// Server `id`, live and running.
	Caller admin_caller(const RouteAccess &access, uint64_t id);
	// The refusal for a server that is not live and running; nullopt for one that is.
	static std::optional<crow::response> not_running(const HostedServer &server);
	// One command on the caller's link within its deadline: the replies when the
	// command succeeded, else nullopt with `refusal` set.
	std::optional<std::vector<std::string>> run(const Caller &c, const ConsoleCommand &command,
			std::optional<crow::response> &refusal);

	// The work behind each route, shared by the owner's and the admin's.
	crow::response players(const Caller &c);
	crow::response player_action(const Caller &c, const ConsoleCommand &command, uint64_t slot);
	crow::response missions(const Caller &c);
	crow::response rotation(const Caller &c);
	crow::response rotation_add(const Caller &c, const ConsoleCommand &command, const std::string &mission);
	crow::response rotation_entry(const Caller &c, const ConsoleCommand &command, uint64_t index);
	crow::response rotation_cycle(const Caller &c);
	crow::response chat(const Caller &c);
	crow::response chat_send(const Caller &c, const ConsoleCommand &command);
	crow::response console(const Caller &c, const crow::request &req);
	crow::response settings(const Caller &c);

	void register_owner(WebApp &app);
	void register_admin(WebApp &app);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	AdminLinks *links_ = nullptr;
	RateLimiter owner_reads_{ kOwnerReadPerUser };
	RateLimiter owner_writes_{ kOwnerWritePerUser };
};

} // namespace opennova::novaworld_server::hosted
