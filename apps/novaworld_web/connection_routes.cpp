#include "connection_routes.h"

#include "accounts/auth.h"
#include "core/loggable.h"
#include "host_routes.h"
#include "hosted/hosted_routes.h" // live_second_listing
#include "hosted/model/hosted_repository.h"
#include "http_json.h"
#include "program/catalog_repository.h"

#include <base/io/cp1252.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include <algorithm>
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

// `out` the game's {slug, display_name}, or null for none.
void set_title(crow::json::wvalue &out, const catalog::GameRow *game) {
	if (game == nullptr) {
		out = nullptr;
		return;
	}
	out["slug"] = game->slug;
	out["display_name"] = game->display_name;
}

// `out` the id, or null for none.
void set_id(crow::json::wvalue &out, std::optional<int64_t> id) {
	if (id)
		out = *id;
	else
		out = nullptr;
}

// The listing on NovaLogic's NovaWorld of the hosted server `hosted_server_id` (none: no
// hosted server) as its agent last reported it (hosted_routes.h live_second_listing): its
// `slots` are the PlayerList slots of the players that service sent, who hold no session here
// though the server's PlayerList counts them. nullopt for no live hosted server listed there.
std::optional<hosted::SecondListingStatus> novalogic_listing_of(
		db::Database &db, std::optional<int64_t> hosted_server_id) {
	const auto server =
			hosted_server_id ? hosted::find_server(db, *hosted_server_id) : std::nullopt;
	return server ? hosted::live_second_listing(db, *server) : std::nullopt;
}

// One person of GET /api/admin/online before it is JSON.
struct OnlinePerson {
	std::string name;
	std::optional<UserRecord> account;
	const catalog::GameRow *game = nullptr;
	bool playing = false;
	const hostdb::HostRow *server = nullptr; // the listing it plays on, null for none
	std::optional<int64_t> hosted_server_id;
	std::optional<int64_t> since_ms;
	bool novalogic = false; // sent by NovaLogic's NovaWorld: no session here
	std::optional<int64_t> session_id;
	std::string client; // the session's AP, UTF-8
};

crow::json::wvalue online_json(const OnlinePerson &p) {
	crow::json::wvalue e;
	e["name"] = p.name;
	if (p.account) {
		crow::json::wvalue a;
		a["id"] = p.account->id;
		a["username"] = p.account->username;
		a["nwhandle"] = p.account->nwhandle;
		e["account"] = std::move(a);
	} else {
		e["account"] = nullptr;
	}
	set_title(e["game"], p.game);
	e["state"] = p.playing ? "playing" : "lobby";
	if (p.server != nullptr) {
		crow::json::wvalue server;
		server["rid"] = p.server->rid;
		server["name"] = retail_text_to_utf8(p.server->server_name);
		server["mission"] = retail_text_to_utf8(p.server->mission_name);
		server["game_type"] = retail_text_to_utf8(p.server->game_type);
		set_id(server["hosted_server_id"], p.hosted_server_id);
		e["server"] = std::move(server);
	} else {
		e["server"] = nullptr;
	}
	set_id(e["since_ms"], p.since_ms);
	e["via"] = p.novalogic ? "novalogic" : "opennova";
	set_id(e["session_id"], p.session_id);
	if (p.client.empty())
		e["client"] = nullptr;
	else
		e["client"] = p.client;
	return e;
}

} // namespace

void ConnectionRoutes::register_routes(WebApp &app, uint64_t heartbeat_timeout_ms) {
	heartbeat_timeout_ms_ = heartbeat_timeout_ms;
	CROW_ROUTE(app, "/api/admin/connections")
			.methods("GET"_method)([this](const crow::request &req) { return list(req); });
	CROW_ROUTE(app, "/api/admin/online").methods("GET"_method)([this](const crow::request &req) {
		return online(req);
	});
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
// the instances), the lobby instance holding it (lobby_id) and the task that
// instance runs as (lobby_task_id, "" for none: the program usage page's key),
// its registry id (id: unique within its instance alone), state, the protocol
// name (pn) and NA its hello and auth carried (identity), the client's executable
// its Hello's AP named (ap, UTF-8: "JOINTOPS.EXE", "OPENNOVA.EXE"), its address, and when
// it connected and was last heard from (created_ms, last_seen_ms; the lobby
// writes the last within ten seconds) as Unix epoch ms; "kind", "server" for a
// session that hosts (a listen host plays on its own server too) and "client"
// for any other; "title", the game it speaks ({slug, display_name} of its games
// row: a server's by its listing's LobbyName, else any session's by its game
// tag; null for a tag no row answers to); "account", who the
// session is; "host", the server it lists; and "playing", the server it plays
// on. The account is the listing's owner while it hosts (listing_owner, "via":
// "host_key"), else the account it signed in as (the row's user_id, pinned by
// the lobby from its Cookie list's retail login, "via": "login"), else null.
// The host is null unless it hosts: {rid, server_name, mission_name (UTF-8, as
// every list of listed servers sends a host's text), players, max_players,
// listed (false while it waits out of the browser for a HostKey naming an
// account), hosted_server_id (null for a listing that is no live hosted
// server), server_kind ("hosted" for a live hosted server of the fleet's,
// "owned" for a listing its HostKey names an account for, "anonymous" for
// one it names none), dedicated (its listing's Dedicated column: true for a
// Serve Only host, false for a listen host; null while it is not listed),
// novalogic_players (a hosted server listed on NovaLogic's NovaWorld too: the
// players in its match that service sent, who hold no session here though its
// PlayerList counts them, hosted_routes.h live_second_listing; null for any other
// server, and while its agent reports none)}.
// Playing is null unless it plays (its latest ClientPlayRequest, not stopped):
// {rid (the listing at its PlaySetup's address when it asked; null for none),
// addr (that address), since_ms, listed (whether that listing is still in the
// browser), server_name (the listing's, UTF-8; null when it is not),
// hosted_server_id (as the host's)}. All of it from the lobby's session rows
// and the listings, in one snapshot.
crow::response ConnectionRoutes::list(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::vector<crow::json::wvalue> entries;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot read(*db_conn);
		const catalog::GameCatalog games = catalog::GameCatalog::load(*db_conn);
		std::map<uint32_t, hostdb::HostRow> listings;
		for (hostdb::HostRow &h : hostdb::list_hosts(*db_conn))
			listings.emplace(h.rid, std::move(h));
		std::map<int64_t, std::string> tasks;
		for (const lobbydb::LobbyInstance &i : lobbydb::list_instances(*db_conn))
			tasks[i.id] = i.task_id;
		// The live hosted server a listing is, by its owner (listing_owner): once per RID.
		std::map<uint32_t, std::optional<int64_t>> hosted_of;
		const auto hosted_server_of = [&](uint32_t rid, std::optional<int64_t> host_user_id) {
			const auto known = hosted_of.find(rid);
			if (known != hosted_of.end())
				return known->second;
			const std::optional<int64_t> id = listing_owner(*db_conn, host_user_id).hosted_server_id;
			hosted_of.emplace(rid, id);
			return id;
		};
		for (const lobbydb::SessionRow &s : lobbydb::list_sessions(*db_conn)) {
			const auto listing = s.hosting ? listings.find(s.rid) : listings.end();
			crow::json::wvalue e;
			e["session_id"] = s.id;
			e["lobby_id"] = s.lobby_id;
			const auto task = tasks.find(s.lobby_id);
			e["lobby_task_id"] = task == tasks.end() ? std::string() : task->second;
			e["id"] = s.connection_id;
			e["state"] = s.state;
			e["pn"] = s.pn;
			e["identity"] = s.identity;
			e["ap"] = retail_text_to_utf8(s.ap);
			e["created_ms"] = s.connected_ms;
			e["last_seen_ms"] = s.last_seen_ms;
			e["addr"] = lobbydb::session_addr(s);
			e["kind"] = s.hosting ? "server" : "client";
			const catalog::GameRow *game = listing != listings.end() && !listing->second.game.empty()
					? games.by_lobby_name(listing->second.game)
					: nullptr;
			set_title(e["title"], game != nullptr ? game : games.by_gate_tag(s.identity));
			e["account"] = nullptr;
			e["host"] = nullptr;
			e["playing"] = nullptr;
			if (s.hosting) {
				const ListingOwner owner = listing_owner(*db_conn, s.host_user_id);
				hosted_of.emplace(s.rid, owner.hosted_server_id);
				crow::json::wvalue host;
				host["rid"] = s.rid;
				host["server_name"] = retail_text_to_utf8(s.server_name);
				host["mission_name"] = retail_text_to_utf8(s.mission_name);
				host["players"] = s.players;
				host["max_players"] = s.max_players;
				host["listed"] = s.listed;
				set_id(host["hosted_server_id"], owner.hosted_server_id);
				host["server_kind"] = owner.hosted_server_id ? "hosted"
						: s.host_user_id                     ? "owned"
															 : "anonymous";
				if (listing != listings.end())
					host["dedicated"] = listing->second.dedicated != "N";
				else
					host["dedicated"] = nullptr;
				// The players NovaLogic's NovaWorld sent a hosted server listed there too: they
				// hold no session here, though its PlayerList counts them.
				const auto novalogic = novalogic_listing_of(*db_conn, owner.hosted_server_id);
				if (novalogic)
					host["novalogic_players"] = novalogic->slots.size();
				else
					host["novalogic_players"] = nullptr;
				e["host"] = std::move(host);
				if (owner.owner)
					e["account"] = account_json(*owner.owner, "host_key");
			}
			if (e["account"].t() == crow::json::type::Null && s.user_id) {
				if (const auto user = get_user_by_id(*db_conn, *s.user_id))
					e["account"] = account_json(*user, "login");
			}
			if (s.playing_since_ms != 0) {
				const auto on = s.playing_rid != 0 ? listings.find(s.playing_rid) : listings.end();
				crow::json::wvalue playing;
				if (s.playing_rid != 0)
					playing["rid"] = s.playing_rid;
				else
					playing["rid"] = nullptr;
				playing["addr"] = s.play_addr;
				playing["since_ms"] = s.playing_since_ms;
				playing["listed"] = on != listings.end();
				if (on != listings.end()) {
					playing["server_name"] = retail_text_to_utf8(on->second.server_name);
					set_id(playing["hosted_server_id"],
							hosted_server_of(on->first, on->second.host_user_id));
				} else {
					playing["server_name"] = nullptr;
					playing["hosted_server_id"] = nullptr;
				}
				e["playing"] = std::move(playing);
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

// GET /api/admin/online — {"count", "players"}: one row per person on our NovaWorld now, those
// playing first (by their server's name, then their own), then those in the lobby (by name). A
// person is a client session (one that hosts is a server, and a hosted server's service
// account no person) or a player NovaLogic's NovaWorld sent one of our hosted servers listed
// there too: the roster rows at the PlayerList slots its agent reports (the connections list's
// novalogic_players), who hold no session here. Each: its "name" (the account's handle, else
// the roster's PlayerName, UTF-8; "" for a session signed in to no account), "account" ({id,
// username, nwhandle}: the account the session signed in as, or a NovaLogic player's whose pcid
// its PlayerPCID is; null for none), "game" ({slug, display_name} as the connections list's
// title: a session's by its game tag, else by the LobbyName of the listing it plays on; a
// NovaLogic player's by its server's; null for none), "state" ("playing" or "lobby"), "server"
// (the listing it plays on: {rid, name, mission, game_type (UTF-8), hosted_server_id (null for
// a listing that is no live hosted server)}; null in the lobby and at an address no listing
// has), "since_ms" (since when it plays, else when its session connected, Unix epoch ms; null
// for a NovaLogic player), "via" ("opennova", or "novalogic" for a player that service sent),
// "session_id" (its session row's, what POST /api/admin/connections/<id>/end takes; null for a
// NovaLogic player) and "client" (the executable its Hello's AP named, UTF-8; null for none).
// All of it in one snapshot.
crow::response ConnectionRoutes::online(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::vector<crow::json::wvalue> entries;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot read(*db_conn);
		const catalog::GameCatalog games = catalog::GameCatalog::load(*db_conn);
		std::map<uint32_t, hostdb::HostRow> listings;
		for (hostdb::HostRow &h : hostdb::list_hosts(*db_conn))
			listings.emplace(h.rid, std::move(h));
		// The live hosted server a listing is, by its owner (listing_owner): once per RID.
		std::map<uint32_t, std::optional<int64_t>> hosted_of;
		const auto hosted_server_of = [&](uint32_t rid, std::optional<int64_t> host_user_id) {
			const auto known = hosted_of.find(rid);
			if (known != hosted_of.end())
				return known->second;
			const std::optional<int64_t> id =
					listing_owner(*db_conn, host_user_id).hosted_server_id;
			hosted_of.emplace(rid, id);
			return id;
		};
		const std::vector<lobbydb::SessionRow> sessions = lobbydb::list_sessions(*db_conn);
		std::vector<OnlinePerson> people;
		for (const lobbydb::SessionRow &s : sessions) {
			if (s.hosting)
				continue;
			OnlinePerson p;
			if (s.user_id) {
				p.account = get_user_by_id(*db_conn, *s.user_id);
				if (p.account && p.account->kind != "player")
					continue;
			}
			if (p.account)
				p.name = p.account->nwhandle;
			p.session_id = s.id;
			p.client = retail_text_to_utf8(s.ap);
			p.playing = s.playing_since_ms != 0;
			p.since_ms = p.playing ? s.playing_since_ms : s.connected_ms;
			const auto on =
					p.playing && s.playing_rid != 0 ? listings.find(s.playing_rid) : listings.end();
			if (on != listings.end()) {
				p.server = &on->second;
				p.hosted_server_id = hosted_server_of(on->first, on->second.host_user_id);
			}
			p.game = games.by_gate_tag(s.identity);
			if (p.game == nullptr && p.server != nullptr && !p.server->game.empty())
				p.game = games.by_lobby_name(p.server->game);
			people.push_back(std::move(p));
		}
		// The players NovaLogic's NovaWorld sent: read from the hosting sessions, as the
		// connections list reads its novalogic_players.
		std::map<uint32_t, std::vector<int>> novalogic_slots;
		for (const lobbydb::SessionRow &s : sessions) {
			if (!s.hosting || listings.count(s.rid) == 0)
				continue;
			const std::optional<int64_t> id =
					listing_owner(*db_conn, s.host_user_id).hosted_server_id;
			hosted_of[s.rid] = id;
			if (const auto novalogic = novalogic_listing_of(*db_conn, id))
				novalogic_slots[s.rid] = novalogic->slots;
		}
		std::vector<uint32_t> rids;
		for (const auto &listed : novalogic_slots)
			rids.push_back(listed.first);
		const auto rosters = hostdb::list_rosters(*db_conn, rids);
		for (const auto &[rid, slots] : novalogic_slots) {
			const auto roster = rosters.find(rid);
			if (roster == rosters.end())
				continue;
			const hostdb::HostRow &listing = listings.at(rid);
			for (const HostRosterSlot &slot : roster->second) {
				if (std::find(slots.begin(), slots.end(), slot.slot) == slots.end())
					continue;
				OnlinePerson p;
				p.account = get_user_by_pcid(*db_conn, slot.pcid);
				p.name = p.account && !p.account->nwhandle.empty()
						? p.account->nwhandle
						: retail_text_to_utf8(slot.player_name);
				p.game = listing.game.empty() ? nullptr : games.by_lobby_name(listing.game);
				p.playing = true;
				p.server = &listing;
				p.hosted_server_id = hosted_of[rid];
				p.novalogic = true;
				people.push_back(std::move(p));
			}
		}
		std::stable_sort(
				people.begin(), people.end(), [](const OnlinePerson &a, const OnlinePerson &b) {
					if (a.playing != b.playing)
						return a.playing;
					const std::string a_server =
							a.server != nullptr ? a.server->server_name : std::string();
					const std::string b_server =
							b.server != nullptr ? b.server->server_name : std::string();
					if (a_server != b_server)
						return a_server < b_server;
					return a.name < b.name;
				});
		for (const OnlinePerson &p : people)
			entries.push_back(online_json(p));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/online failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["count"] = entries.size();
	out["players"] = std::move(entries);
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
// "draining" does not), the task its lobby runs as (task_id, "" for none: the
// program usage page's key), when it
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
			e["task_id"] = i.task_id;
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
