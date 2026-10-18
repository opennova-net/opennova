#include "lobby_routes.h"

#include "hosted/console_commands.h"
#include "hosted/console_queue.h"
#include "hosted/data_set_repository.h"
#include "hosted/hosted_repository.h"
#include "http_json.h"
#include "program/catalog_repository.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <net/admin/admin_protocol.h>
#include <net/novaworld/host_repository.h>

#include <climits>
#include <cstdio>
#include <iterator>
#include <utility>

namespace opennova::novaworld_server {

namespace {

using Clock = std::chrono::steady_clock;

// A scoreboard nobody asked for this long is dropped (its server likely ended).
constexpr std::chrono::minutes kScoreboardKept{ 10 };

// An expansion's name by its base's games slug and its /exp code, the code a
// listing's Exp column carries ("jox01": "Escalation").
using ExpansionNames = std::map<std::pair<std::string, std::string>, std::string>;

ExpansionNames expansion_names(db::Database &db) {
	ExpansionNames out;
	const auto sets = hosted::list_data_sets(db);
	std::map<int64_t, std::string> base_game;
	for (const auto &d : sets)
		if (d.kind == hosted::DataSetKind::Base) base_game[d.id] = d.game;
	for (const auto &d : sets)
		if (d.kind == hosted::DataSetKind::Expansion && d.parent_id)
			out[{ base_game[*d.parent_id], d.expansion_code }] = d.display_name;
	return out;
}

// A listing as the lobby's card shows it. camelCase to match the web LobbyHost
// type (snake_case rendered every host as "Unnamed Server 0/0"). hostIp/hostPort
// are the join address. The host's own text comes in its code page (a stock
// host's cp1252) and goes out as UTF-8; the GSB keeps the host's bytes for stock
// clients. `slug` is its game's.
crow::json::wvalue card_of(const hostdb::HostRow &h, const ExpansionNames &names, const std::string &slug) {
	crow::json::wvalue card;
	card["id"]         = h.rid;
	card["serverName"] = retail_text_to_utf8(h.server_name);
	card["hostIp"]     = retail_text_to_utf8(h.host_ip);
	card["hostPort"]   = h.host_port;
	card["players"]    = h.player_count;
	card["maxPlayers"] = h.max_players;
	card["region"]     = retail_text_to_utf8(h.region);
	card["mission"]    = retail_text_to_utf8(h.mission_name);
	card["gameType"]   = retail_text_to_utf8(h.game_type);
	// The expansion a joiner needs installed ("" for the base game alone), by
	// its data set's name, else its code.
	const auto named   = names.find({ slug, h.exp });
	card["expansion"]  = named != names.end() ? named->second : retail_text_to_utf8(h.exp);
	card["locked"]     = h.locked == "Y";
	return card;
}

} // namespace

void LobbyRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/lobbies")([this]() { return list(); });
	CROW_ROUTE(app, "/api/lobbies/<uint>")([this](uint64_t rid) { return one(rid); });
}

// GET /api/lobbies — {"games": [{"slug", "displayName", "hosts": [cards]}]}:
// one group per games row, live from the table; a listing lands under the row
// whose LobbyName it registered with (active_hosts.game).
crow::response LobbyRoutes::list() {
	std::vector<crow::json::wvalue> games_json;
	try {
		auto db_conn = pool_.acquire();
		const auto games = catalog::list_games(*db_conn);
		const auto host_rows = hostdb::list_hosts(*db_conn);
		const auto names = expansion_names(*db_conn);
		games_json.reserve(games.size());
		for (const auto &g : games) {
			crow::json::wvalue game;
			game["slug"]        = g.slug;
			game["displayName"] = g.display_name;
			std::vector<crow::json::wvalue> hosts;
			for (const auto &h : host_rows) {
				if (h.game != g.lobby_name) continue;
				hosts.push_back(card_of(h, names, g.slug));
			}
			game["hosts"] = std::move(hosts);
			games_json.push_back(std::move(game));
		}
	} catch (const db::Error &e) {
		// An outage answers 500 like every other route, never an empty
		// list the lobby page would show as "no servers".
		std::fprintf(stderr, "[http] /api/lobbies failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::json::wvalue out;
	out["games"] = std::move(games_json);
	return json_reply(200, out);
}

// GET /api/lobbies/<rid> — one listing: its card, "game" ({slug,
// displayName}), "timeLeft" (minutes; null when the host gave none),
// "timeOfDay", "message", "roster" (the player names its host's status
// reports carry) and "scoreboard" (a running hosted server's players with
// team, kills and deaths, read over its admin link at most every
// kScoreboardFresh; null for a player's own host, or while no read has
// answered). 404 not_found once it is no longer listed.
crow::response LobbyRoutes::one(uint64_t rid) {
	if (rid > UINT32_MAX) return json_error(404, "not_found", "no server is listed with this id");
	crow::json::wvalue out;
	std::optional<int64_t> hosted_server;
	try {
		auto db = pool_.acquire();
		const auto row = hostdb::find_host_by_rid(*db, static_cast<uint32_t>(rid));
		if (!row) return json_error(404, "not_found", "no server is listed with this id");
		std::string slug;
		std::string display_name;
		for (const auto &g : catalog::list_games(*db)) {
			if (g.lobby_name != row->game) continue;
			slug = g.slug;
			display_name = g.display_name;
		}
		out = card_of(*row, expansion_names(*db), slug);
		out["game"]["slug"]        = slug;
		out["game"]["displayName"] = display_name;
		if (const auto minutes = strutil::parse_int(row->time_left)) out["timeLeft"] = *minutes;
		else out["timeLeft"] = nullptr;
		out["timeOfDay"] = retail_text_to_utf8(row->time_of_day);
		out["message"]   = retail_text_to_utf8(row->msg);
		std::vector<crow::json::wvalue> roster;
		for (const auto &p : hostdb::list_players(*db, row->rid)) roster.emplace_back(retail_text_to_utf8(p.nwhandle));
		out["roster"] = std::move(roster);
		if (row->host_user_id) {
			const auto account = hosted::hosted_server_of_account(*db, *row->host_user_id);
			if (account && account->state == hosted::State::Running) hosted_server = account->server_id;
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] /api/lobbies/%llu failed: %s\n", static_cast<unsigned long long>(rid), e.what());
		return json_error(500, "db_error");
	}
	// Read with the connection given back: the read waits on the server.
	const auto scores = hosted_server ? scoreboard_of(*hosted_server) : std::nullopt;
	if (scores) {
		std::vector<crow::json::wvalue> rows;
		for (const ScoreRow &s : *scores) {
			crow::json::wvalue r;
			r["name"]   = s.name;
			r["team"]   = s.team;
			r["kills"]  = s.kills;
			r["deaths"] = s.deaths;
			rows.push_back(std::move(r));
		}
		out["scoreboard"] = std::move(rows);
	} else {
		out["scoreboard"] = nullptr;
	}
	return json_reply(200, out);
}

std::optional<std::vector<LobbyRoutes::ScoreRow>> LobbyRoutes::scoreboard_of(int64_t server_id) {
	const auto now = Clock::now();
	{
		std::lock_guard<std::mutex> lock(mu_);
		for (auto it = scoreboards_.begin(); it != scoreboards_.end();)
			it = !it->second.in_flight && now - it->second.read_at > kScoreboardKept ? scoreboards_.erase(it) : std::next(it);
		Scoreboard &s = scoreboards_[server_id];
		const bool fresh = s.read_at != Clock::time_point{} && now - s.read_at < kScoreboardFresh;
		// Fresh, or another request reads it now: this one takes what there is.
		if (fresh || s.in_flight) return s.read ? std::optional<std::vector<ScoreRow>>(s.rows) : std::nullopt;
		s.in_flight = true;
	}
	std::optional<std::vector<ScoreRow>> rows;
	try {
		hosted::AdminCommandOptions options;
		options.record = false; // a read: the console keeps the writes
		const auto result = hosted::run_console_command(pool_, server_id,
				hosted::console_command(hosted::ConsoleVerb::PlayerList)->line, options, kScoreboardWait);
		if (result.outcome == hosted::AdminCommandOutcome::Answered && !result.replies.empty()) {
			rows.emplace();
			// A hosted server is an opennova-serve, a Serve Only host: its slot 0 is its own row.
			for (const AdminPlayer &p : admin_parse_player_list(result.replies.front(), AdminServerRow::Dedicated))
				rows->push_back({ retail_text_to_utf8(p.name), strutil::parse_int(p.team).value_or(0), p.kills, p.deaths });
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] the scoreboard of hosted server %lld failed: %s\n",
				static_cast<long long>(server_id), e.what());
	}
	std::lock_guard<std::mutex> lock(mu_);
	Scoreboard &s = scoreboards_[server_id];
	s.in_flight = false;
	// A read that failed waits its turn too, so a silent server is not asked on
	// every request.
	s.read_at = Clock::now();
	if (rows) {
		s.rows = *rows;
		s.read = true;
	}
	return s.read ? std::optional<std::vector<ScoreRow>>(s.rows) : std::nullopt;
}

} // namespace opennova::novaworld_server
