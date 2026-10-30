#pragma once

// NovaLogic's NovaWorld server list for the Lobbies page (GET /api/lobbies/novalogic,
// lobby_routes.h): the Joint Operations servers a stock client's browser lists from
// NovaLogic's live service, read the way that browser reads them. The list is one plain
// GET of the title's GSB feed on NovaLogic's web host, with no login and no cookie: the
// web host is the SessionInit web domain, 207.178.209.204:80, and the stock client's list
// load is `GET /jop_2.gsb?a=1` there, which answers unauthenticated
// (docs/net/novaworld-net-re.md §7 Wave 4, "CONFIRMED LIVE against .204" and the retail
// capture's step 5; the feed's bytes §2 "GSB server row"). Its rows are parsed with
// gsb_parse_response (net/novaworld/gsb.h). Only JO's feed is witnessed: no DFX2 page past
// the start was ever captured from NovaLogic's service (docs/titles/dfx2-vs-jo-re.md §6.4).
//
// Behind ONNET_NOVALOGIC_LOBBIES (ServerConfig::novalogic_lobbies, off when unset): web
// builds this list only with it on, so no test, dev stack or CI run contacts NovaLogic.
//
// The cache: read() answers the last list fetched and never waits on NovaLogic. When that
// list is older than kRefreshMs, or there is none, and no fetch is in flight, the read
// starts one on a thread of its own. So a web process fetches at most once per kRefreshMs
// however many visitors look, and not at all while nobody does. A fetch that fails (no
// answer, a status other than 200, bytes that do not parse) keeps the last good list and
// its time, and the next fetch waits kRefreshMs too. Service policy, not a port: a stock
// client loads the list when its lobby page opens and again at each press of Refresh (the
// page's ACTIVATE of SERVER_LIST_REFRESH, whose action is GLB_LOADANDPING over
// @GSB_SERVER@, templates/jop_2_main.mnx).

#include <net/http/http_exchange.h>

#include <net/novaworld/gsb.h>

#include <crow/json.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

// What the Lobbies page is told of NovaLogic's list.
struct NovaLogicList {
	bool fetched = false;      // a fetch ever succeeded
	int64_t fetched_at_ms = 0; // the last good fetch's wall clock (Unix ms)
	int64_t age_ms = 0;        // how long ago that was (the steady clock's)
	bool failing = false;      // the last fetch failed: the list is the one before it, or none
	std::vector<GsbServerEntry> servers; // the host's cp1252 text, as the feed carries it
};

class NovaLogicLobbies {
public:
	// NovaLogic's web host's JO feed, as the stock client's capture names it (above).
	static constexpr const char *kUrl = "http://207.178.209.204/jop_2.gsb?a=1";
	// The fetch names itself: the site's, not a game client's.
	static constexpr const char *kUserAgent = "User-Agent: OpenNova NovaWorld web (Lobbies page)";
	// A list this old is fetched again at the next read.
	static constexpr int64_t kRefreshMs = 5LL * 60 * 1000;
	// Each connect, send and recv of a fetch waits at most this long, the whole reply at most
	// kFetchTotalMs, and a reply past kMaxReplyBytes is refused: the live feed is a few KiB, and
	// its parse grows a large body many times over (a row's strings), so the cap goes before it.
	static constexpr int kFetchTimeoutMs = 5000;
	static constexpr int kFetchTotalMs = 20000;
	static constexpr size_t kMaxReplyBytes = 256u * 1024u;

	// One fetch of the feed: the HTTP reply (apps/common/net_http.h).
	using Fetch = std::function<net::HttpReply()>;
	// Milliseconds: the wall clock (fetched_at_ms) and a steady one (the cache).
	using Clock = std::function<int64_t()>;

	// The feed at kUrl over apps/common's http_exchange.
	static net::HttpReply fetch_novalogic();

	explicit NovaLogicLobbies(Fetch fetch = fetch_novalogic, Clock wall_ms = {}, Clock steady_ms = {});
	// Waits for the fetch in flight, if any (bounded by kFetchTotalMs past its connect).
	~NovaLogicLobbies();
	NovaLogicLobbies(const NovaLogicLobbies &) = delete;
	NovaLogicLobbies &operator=(const NovaLogicLobbies &) = delete;

	// The list as it stands; starts a fetch in the background when it is due (above).
	NovaLogicList read();
	// Waits for the fetch in flight, if any (the tests').
	void settle();
	// The fetches started so far.
	uint64_t fetches() const;

private:
	// One fetch and its parse, its outcome stored (the background thread's).
	void refresh();

	const Fetch fetch_;
	const Clock wall_;
	const Clock steady_;
	mutable std::mutex mu_;
	bool fetched_ = false;
	int64_t fetched_wall_ms_ = 0;
	int64_t fetched_steady_ms_ = 0;
	bool attempted_ = false;
	int64_t attempted_steady_ms_ = 0;
	bool failing_ = false;
	uint64_t fetches_ = 0;
	std::vector<GsbServerEntry> servers_;
	std::future<void> inflight_;
};

// GET /api/lobbies/novalogic's body: {"enabled", "servers": [{"id", "serverName",
// "mission", "gameType", "players", "maxPlayers", "locked", "password", "message", "country",
// "region", "timeLeft", "timeOfDay", "expansion", "punkBuster", "dedicated", "tracers",
// "roster"}], "fetchedAt" (ISO 8601 UTC, null before the first good fetch), "ageSeconds"
// (null alike), "failing"}. `list` is null with the switch off: enabled false, no servers.
// Text is UTF-8 (the feed's cp1252 read as a stock client's host wrote it) with the game's
// inline markup left in; the roster is the players' names; a row's host address is left out.
crow::json::wvalue novalogic_lobbies_json(const NovaLogicList *list);

} // namespace opennova::novaworld_server
