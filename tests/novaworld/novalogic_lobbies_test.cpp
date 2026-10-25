// The list of the servers on NovaLogic's NovaWorld for the Lobbies page
// (apps/novaworld_web/novalogic_lobbies.h), no database and no network: the feed is a
// scripted fetch answering a GSB body minted through our own writer
// (gsb_build_response, net/novaworld/gsb.h), never a retail capture, and the clocks
// are the test's:
//
//   - the parse: the minted rows read back, their cp1252 text UTF-8 in the JSON, the
//     lock column from Password or Locked, no host address in the body;
//   - the cache: reads inside kRefreshMs fetch nothing more, the next read after it
//     fetches once, reads from many threads while a fetch is in flight start no
//     second one, and no read waits on the fetch;
//   - failures (no reply, a status other than 200, bytes that are no server list, a
//     feed cut before its end, a reply past kMaxReplyBytes, a fetch that throws): the last good list kept with its
//     time, `failing` set, the next fetch a kRefreshMs on; a first fetch that fails
//     answers no list;
//   - the switch: ONNET_NOVALOGIC_LOBBIES off unless set, and the route's body with it
//     off (enabled false, nothing fetched);
//   - the feed's URL, as the stock client's capture names it.

#include "novalogic_lobbies.h"

#include "program/server_config.h"

#include <net/novaworld/gsb.h>

#include <crow/json.h>

#include "common/test_expect.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace nws = opennova::novaworld_server;
namespace net = opennova::net;
using opennova::GsbServerEntry;
using nws::NovaLogicLobbies;

namespace {

// 2026-10-25T12:00:00Z.
constexpr int64_t kNowMs = 1792929600000LL;

#ifdef _WIN32
void set_env(const char *name, const char *value) { _putenv_s(name, value); }
#else
void set_env(const char *name, const char *value) {
	if (*value == '\0') unsetenv(name);
	else setenv(name, value, 1);
}
#endif

// Three rows as a stock JO host's status would fill them: a name in cp1252 (é 0xE9,
// © 0xA9, as a live feed's names carry them), one with a password, one locked.
std::vector<GsbServerEntry> minted_rows() {
	GsbServerEntry a;
	a.rid = 0x0A0027A0;
	a.ip = "203.0.113.7";
	a.server_name = "Caf\xE9 \xA9 Ops";
	a.mission_name = "AS - Kombang Valley";
	a.game_type = "AAS";
	a.players = 12;
	a.max_players = 32;
	a.player_names = { "biggy", "Ann" };
	GsbServerEntry b;
	b.rid = 0x0A0027A1;
	b.ip = "198.51.100.9";
	b.server_name = "Bravo";
	b.mission_name = "TDM - Desert Rain";
	b.game_type = "TDM";
	b.players = 0;
	b.max_players = 16;
	b.password = "Y";
	GsbServerEntry c;
	c.rid = 0x0A0027A2;
	c.ip = "192.0.2.44";
	c.server_name = "Charlie";
	c.game_type = "COOP";
	c.players = 3;
	c.max_players = 8;
	c.locked = "Y";
	return { a, b, c };
}

net::HttpReply answer(const std::vector<uint8_t> &body, int code = 200) {
	net::HttpReply reply;
	reply.transport_ok = true;
	reply.connected = true;
	reply.code = code;
	reply.body = body;
	return reply;
}

net::HttpReply no_answer(const char *why) {
	net::HttpReply reply;
	reply.error = why;
	return reply;
}

// A feed answering from a script, one reply a fetch (the last again once it runs out).
struct ScriptedFeed {
	std::vector<net::HttpReply> replies;
	std::atomic<size_t> calls{ 0 };

	NovaLogicLobbies::Fetch fetch() {
		return [this]() {
			const size_t i = calls++;
			return replies.at(i < replies.size() ? i : replies.size() - 1);
		};
	}
};

// The test's clocks: wall (Unix ms) and steady (ms), stepped by hand.
struct Clocks {
	std::atomic<int64_t> wall{ kNowMs };
	std::atomic<int64_t> steady{ 5000 };

	NovaLogicLobbies::Clock wall_clock() { return [this] { return wall.load(); }; }
	NovaLogicLobbies::Clock steady_clock() { return [this] { return steady.load(); }; }
	void advance(int64_t ms) {
		wall += ms;
		steady += ms;
	}
};

crow::json::rvalue json_of(const nws::NovaLogicList *list) {
	return crow::json::load(nws::novalogic_lobbies_json(list).dump());
}

std::string str(const crow::json::rvalue &v) { return std::string(v.s()); }

int test_parse() {
	ScriptedFeed feed;
	feed.replies = { answer(opennova::gsb_build_response(minted_rows())) };
	Clocks clocks;
	NovaLogicLobbies lobbies(feed.fetch(), clocks.wall_clock(), clocks.steady_clock());

	// The first read has no list yet and starts the fetch; it does not wait for it.
	nws::NovaLogicList list = lobbies.read();
	TEST_EXPECT(!list.fetched && !list.failing && list.servers.empty());
	TEST_EXPECT(lobbies.fetches() == 1);
	auto json = json_of(&list);
	TEST_EXPECT(json && json["enabled"].b() && json["servers"].size() == 0);
	TEST_EXPECT(json["fetchedAt"].t() == crow::json::type::Null);
	TEST_EXPECT(json["ageSeconds"].t() == crow::json::type::Null);
	TEST_EXPECT(!json["failing"].b());

	lobbies.settle();
	clocks.advance(42000);
	list = lobbies.read();
	TEST_EXPECT(lobbies.fetches() == 1 && feed.calls == 1);
	TEST_EXPECT(list.fetched && !list.failing);
	TEST_EXPECT(list.fetched_at_ms == kNowMs && list.age_ms == 42000);
	TEST_EXPECT(list.servers.size() == 3);
	TEST_EXPECT(list.servers[0].rid == 0x0A0027A0 && list.servers[0].server_name == "Caf\xE9 \xA9 Ops");
	// The list keeps no host address (the route never shows one); its players' names it keeps.
	TEST_EXPECT(list.servers[0].ip.empty());
	TEST_EXPECT(list.servers[0].player_names == std::vector<std::string>({ "biggy", "Ann" }));

	const std::string body = nws::novalogic_lobbies_json(&list).dump();
	json = crow::json::load(body);
	TEST_EXPECT(json && json["enabled"].b() && !json["failing"].b());
	TEST_EXPECT(str(json["fetchedAt"]) == "2026-10-25T12:00:00Z");
	TEST_EXPECT(json["ageSeconds"].i() == 42);
	const auto &servers = json["servers"];
	TEST_EXPECT(servers.size() == 3);
	TEST_EXPECT(servers[0]["id"].i() == 0x0A0027A0);
	TEST_EXPECT(str(servers[0]["serverName"]) == "Caf\xC3\xA9 \xC2\xA9 Ops"); // UTF-8
	TEST_EXPECT(str(servers[0]["mission"]) == "AS - Kombang Valley");
	TEST_EXPECT(str(servers[0]["gameType"]) == "AAS");
	TEST_EXPECT(servers[0]["players"].i() == 12 && servers[0]["maxPlayers"].i() == 32);
	TEST_EXPECT(!servers[0]["locked"].b());
	TEST_EXPECT(servers[1]["locked"].b()); // Password Y
	TEST_EXPECT(servers[2]["locked"].b()); // Locked Y
	TEST_EXPECT(str(servers[2]["mission"]).empty());
	// The server's page: the row's other columns and its roster.
	TEST_EXPECT(!servers[0]["password"].b() && servers[1]["password"].b());
	TEST_EXPECT(servers[0]["roster"].size() == 2 && str(servers[0]["roster"][0]) == "biggy" &&
			str(servers[0]["roster"][1]) == "Ann");
	TEST_EXPECT(servers[0].has("message") && servers[0].has("country") && servers[0].has("timeLeft") &&
			servers[0].has("expansion") && servers[0].has("punkBuster") && servers[0].has("dedicated"));
	// No host's address reaches the page.
	for (const char *hidden : { "203.0.113.7", "198.51.100.9", "192.0.2.44" })
		TEST_EXPECT(body.find(hidden) == std::string::npos);
	return 0;
}

int test_cache() {
	ScriptedFeed feed;
	std::vector<GsbServerEntry> second = minted_rows();
	second.resize(1);
	second[0].players = 20;
	feed.replies = { answer(opennova::gsb_build_response(minted_rows())),
		answer(opennova::gsb_build_response(second)) };
	Clocks clocks;
	NovaLogicLobbies lobbies(feed.fetch(), clocks.wall_clock(), clocks.steady_clock());
	lobbies.read();
	lobbies.settle();

	// Any number of reads inside the window: the one fetch.
	for (int i = 0; i < 200; ++i) {
		clocks.advance((NovaLogicLobbies::kRefreshMs - 1) / 200);
		TEST_EXPECT(lobbies.read().servers.size() == 3);
	}
	lobbies.settle();
	TEST_EXPECT(lobbies.fetches() == 1 && feed.calls == 1);

	// Past it: the next read answers the list it has and fetches once.
	clocks.advance(NovaLogicLobbies::kRefreshMs);
	const nws::NovaLogicList before = lobbies.read();
	TEST_EXPECT(before.servers.size() == 3 && before.age_ms >= NovaLogicLobbies::kRefreshMs);
	TEST_EXPECT(lobbies.fetches() == 2);
	lobbies.settle();
	const nws::NovaLogicList after = lobbies.read();
	TEST_EXPECT(lobbies.fetches() == 2 && feed.calls == 2);
	TEST_EXPECT(after.servers.size() == 1 && after.servers[0].players == 20 && after.age_ms == 0);

	// Reads from many threads while a fetch hangs: one fetch, and every read answers.
	std::promise<void> release;
	std::shared_future<void> gate = release.get_future().share();
	std::atomic<int> slow_calls{ 0 };
	Clocks slow_clocks;
	const std::vector<uint8_t> feed_bytes = opennova::gsb_build_response(minted_rows());
	NovaLogicLobbies slow(
			[&]() {
				++slow_calls;
				gate.wait();
				return answer(feed_bytes);
			},
			slow_clocks.wall_clock(), slow_clocks.steady_clock());
	std::atomic<int> answered{ 0 };
	std::vector<std::thread> readers;
	for (int t = 0; t < 8; ++t) {
		readers.emplace_back([&]() {
			for (int i = 0; i < 50; ++i) {
				if (!slow.read().fetched) ++answered;
			}
		});
	}
	for (std::thread &t : readers) t.join();
	TEST_EXPECT(answered == 400);
	// Past the window with the fetch still out: still the one.
	slow_clocks.advance(2 * NovaLogicLobbies::kRefreshMs);
	TEST_EXPECT(!slow.read().fetched);
	TEST_EXPECT(slow.fetches() == 1);
	release.set_value();
	slow.settle();
	TEST_EXPECT(slow_calls == 1 && slow.fetches() == 1);
	// Its list, now; the fetch it ran past its window starts the next (the destructor waits).
	TEST_EXPECT(slow.read().servers.size() == 3);
	return 0;
}

int test_failures() {
	const std::vector<uint8_t> good = opennova::gsb_build_response(minted_rows());
	std::vector<uint8_t> cut = good;
	cut.resize(cut.size() - 8); // the XXXX record gone
	const std::string junk = "<html>Service Unavailable</html>";
	ScriptedFeed feed;
	feed.replies = { answer(good), no_answer("connect to 207.178.209.204:80 failed"),
		answer(std::vector<uint8_t>(junk.begin(), junk.end()), 503),
		answer(std::vector<uint8_t>(junk.begin(), junk.end())), answer(cut), answer({}),
		// A reply past the cap is refused before its parse.
		answer(std::vector<uint8_t>(NovaLogicLobbies::kMaxReplyBytes + 1, 0)) };
	Clocks clocks;
	NovaLogicLobbies lobbies(feed.fetch(), clocks.wall_clock(), clocks.steady_clock());
	lobbies.read();
	lobbies.settle();
	TEST_EXPECT(lobbies.read().fetched);

	for (size_t failure = 1; failure < feed.replies.size(); ++failure) {
		// The next fetch is a kRefreshMs after the last, a good one's or a failed one's.
		clocks.advance(NovaLogicLobbies::kRefreshMs - 1);
		lobbies.read();
		lobbies.settle();
		TEST_EXPECT(feed.calls == failure);
		clocks.advance(1);
		lobbies.read();
		lobbies.settle();
		const nws::NovaLogicList list = lobbies.read();
		TEST_EXPECT(feed.calls == failure + 1);
		TEST_EXPECT(list.fetched && list.failing);
		TEST_EXPECT(list.servers.size() == 3 && list.servers[0].server_name == "Caf\xE9 \xA9 Ops");
		TEST_EXPECT(list.fetched_at_ms == kNowMs);
		TEST_EXPECT(list.age_ms == static_cast<int64_t>(failure) * NovaLogicLobbies::kRefreshMs);
		const auto json = json_of(&list);
		TEST_EXPECT(json["failing"].b() && json["servers"].size() == 3);
		TEST_EXPECT(str(json["fetchedAt"]) == "2026-10-25T12:00:00Z");
	}

	// A good fetch after failures: the new list, failing cleared.
	feed.replies.push_back(answer(good));
	clocks.advance(NovaLogicLobbies::kRefreshMs);
	lobbies.read();
	lobbies.settle();
	const nws::NovaLogicList again = lobbies.read();
	TEST_EXPECT(again.fetched && !again.failing && again.age_ms == 0 && again.fetched_at_ms > kNowMs);

	// A fetch that throws counts as a failure; a first fetch that fails gives no list.
	Clocks fresh_clocks;
	NovaLogicLobbies throwing([]() -> net::HttpReply { throw std::runtime_error("resolver gone"); },
			fresh_clocks.wall_clock(), fresh_clocks.steady_clock());
	throwing.read();
	throwing.settle();
	const nws::NovaLogicList none = throwing.read();
	TEST_EXPECT(!none.fetched && none.failing && none.servers.empty());
	const auto json = json_of(&none);
	TEST_EXPECT(json["enabled"].b() && json["failing"].b() && json["servers"].size() == 0);
	TEST_EXPECT(json["fetchedAt"].t() == crow::json::type::Null);
	return 0;
}

int test_switch_off() {
	// The route's body with the switch off: nothing listed, nothing fetched.
	const std::string body = nws::novalogic_lobbies_json(nullptr).dump();
	const auto json = crow::json::load(body);
	TEST_EXPECT(json && !json["enabled"].b() && !json["failing"].b());
	TEST_EXPECT(json["servers"].t() == crow::json::type::List && json["servers"].size() == 0);
	TEST_EXPECT(json["fetchedAt"].t() == crow::json::type::Null);
	TEST_EXPECT(json["ageSeconds"].t() == crow::json::type::Null);

	// ONNET_NOVALOGIC_LOBBIES: off by default and when unset; "1" turns it on.
	TEST_EXPECT(!nws::ServerConfig{}.novalogic_lobbies);
	set_env("ONNET_NOVALOGIC_LOBBIES", "");
	TEST_EXPECT(!nws::ServerConfig::from_env().novalogic_lobbies);
	set_env("ONNET_NOVALOGIC_LOBBIES", "0");
	TEST_EXPECT(!nws::ServerConfig::from_env().novalogic_lobbies);
	set_env("ONNET_NOVALOGIC_LOBBIES", "1");
	TEST_EXPECT(nws::ServerConfig::from_env().novalogic_lobbies);
	set_env("ONNET_NOVALOGIC_LOBBIES", "");
	return 0;
}

int test_feed_url() {
	// The stock client's list load on NovaLogic's web host (the SessionInit web domain,
	// 207.178.209.204:80): `GET /jop_2.gsb?a=1` (docs/net/novaworld-net-re.md §7 Wave 4).
	TEST_EXPECT(std::strcmp(NovaLogicLobbies::kUrl, "http://207.178.209.204/jop_2.gsb?a=1") == 0);
	TEST_EXPECT(NovaLogicLobbies::kRefreshMs == 300000);
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "parse", test_parse },
		{ "cache", test_cache },
		{ "failures", test_failures },
		{ "switch_off", test_switch_off },
		{ "feed_url", test_feed_url },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: NovaLogic's server list\n");
	return 0;
}
