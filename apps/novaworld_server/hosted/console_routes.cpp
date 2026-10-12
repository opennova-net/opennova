#include "console_routes.h"

#include "http_json.h"
#include "loggable.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <net/admin/admin_protocol.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

using Clock = std::chrono::steady_clock;

crow::response no_store(crow::response res) {
	res.set_header("Cache-Control", "no-store");
	return res;
}

// The server's cp1252 as UTF-8 for a JSON reply.
std::string utf8(const std::string &cp1252) {
	return cp1252_to_utf8(cp1252);
}

int64_t server_id_of(uint64_t id) {
	return id <= static_cast<uint64_t>(INT64_MAX) ? static_cast<int64_t>(id) : 0;
}

crow::response refused(const ConsoleRefusal &why) {
	return json_error(400, why.error.c_str(), why.message);
}

// A query parameter that must be a small non-negative integer (digits alone).
std::optional<uint64_t> query_number(const crow::request &req, const char *name) {
	const char *value = req.url_params.get(name);
	if (value == nullptr) return std::nullopt;
	const std::string text(value);
	if (text.empty() || text.size() > 18 ||
			!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
		return std::nullopt;
	const auto number = strutil::parse_ullong(text);
	if (!number) return std::nullopt;
	return static_cast<uint64_t>(*number);
}

const char *result_of(ConsoleVerb verb) {
	switch (verb) {
		case ConsoleVerb::PlayerPunt: return "punted";
		case ConsoleVerb::PlayerBan: return "banned";
		case ConsoleVerb::PlayerSwapTeam: return "swapped";
		case ConsoleVerb::PlayerKill: return "killed";
		case ConsoleVerb::MissionAdd: return "added";
		case ConsoleVerb::MissionRemove: return "removed";
		case ConsoleVerb::MissionSetNext: return "next_set";
		case ConsoleVerb::MissionCycle: return "cycling";
		case ConsoleVerb::ChatSend: return "sent";
		default: return "ok";
	}
}

crow::json::wvalue link_json(const AdminLinkStatus &status) {
	crow::json::wvalue out;
	out["state"] = admin_link_state_name(status.state);
	out["game_state"] = status.game_state;
	out["last_poll_at"] = status.last_poll_at;
	out["up_since"] = status.up_since;
	out["error"] = status.error;
	return out;
}

// A release run when the last holder of the returned pointer lets it go.
std::shared_ptr<void> on_release(std::function<void()> fn) {
	struct Release {
		std::function<void()> fn;
		~Release() {
			if (fn) fn();
		}
	};
	auto release = std::make_shared<Release>();
	release->fn = std::move(fn);
	return release;
}

// A refusal the caller may try again shortly.
crow::response busy(int status, const char *error, const char *message) {
	crow::response res = json_error(status, error, message);
	res.set_header("Retry-After", "1");
	return res;
}

} // namespace

void ConsoleRoutes::set_handler_threads(size_t threads) {
	std::lock_guard<std::mutex> lock(admit_mu_);
	max_admitted_ = threads > kHandlersKeptFree + 1 ? threads - kHandlersKeptFree : 1;
}

void ConsoleRoutes::register_routes(WebApp &app, AdminLinks *links) {
	links_ = links;
	register_owner(app);
	register_admin(app);
}

std::optional<crow::response> ConsoleRoutes::check_server(const HostedServer &server) {
	if (server.state == State::Stopping)
		return json_error(409, "server_stopping", "this hosted server is being stopped");
	if (server.state != State::Running)
		return json_error(409, "server_not_running", "this hosted server is not running yet");
	// No link up: refused before any admission or token.
	const std::shared_ptr<AdminLink> link = links_ != nullptr ? links_->find(server.id) : nullptr;
	if (!link || link->status().state != AdminLinkState::Up)
		return json_error(503, "admin_unavailable", "the server's admin link is not up; try again shortly");
	return std::nullopt;
}

void ConsoleRoutes::admit(Caller &c, const std::string &account, bool rotation_edit) {
	std::lock_guard<std::mutex> lock(admit_mu_);
	if (busy_accounts_.count(account) != 0) {
		c.refusal = busy(429, "console_busy", "this account's previous console request is still running");
		return;
	}
	if (admitted_ >= max_admitted_) {
		c.refusal = busy(503, "console_full", "the service's console is busy; try again shortly");
		return;
	}
	if (rotation_edit && rotation_edits_.count(c.server_id) != 0) {
		c.refusal = busy(409, "rotation_busy", "another edit of this server's rotation is running");
		return;
	}
	busy_accounts_.insert(account);
	++admitted_;
	c.admission = on_release([this, account] {
		std::lock_guard<std::mutex> release_lock(admit_mu_);
		busy_accounts_.erase(account);
		--admitted_;
	});
	if (rotation_edit) {
		const int64_t id = c.server_id;
		rotation_edits_.insert(id);
		c.rotation_edit = on_release([this, id] {
			std::lock_guard<std::mutex> release_lock(admit_mu_);
			rotation_edits_.erase(id);
		});
	}
}

void ConsoleRoutes::refund(const Caller &c) {
	if (c.bucket == nullptr || c.refunded) return;
	c.refunded = true;
	c.bucket->refund(c.bucket_key, RateLimiter::Clock::now());
}

bool ConsoleRoutes::log_limit(const std::string &account) {
	std::lock_guard<std::mutex> lock(admit_mu_);
	const auto now = Clock::now();
	const auto it = limit_logged_.find(account);
	if (it != limit_logged_.end() && now - it->second < kLimitLogWindow) return false;
	if (limit_logged_.size() >= 1024) {
		for (auto old = limit_logged_.begin(); old != limit_logged_.end();)
			old = now - old->second >= kLimitLogWindow ? limit_logged_.erase(old) : std::next(old);
	}
	limit_logged_[account] = now;
	return true;
}

ConsoleRoutes::Caller ConsoleRoutes::owner_caller(const WebSessionUser &user, bool write, bool rotation_edit) {
	Caller c;
	c.actor = "owner";
	c.who = "owner '" + loggable(user.username) + "' (id=" + std::to_string(user.id) + ")";
	try {
		const auto server = latest_server_of(*pool_.acquire(), user.id);
		if (!server || !is_live(server->state)) {
			c.refusal = json_error(404, "no_live_server", "this account runs no hosted server");
			return c;
		}
		c.server_id = server->id;
		if (auto refusal = check_server(*server)) {
			c.refusal = std::move(refusal);
			return c;
		}
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] hosted console lookup failed: %s\n", e.what());
		c.refusal = json_error(500, "db_error");
		return c;
	}
	const std::string account = "user:" + std::to_string(user.id);
	admit(c, account, rotation_edit);
	if (c.refusal) return c;
	// One token, after the checks and the admission: a refused request costs the
	// account nothing, and a 503 from the link hands it back (run()).
	RateLimiter &bucket = write ? owner_writes_ : owner_reads_;
	const std::string key = "console|" + std::to_string(user.id);
	if (const int64_t wait = bucket.take(key, RateLimiter::Clock::now())) {
		if (log_limit(account))
			std::printf("[hosted] console server %lld: %s -> rate_limited (logged once a minute)\n",
					static_cast<long long>(c.server_id), c.who.c_str());
		c.refusal = too_many_requests(wait);
		return c;
	}
	c.bucket = &bucket;
	c.bucket_key = key;
	c.deadline = Clock::now() + kCommandBudget;
	return c;
}

ConsoleRoutes::Caller ConsoleRoutes::admin_caller(const RouteAccess &access, uint64_t id, bool rotation_edit) {
	Caller c;
	c.actor = "admin";
	c.who = access.user ? "admin '" + loggable(access.user->username) + "'" : std::string("admin (token)");
	try {
		const auto server = find_server(*pool_.acquire(), server_id_of(id));
		if (!server) {
			c.refusal = json_error(404, "not_found", "no hosted server has this id");
			return c;
		}
		if (!is_live(server->state)) {
			c.refusal = json_error(409, "not_live", "this hosted server has ended");
			return c;
		}
		c.server_id = server->id;
		if (auto refusal = check_server(*server)) {
			c.refusal = std::move(refusal);
			return c;
		}
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] hosted console lookup failed: %s\n", e.what());
		c.refusal = json_error(500, "db_error");
		return c;
	}
	admit(c, access.user ? "user:" + std::to_string(access.user->id) : std::string("token"), rotation_edit);
	if (c.refusal) return c;
	c.deadline = Clock::now() + kCommandBudget;
	return c;
}

std::optional<std::vector<std::string>> ConsoleRoutes::run(const Caller &c, const ConsoleCommand &command,
		std::optional<crow::response> &refusal) {
	const bool writes = console_verb_writes(command.verb);
	// Every command is logged, a read only when it fails (the pages poll them).
	const auto log = [&](const std::string &outcome) {
		if (writes || outcome != "ok")
			std::printf("[hosted] console server %lld: %s %s -> %s\n", static_cast<long long>(c.server_id),
					c.who.c_str(), console_verb_name(command.verb), outcome.c_str());
	};
	// The link took no command: the owner's token goes back.
	const auto unavailable = [&](const char *error, const char *message) {
		log(error);
		refund(c);
		refusal = busy(503, error, message);
		return std::nullopt;
	};
	const std::shared_ptr<AdminLink> link = links_ != nullptr ? links_->find(c.server_id) : nullptr;
	if (!link) return unavailable("admin_unavailable", "the server's admin link is not up; try again shortly");
	const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(c.deadline - Clock::now());
	if (left.count() <= 0) {
		log("admin_timeout");
		refusal = json_error(504, "admin_timeout", "the server did not answer in time");
		return std::nullopt;
	}
	AdminCommandOptions options;
	options.actor = c.actor;
	options.record = writes;
	AdminCommandResult result = link->run(command.line, options, left);
	switch (result.outcome) {
		case AdminCommandOutcome::Answered:
			break;
		case AdminCommandOutcome::Busy:
			return unavailable("admin_busy", "the server's admin link is busy; try again shortly");
		case AdminCommandOutcome::TimedOut: {
			log(result.sent ? "admin_timeout (sent)" : "admin_timeout (dropped)");
			crow::json::wvalue out;
			out["error"] = "admin_timeout";
			out["message"] = result.sent ? "the server did not answer in time; the command went out"
										 : "the server did not answer in time; the command was not sent";
			out["sent"] = result.sent;
			refusal = json_reply(504, out);
			return std::nullopt;
		}
		case AdminCommandOutcome::Refused:
			// The service composed the line: a fault of its own.
			std::fprintf(stderr, "[hosted] ERROR console server %lld: the link refused a composed %s line\n",
					static_cast<long long>(c.server_id), console_verb_name(command.verb));
			refusal = json_error(500, "internal_error");
			return std::nullopt;
		case AdminCommandOutcome::LinkDown:
		case AdminCommandOutcome::Stopped:
			return unavailable("admin_unavailable", "the server's admin link is not up; try again shortly");
	}
	const ConsoleOutcome outcome = console_outcome(command.verb, result.replies);
	if (outcome.status != 200) {
		log(outcome.error);
		refusal = json_error(outcome.status, outcome.error.c_str(), outcome.message);
		return std::nullopt;
	}
	log("ok");
	return std::move(result.replies);
}

// Retail answers MISSION LIST with `No missions in queue.` outside the Game Loop
// (the inter-round window, the menus), and MISSION ADD with `OK - Entry Added`
// there while it appends nothing, so an empty rotation is read against the
// scene. [orig: CAdminServer_HandleMissionCommand @0x40634F;
//  CAdminServer_HandleMissionAdd @0x403E9C..0x403ECF (the append in the Game Loop
//  alone)]
std::optional<crow::response> ConsoleRoutes::require_match(const Caller &c) {
	std::optional<crow::response> refusal;
	const auto replies = run(c, *console_command(ConsoleVerb::GameState), refusal);
	if (!replies) return refusal;
	if (console_scene(*replies) != "Game")
		return json_error(409, "not_in_game", "the server is between matches; try again shortly");
	return std::nullopt;
}

crow::response ConsoleRoutes::players(const Caller &c) {
	std::optional<crow::response> refusal;
	const auto replies = run(c, *console_command(ConsoleVerb::PlayerList), refusal);
	if (!replies) return std::move(*refusal);
	std::vector<crow::json::wvalue> list;
	for (const AdminPlayer &p : admin_parse_player_list(replies->front())) {
		crow::json::wvalue e;
		e["slot"] = p.slot;
		e["name"] = utf8(p.name);
		e["team"] = strutil::parse_int(p.team).value_or(0);
		e["class"] = p.player_class;
		e["kills"] = p.kills;
		e["deaths"] = p.deaths;
		e["ping"] = p.ping;
		list.push_back(std::move(e));
	}
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["players"] = std::move(list);
	return no_store(json_reply(200, out));
}

crow::response ConsoleRoutes::player_action(const Caller &c, const ConsoleCommand &command, uint64_t slot) {
	std::optional<crow::response> refusal;
	const auto replies = run(c, command, refusal);
	if (!replies) return std::move(*refusal);
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["slot"] = static_cast<int64_t>(slot);
	out["result"] = result_of(command.verb);
	// A NovaWorld host lists the PCID (`OK - Banned Player #...`) before the punt's reply.
	if (command.verb == ConsoleVerb::PlayerBan) out["listed"] = replies->size() >= 2;
	return json_reply(200, out);
}

crow::response ConsoleRoutes::missions(const Caller &c) {
	std::optional<crow::response> refusal;
	const auto replies = run(c, *console_command(ConsoleVerb::MissionAvailable), refusal);
	if (!replies) return std::move(*refusal);
	std::vector<crow::json::wvalue> list;
	for (const AdminCatalogEntry &m : admin_parse_mission_available(replies->front())) {
		crow::json::wvalue e;
		e["index"] = m.index;
		e["file"] = utf8(m.file);
		e["title"] = utf8(m.title);
		list.push_back(std::move(e));
	}
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["missions"] = std::move(list);
	return no_store(json_reply(200, out));
}

crow::response ConsoleRoutes::rotation(const Caller &c) {
	std::optional<crow::response> refusal;
	const auto replies = run(c, *console_command(ConsoleVerb::MissionList), refusal);
	if (!replies) return std::move(*refusal);
	const std::vector<AdminRotationEntry> entries = admin_parse_mission_list(replies->front());
	// None listed: between matches, or a match with no rotation.
	if (entries.empty()) {
		if (auto out_of_match = require_match(c)) return std::move(*out_of_match);
	}
	std::vector<crow::json::wvalue> list;
	for (const AdminRotationEntry &r : entries) {
		crow::json::wvalue e;
		e["index"] = r.index;
		e["file"] = utf8(r.file);
		e["swap_sides"] = r.swap_sides;
		e["flipped"] = r.flipped;
		e["one_shot"] = r.one_shot;
		e["current"] = r.current;
		e["next"] = r.next;
		list.push_back(std::move(e));
	}
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["rotation"] = std::move(list);
	return no_store(json_reply(200, out));
}

// The rotation is read first, in the edit the caller's admission holds: a
// rotation of kConsoleRotationMax entries takes no more (service policy), and an
// empty one is read against the scene (require_match: retail's ADD answers OK
// outside a match while it appends nothing).
crow::response ConsoleRoutes::rotation_add(const Caller &c, const ConsoleCommand &command, const std::string &mission) {
	std::optional<crow::response> refusal;
	const auto listed = run(c, *console_command(ConsoleVerb::MissionList), refusal);
	if (!listed) return std::move(*refusal);
	const std::vector<AdminRotationEntry> entries = admin_parse_mission_list(listed->front());
	if (entries.empty()) {
		if (auto out_of_match = require_match(c)) return std::move(*out_of_match);
	}
	if (entries.size() >= kConsoleRotationMax)
		return json_error(409, "rotation_full",
				"the rotation holds " + std::to_string(kConsoleRotationMax) + " maps, all the console builds");
	if (!run(c, command, refusal)) return std::move(*refusal);
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["mission"] = mission;
	out["result"] = result_of(command.verb);
	return json_reply(200, out);
}

// REMOVE and SETNEXT answer OK whatever the index, so the rotation is read
// first, in the edit the caller's admission holds (so no other edit lands
// between the read and this one): an index it does not have is refused, and so
// is removing its last map (service policy: an empty rotation finds no entry
// at the round's end, as MISSION CLEAR's does, and the session ends, D-NET-331's
// miss arm); an empty read is checked against the scene.
// [orig: CAdminServer_HandleMissionCommand — REMOVE's `OK - Mission Removed.`
//  and SETNEXT's `OK - Next Mission Set.` @0x4066BF..0x4066EA, either way]
crow::response ConsoleRoutes::rotation_entry(const Caller &c, const ConsoleCommand &command, uint64_t index) {
	std::optional<crow::response> refusal;
	const auto listed = run(c, *console_command(ConsoleVerb::MissionList), refusal);
	if (!listed) return std::move(*refusal);
	const std::vector<AdminRotationEntry> entries = admin_parse_mission_list(listed->front());
	if (entries.empty()) {
		if (auto out_of_match = require_match(c)) return std::move(*out_of_match);
	}
	const bool present = std::any_of(entries.begin(), entries.end(),
			[index](const AdminRotationEntry &e) { return e.index >= 0 && static_cast<uint64_t>(e.index) == index; });
	if (!present) return json_error(404, "no_such_entry", "the rotation has no entry at this index");
	if (command.verb == ConsoleVerb::MissionRemove && entries.size() <= 1)
		return json_error(409, "last_mission", "the rotation's last map stays: without it the server's session would end");
	if (!run(c, command, refusal)) return std::move(*refusal);
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["index"] = static_cast<int64_t>(index);
	out["result"] = result_of(command.verb);
	return json_reply(200, out);
}

crow::response ConsoleRoutes::rotation_cycle(const Caller &c) {
	std::optional<crow::response> refusal;
	if (!run(c, *console_command(ConsoleVerb::MissionCycle), refusal)) return std::move(*refusal);
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["result"] = result_of(ConsoleVerb::MissionCycle);
	return json_reply(200, out);
}

crow::response ConsoleRoutes::chat(const Caller &c) {
	std::optional<crow::response> refusal;
	const auto replies = run(c, *console_command(ConsoleVerb::ChatGet), refusal);
	if (!replies) return std::move(*refusal);
	std::vector<crow::json::wvalue> lines;
	for (const std::string &line : admin_parse_chat_window(replies->front())) {
		crow::json::wvalue e;
		e = utf8(line);
		lines.push_back(std::move(e));
	}
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["lines"] = std::move(lines);
	return no_store(json_reply(200, out));
}

crow::response ConsoleRoutes::chat_send(const Caller &c, const ConsoleCommand &command) {
	std::optional<crow::response> refusal;
	if (!run(c, command, refusal)) return std::move(*refusal);
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["result"] = result_of(command.verb);
	return json_reply(200, out);
}

crow::response ConsoleRoutes::settings(const Caller &c) {
	std::optional<crow::response> refusal;
	const auto replies = run(c, *console_command(ConsoleVerb::GameSettings), refusal);
	if (!replies) return std::move(*refusal);
	std::vector<crow::json::wvalue> rows;
	for (const auto &row : admin_parse_game_settings(replies->front())) {
		crow::json::wvalue e;
		e["key"] = utf8(row.first);
		e["value"] = utf8(row.second);
		rows.push_back(std::move(e));
	}
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	out["settings"] = std::move(rows);
	return no_store(json_reply(200, out));
}

// The server's console ring (hosted_server_console) and its link's status: the
// newest `limit` lines (kConsoleLinesShown by default, kConsoleKeep at most)
// with an id past `after` (0), oldest first. No admin traffic.
crow::response ConsoleRoutes::console(const Caller &c, const crow::request &req) {
	const uint64_t after = query_number(req, "after").value_or(0);
	const uint64_t limit =
			std::min<uint64_t>(std::max<uint64_t>(query_number(req, "limit").value_or(kConsoleLinesShown), 1), kConsoleKeep);
	std::vector<crow::json::wvalue> lines;
	try {
		for (const ConsoleLine &line : list_console(*pool_.acquire(), c.server_id,
					 static_cast<int64_t>(std::min<uint64_t>(after, static_cast<uint64_t>(INT64_MAX))),
					 static_cast<size_t>(limit))) {
			crow::json::wvalue e;
			e["id"] = line.id;
			e["kind"] = line.kind;
			e["actor"] = line.actor;
			e["text"] = line.text;
			e["at"] = line.at;
			lines.push_back(std::move(e));
		}
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] hosted console read failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["server_id"] = c.server_id;
	const std::shared_ptr<AdminLink> link = links_ != nullptr ? links_->find(c.server_id) : nullptr;
	if (link)
		out["link"] = link_json(link->status());
	else
		out["link"] = nullptr;
	out["lines"] = std::move(lines);
	return no_store(json_reply(200, out));
}

void ConsoleRoutes::register_owner(WebApp &app) {
	using Work = crow::response (ConsoleRoutes::*)(const Caller &);
	// A route with no argument: require_user, then the owner's server and a token.
	const auto simple = [this](const crow::request &req, bool write, Work work) -> crow::response {
		auto access = access_.require_user(req);
		if (access.refusal) return std::move(*access.refusal);
		Caller c = owner_caller(*access.user, write, false);
		if (c.refusal) return std::move(*c.refusal);
		return (this->*work)(c);
	};
	CROW_ROUTE(app, "/api/me/server/players")
			.methods("GET"_method)([simple](const crow::request &req) { return simple(req, false, &ConsoleRoutes::players); });
	CROW_ROUTE(app, "/api/me/server/missions")
			.methods("GET"_method)([simple](const crow::request &req) { return simple(req, false, &ConsoleRoutes::missions); });
	CROW_ROUTE(app, "/api/me/server/rotation")
			.methods("GET"_method)([simple](const crow::request &req) { return simple(req, false, &ConsoleRoutes::rotation); });
	CROW_ROUTE(app, "/api/me/server/rotation/cycle")
			.methods("POST"_method)([simple](const crow::request &req) { return simple(req, true, &ConsoleRoutes::rotation_cycle); });
	CROW_ROUTE(app, "/api/me/server/chat")
			.methods("GET"_method)([simple](const crow::request &req) { return simple(req, false, &ConsoleRoutes::chat); });

	// POST /api/me/server/players/<slot>/punt, /ban, /swap, /kill.
	const auto player = [this](const crow::request &req, uint64_t slot, ConsoleVerb verb) -> crow::response {
		auto access = access_.require_user(req);
		if (access.refusal) return std::move(*access.refusal);
		ConsoleRefusal why;
		const auto command = console_player_command(verb, slot, why);
		if (!command) return refused(why);
		Caller c = owner_caller(*access.user, true, false);
		if (c.refusal) return std::move(*c.refusal);
		return player_action(c, *command, slot);
	};
	CROW_ROUTE(app, "/api/me/server/players/<uint>/punt").methods("POST"_method)([player](const crow::request &req, uint64_t slot) {
		return player(req, slot, ConsoleVerb::PlayerPunt);
	});
	CROW_ROUTE(app, "/api/me/server/players/<uint>/ban").methods("POST"_method)([player](const crow::request &req, uint64_t slot) {
		return player(req, slot, ConsoleVerb::PlayerBan);
	});
	CROW_ROUTE(app, "/api/me/server/players/<uint>/swap").methods("POST"_method)([player](const crow::request &req, uint64_t slot) {
		return player(req, slot, ConsoleVerb::PlayerSwapTeam);
	});
	CROW_ROUTE(app, "/api/me/server/players/<uint>/kill").methods("POST"_method)([player](const crow::request &req, uint64_t slot) {
		return player(req, slot, ConsoleVerb::PlayerKill);
	});

	// POST /api/me/server/rotation/<index>/remove and /next.
	const auto entry = [this](const crow::request &req, uint64_t index, ConsoleVerb verb) -> crow::response {
		auto access = access_.require_user(req);
		if (access.refusal) return std::move(*access.refusal);
		ConsoleRefusal why;
		const auto command = console_rotation_command(verb, index, why);
		if (!command) return refused(why);
		Caller c = owner_caller(*access.user, true, true);
		if (c.refusal) return std::move(*c.refusal);
		return rotation_entry(c, *command, index);
	};
	CROW_ROUTE(app, "/api/me/server/rotation/<uint>/remove").methods("POST"_method)([entry](const crow::request &req, uint64_t index) {
		return entry(req, index, ConsoleVerb::MissionRemove);
	});
	CROW_ROUTE(app, "/api/me/server/rotation/<uint>/next").methods("POST"_method)([entry](const crow::request &req, uint64_t index) {
		return entry(req, index, ConsoleVerb::MissionSetNext);
	});

	// POST /api/me/server/rotation — {"mission": "<file>.bms", "swap_sides": false}.
	CROW_ROUTE(app, "/api/me/server/rotation").methods("POST"_method)([this](const crow::request &req) -> crow::response {
		auto access = access_.require_user(req);
		if (access.refusal) return std::move(*access.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		const auto mission = body->text("mission");
		const bool swap_sides = body->flag("swap_sides").value_or(false);
		if (body->wrong_type()) return json_error(400, "invalid_field");
		if (!mission) return json_error(400, "missing_field", "\"mission\" (a .bms file name) is required");
		ConsoleRefusal why;
		const auto command = console_mission_add(*mission, swap_sides, why);
		if (!command) return refused(why);
		Caller c = owner_caller(*access.user, true, true);
		if (c.refusal) return std::move(*c.refusal);
		return rotation_add(c, *command, *mission);
	});

	// POST /api/me/server/chat — {"text": "..."}.
	CROW_ROUTE(app, "/api/me/server/chat").methods("POST"_method)([this](const crow::request &req) -> crow::response {
		auto access = access_.require_user(req);
		if (access.refusal) return std::move(*access.refusal);
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		const auto text = body->text("text");
		if (body->wrong_type()) return json_error(400, "invalid_field");
		if (!text) return json_error(400, "missing_field", "\"text\" is required");
		ConsoleRefusal why;
		const auto command = console_chat_send(*text, why);
		if (!command) return refused(why);
		Caller c = owner_caller(*access.user, true, false);
		if (c.refusal) return std::move(*c.refusal);
		return chat_send(c, *command);
	});

	// GET /api/me/server/console — the account's newest server's console, in any
	// state (a stopped or failed one's last lines included).
	CROW_ROUTE(app, "/api/me/server/console").methods("GET"_method)([this](const crow::request &req) -> crow::response {
		auto access = access_.require_user(req);
		if (access.refusal) return std::move(*access.refusal);
		Caller c;
		try {
			const auto server = latest_server_of(*pool_.acquire(), access.user->id);
			if (!server) return json_error(404, "no_server", "this account has no hosted server");
			c.server_id = server->id;
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[http] hosted console lookup failed: %s\n", e.what());
			return json_error(500, "db_error");
		}
		if (const int64_t wait = owner_reads_.take("console|" + std::to_string(access.user->id), RateLimiter::Clock::now()))
			return too_many_requests(wait);
		return console(c, req);
	});
}

void ConsoleRoutes::register_admin(WebApp &app) {
	using Work = crow::response (ConsoleRoutes::*)(const Caller &);
	const auto simple = [this](const crow::request &req, uint64_t id, Work work) -> crow::response {
		auto access = access_.require_admin(req);
		if (access.refusal) return std::move(*access.refusal);
		Caller c = admin_caller(access, id, false);
		if (c.refusal) return std::move(*c.refusal);
		return (this->*work)(c);
	};
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/players").methods("GET"_method)([simple](const crow::request &req, uint64_t id) {
		return simple(req, id, &ConsoleRoutes::players);
	});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/missions").methods("GET"_method)([simple](const crow::request &req, uint64_t id) {
		return simple(req, id, &ConsoleRoutes::missions);
	});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/rotation").methods("GET"_method)([simple](const crow::request &req, uint64_t id) {
		return simple(req, id, &ConsoleRoutes::rotation);
	});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/rotation/cycle").methods("POST"_method)([simple](const crow::request &req, uint64_t id) {
		return simple(req, id, &ConsoleRoutes::rotation_cycle);
	});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/chat").methods("GET"_method)([simple](const crow::request &req, uint64_t id) {
		return simple(req, id, &ConsoleRoutes::chat);
	});
	// The admins' one wider read.
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/settings").methods("GET"_method)([simple](const crow::request &req, uint64_t id) {
		return simple(req, id, &ConsoleRoutes::settings);
	});

	const auto player = [this](const crow::request &req, uint64_t id, uint64_t slot, ConsoleVerb verb) -> crow::response {
		auto access = access_.require_admin(req);
		if (access.refusal) return std::move(*access.refusal);
		ConsoleRefusal why;
		const auto command = console_player_command(verb, slot, why);
		if (!command) return refused(why);
		Caller c = admin_caller(access, id, false);
		if (c.refusal) return std::move(*c.refusal);
		return player_action(c, *command, slot);
	};
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/players/<uint>/punt")
			.methods("POST"_method)([player](const crow::request &req, uint64_t id, uint64_t slot) {
				return player(req, id, slot, ConsoleVerb::PlayerPunt);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/players/<uint>/ban")
			.methods("POST"_method)([player](const crow::request &req, uint64_t id, uint64_t slot) {
				return player(req, id, slot, ConsoleVerb::PlayerBan);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/players/<uint>/swap")
			.methods("POST"_method)([player](const crow::request &req, uint64_t id, uint64_t slot) {
				return player(req, id, slot, ConsoleVerb::PlayerSwapTeam);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/players/<uint>/kill")
			.methods("POST"_method)([player](const crow::request &req, uint64_t id, uint64_t slot) {
				return player(req, id, slot, ConsoleVerb::PlayerKill);
			});

	const auto entry = [this](const crow::request &req, uint64_t id, uint64_t index, ConsoleVerb verb) -> crow::response {
		auto access = access_.require_admin(req);
		if (access.refusal) return std::move(*access.refusal);
		ConsoleRefusal why;
		const auto command = console_rotation_command(verb, index, why);
		if (!command) return refused(why);
		Caller c = admin_caller(access, id, true);
		if (c.refusal) return std::move(*c.refusal);
		return rotation_entry(c, *command, index);
	};
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/rotation/<uint>/remove")
			.methods("POST"_method)([entry](const crow::request &req, uint64_t id, uint64_t index) {
				return entry(req, id, index, ConsoleVerb::MissionRemove);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/rotation/<uint>/next")
			.methods("POST"_method)([entry](const crow::request &req, uint64_t id, uint64_t index) {
				return entry(req, id, index, ConsoleVerb::MissionSetNext);
			});

	CROW_ROUTE(app, "/api/admin/hosted/<uint>/rotation")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) -> crow::response {
				auto access = access_.require_admin(req);
				if (access.refusal) return std::move(*access.refusal);
				auto body = JsonBody::parse(req.body);
				if (!body) return json_error(400, "invalid_json");
				const auto mission = body->text("mission");
				const bool swap_sides = body->flag("swap_sides").value_or(false);
				if (body->wrong_type()) return json_error(400, "invalid_field");
				if (!mission) return json_error(400, "missing_field", "\"mission\" (a .bms file name) is required");
				ConsoleRefusal why;
				const auto command = console_mission_add(*mission, swap_sides, why);
				if (!command) return refused(why);
				Caller c = admin_caller(access, id, true);
				if (c.refusal) return std::move(*c.refusal);
				return rotation_add(c, *command, *mission);
			});
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/chat")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) -> crow::response {
				auto access = access_.require_admin(req);
				if (access.refusal) return std::move(*access.refusal);
				auto body = JsonBody::parse(req.body);
				if (!body) return json_error(400, "invalid_json");
				const auto text = body->text("text");
				if (body->wrong_type()) return json_error(400, "invalid_field");
				if (!text) return json_error(400, "missing_field", "\"text\" is required");
				ConsoleRefusal why;
				const auto command = console_chat_send(*text, why);
				if (!command) return refused(why);
				Caller c = admin_caller(access, id, false);
				if (c.refusal) return std::move(*c.refusal);
				return chat_send(c, *command);
			});

	// GET /api/admin/hosted/<id>/console — any server's console, in any state.
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/console")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) -> crow::response {
				auto access = access_.require_admin(req);
				if (access.refusal) return std::move(*access.refusal);
				Caller c;
				try {
					const auto server = find_server(*pool_.acquire(), server_id_of(id));
					if (!server) return json_error(404, "not_found", "no hosted server has this id");
					c.server_id = server->id;
				} catch (const db::SqliteError &e) {
					std::fprintf(stderr, "[http] hosted console lookup failed: %s\n", e.what());
					return json_error(500, "db_error");
				}
				return console(c, req);
			});
}

} // namespace opennova::novaworld_server::hosted
