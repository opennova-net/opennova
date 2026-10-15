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
// server_not_running before it runs), its link up (503 admin_unavailable); the
// admission (below: 429 console_busy, 503 console_full, 409 rotation_busy);
// for an owner, one token from the account's read or write bucket (429
// rate_limited); then the link (503 admin_busy with its queue full, 504
// admin_timeout past the 3 s a request has for all its commands; a 503 hands
// the token back), and the command's own outcome (console_outcome). Behind
// ONNET_API_PROXY_ONLY like every /api route.
//
// The admission. A console request that runs commands holds a Crow handler
// thread while it waits on the link (up to 3 s; a hosted server pumps its admin
// port from its main loop, so a map load stalls it), so the console never takes
// every handler thread: each account (each admin session; the Bearer token as
// one) has at most one such request in flight, a second answered 429
// console_busy at once with no token drawn; and at most handler threads - 2
// are admitted service-wide (503 console_full past that), so two always stay
// for the rest of the service (the retail pages, the .gsb feeds, the logins).
// A rotation edit (an add, a remove, a next) also holds the server's one edit:
// its read of the rotation and its edit run as a pair, a second edit meanwhile
// answered 409 rotation_busy, so two removes can never both pass the
// last-map check. GET .../console reads the database alone and is not
// admitted.

#include "admin_link.h"
#include "console_commands.h"
#include "hosted_repository.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
	// The handler threads the console always leaves to the rest of the service.
	static constexpr size_t kHandlersKeptFree = 2;
	// A rate-limited account's refusals are logged once in this window.
	static constexpr std::chrono::seconds kLimitLogWindow{ 60 };

	ConsoleRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	ConsoleRoutes(const ConsoleRoutes &) = delete;
	ConsoleRoutes &operator=(const ConsoleRoutes &) = delete;

	// The HTTP listener's handler threads: the console admits at most
	// `threads` - kHandlersKeptFree requests at once (at least one). Before
	// register_routes().
	void set_handler_threads(size_t threads);
	// Registers the routes on `app`. `links` (which must outlive the routes) is
	// the service's admin links; null answers every command 503.
	void register_routes(WebApp &app, AdminLinks *links);

private:
	// Who a request acts for, on which server, and until when its commands may
	// wait; or the refusal. It holds its admission (and, for a rotation edit,
	// the server's one edit) until it is destroyed, and the owner's token, which
	// a 503 hands back.
	struct Caller {
		std::optional<crow::response> refusal;
		int64_t server_id = 0;
		std::string actor; // "owner" or "admin"
		std::string who;   // the caller in the log
		std::chrono::steady_clock::time_point deadline;
		std::shared_ptr<void> admission;
		std::shared_ptr<void> rotation_edit;
		RateLimiter *bucket = nullptr;
		std::string bucket_key;
		mutable bool refunded = false;
	};
	// The owner's live running server, admitted, a token drawn from its write
	// bucket (a command that changes the server) or its read bucket.
	Caller owner_caller(const WebSessionUser &user, bool write, bool rotation_edit);
	// Server `id`, live and running, admitted.
	Caller admin_caller(const RouteAccess &access, uint64_t id, bool rotation_edit);
	// The server's state and link, for `c.server_id`; the refusal when it is not
	// running or its link is not up.
	std::optional<crow::response> check_server(const HostedServer &server);
	// The admission of `account`'s request on `c`'s server (the class comment).
	void admit(Caller &c, const std::string &account, bool rotation_edit);
	// Hands the owner's token back (a 503: the link took no command).
	static void refund(const Caller &c);
	// Whether `account`'s rate-limit refusal is logged (once per kLimitLogWindow).
	bool log_limit(const std::string &account);
	// One command on the caller's link within its deadline: the replies when the
	// command succeeded, else nullopt with `refusal` set.
	std::optional<std::vector<std::string>> run(const Caller &c, const ConsoleCommand &command,
			std::optional<crow::response> &refusal);
	// The server's scene (GET GAMESTATE): nullopt in a match, else the refusal
	// (409 not_in_game, or the link's).
	std::optional<crow::response> require_match(const Caller &c);

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

	std::mutex admit_mu_;
	size_t max_admitted_ = 6;
	size_t admitted_ = 0;
	std::set<std::string> busy_accounts_;
	std::set<int64_t> rotation_edits_;
	std::map<std::string, std::chrono::steady_clock::time_point> limit_logged_;
};

} // namespace opennova::novaworld_server::hosted
