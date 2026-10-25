#include "novalogic_lobbies.h"

#include "core/health.h" // Watchdog::steady_now_ms
#include "core/loggable.h"
#include "hosted/model/stats_repository.h" // iso_utc
#include "net_sockets.h"

#include <base/io/cp1252.h>
#include <net/novaworld/server_browser.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// The most rows a list keeps: well past the live feed's (52 to 58 rows in the record's
// fetches), so a runaway reply never reaches the page whole.
constexpr size_t kMaxServers = 1000;

int64_t wall_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

int64_t steady_now_ms() { return static_cast<int64_t>(Watchdog::steady_now_ms()); }

} // namespace

net::HttpReply NovaLogicLobbies::fetch_novalogic() {
	return net::http_exchange("GET", kUrl, { kUserAgent }, {}, kFetchTimeoutMs,
			[](const std::string &host, net::Endpoint &out) { return net::resolve_ipv4(host, out); },
			kMaxReplyBytes, kFetchTotalMs);
}

NovaLogicLobbies::NovaLogicLobbies(Fetch fetch, Clock wall_ms, Clock steady_ms) :
		fetch_(std::move(fetch)),
		wall_(wall_ms ? std::move(wall_ms) : Clock(wall_now_ms)),
		steady_(steady_ms ? std::move(steady_ms) : Clock(steady_now_ms)) {}

NovaLogicLobbies::~NovaLogicLobbies() { settle(); }

NovaLogicList NovaLogicLobbies::read() {
	std::lock_guard<std::mutex> lock(mu_);
	const int64_t now = steady_();
	const bool busy = inflight_.valid() &&
			inflight_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
	const bool due = !attempted_ || now - attempted_steady_ms_ >= kRefreshMs;
	if (!busy && due) {
		if (inflight_.valid()) inflight_.get(); // the last one's, done
		attempted_ = true;
		attempted_steady_ms_ = now;
		try {
			inflight_ = std::async(std::launch::async, [this] { refresh(); });
			++fetches_;
		} catch (const std::system_error &e) {
			// No thread: no fetch this time; the next is due a kRefreshMs on.
			std::fprintf(stderr, "[http] NovaLogic's server list: no fetch started (%s)\n", e.what());
		}
	}
	NovaLogicList out;
	out.fetched = fetched_;
	out.fetched_at_ms = fetched_wall_ms_;
	out.age_ms = fetched_ ? now - fetched_steady_ms_ : 0;
	out.failing = failing_;
	out.servers = servers_;
	return out;
}

void NovaLogicLobbies::settle() {
	std::future<void> inflight;
	{
		std::lock_guard<std::mutex> lock(mu_);
		inflight = std::move(inflight_);
	}
	if (inflight.valid()) inflight.wait();
}

uint64_t NovaLogicLobbies::fetches() const {
	std::lock_guard<std::mutex> lock(mu_);
	return fetches_;
}

void NovaLogicLobbies::refresh() {
	net::HttpReply reply;
	try {
		reply = fetch_();
	} catch (const std::exception &e) {
		reply = net::HttpReply{};
		reply.error = e.what();
	}
	GsbResponse parsed;
	std::string why;
	if (!reply.transport_ok)
		why = reply.error.empty() ? "no reply" : reply.error;
	else if (reply.code != 200)
		why = "HTTP " + std::to_string(reply.code);
	else if (reply.body.size() > kMaxReplyBytes)
		why = "the reply is too large (" + std::to_string(reply.body.size()) + " bytes)";
	else {
		try {
			if (!gsb_parse_response(reply.body.data(), reply.body.size(), parsed))
				why = "the reply is no server list (" + std::to_string(reply.body.size()) + " bytes)";
		} catch (...) {
			// Whatever the parse throws (an allocation) is a failed fetch, never the reader's.
			why = "the reply did not parse";
		}
	}
	if (parsed.servers.size() > kMaxServers) parsed.servers.resize(kMaxServers);
	// The route never shows a host's address: the list keeps none. Its players' names it shows
	// (the server's page lists them, as the game's browser does).
	for (GsbServerEntry &s : parsed.servers) s.ip.clear();

	std::lock_guard<std::mutex> lock(mu_);
	if (!why.empty()) {
		// The last good list stays, with its time.
		std::fprintf(stderr, "[http] NovaLogic's server list: %s; the last list kept\n",
				loggable(why, 160).c_str());
		failing_ = true;
		return;
	}
	if (!fetched_ || failing_)
		std::printf("[http] NovaLogic's server list: %zu servers\n", parsed.servers.size());
	failing_ = false;
	fetched_ = true;
	fetched_wall_ms_ = wall_();
	fetched_steady_ms_ = steady_();
	servers_ = std::move(parsed.servers);
}

crow::json::wvalue novalogic_lobbies_json(const NovaLogicList *list) {
	crow::json::wvalue out;
	out["enabled"] = list != nullptr;
	std::vector<crow::json::wvalue> servers;
	if (list != nullptr) {
		servers.reserve(list->servers.size());
		for (const GsbServerEntry &s : list->servers) {
			crow::json::wvalue row;
			row["id"]         = s.rid;
			row["serverName"] = retail_text_to_utf8(s.server_name);
			row["mission"]    = retail_text_to_utf8(s.mission_name);
			row["gameType"]   = retail_text_to_utf8(s.game_type);
			row["players"]    = s.players;
			row["maxPlayers"] = s.max_players;
			row["locked"]     = browser_row_is_locked(s);
			// The server's page: the row's other columns as the game's browser has them, the
			// game's inline markup left in for the page to draw.
			row["password"]   = s.password == "Y";
			row["message"]    = retail_text_to_utf8(s.msg);
			row["country"]    = retail_text_to_utf8(s.country);
			row["region"]     = retail_text_to_utf8(s.region);
			row["timeLeft"]   = retail_text_to_utf8(s.time_left);
			row["timeOfDay"]  = retail_text_to_utf8(s.time_of_day);
			row["expansion"]  = retail_text_to_utf8(s.exp);
			row["punkBuster"] = !s.pb_server.empty() && s.pb_server != "0";
			row["dedicated"]  = s.dedicated == "Y";
			row["tracers"]    = s.tracers == "Y";
			std::vector<crow::json::wvalue> roster;
			roster.reserve(s.player_names.size());
			for (const std::string &name : s.player_names) roster.emplace_back(retail_text_to_utf8(name));
			row["roster"] = std::move(roster);
			servers.push_back(std::move(row));
		}
	}
	out["servers"] = std::move(servers);
	if (list != nullptr && list->fetched) {
		out["fetchedAt"]  = hosted::iso_utc(list->fetched_at_ms / 1000);
		out["ageSeconds"] = list->age_ms / 1000;
	} else {
		out["fetchedAt"]  = nullptr;
		out["ageSeconds"] = nullptr;
	}
	out["failing"] = list != nullptr && list->failing;
	return out;
}

} // namespace opennova::novaworld_server
