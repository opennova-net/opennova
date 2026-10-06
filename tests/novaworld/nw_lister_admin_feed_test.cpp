// opennova-nw-lister's admin poll thread (apps/nw_lister/admin_feed.h) against the engine's own
// remote-admin server (net/admin/admin_server.h) behind the apps' TCP pump
// (apps/common/admin_tcp_server.h) over loopback: the challenge, the login it decrypts and
// checks, one reply per command from a handler answering in the server's formats, and the QUIT
// the feed ends with (a FIN would leak a retail server's slot).
// [orig: CAdminServer_AcceptConnection @0x405580; CAdminServer_HandleLogin @0x405870;
//  CAdminServer_DispatchCommand @0x406811 (QUIT)]

#include "admin_feed.h"

#include "admin_tcp_server.h"
#include "net_sockets.h"

#include <base/io/crt_rand.h>
#include <formats/admincfg/admin_cfg.h>
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

// The commands the feed polls, answered in the server's formats.
struct FeedReplies final : opennova::AdminCommandHandler {
	std::atomic<int> commands{0};
	std::atomic<bool> quit{false};

	bool dispatch(const opennova::AdminSession &, std::string_view line,
			std::vector<std::string> &replies) override {
		++commands;
		if (line == "QUIT") {
			quit = true;
			return false; // QUIT returns 0: the server closes
		}
		if (line == "PLAYER LIST") {
			char rows[256];
			std::snprintf(rows, sizeof(rows),
					"NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n"
					"%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n",
					"Host", 0, 1, 9, 0, 0, 0, "Alpha", 2, 1, 8, 3, 1, 120);
			replies.emplace_back(rows);
		} else if (line == "MISSION LIST") {
			replies.emplace_back("0: CP01.bms - () () () <CURRENT MISSION> <>\n1: CP02.bms - () () () <> <NEXT MISSION>\n");
		} else if (line == "GET GAMESETTINGS") {
			replies.emplace_back("Tracers              = 1\nGameTime             = 12/30\n");
		} else {
			replies.emplace_back("USAGE - [QUIT | GET | MISSION | PLAYER]");
		}
		return true;
	}
	std::string status_report() override { return {}; }
};

} // namespace

int main() {
	using namespace std::chrono_literals;
	if (opennova::net::startup() != 0) return 1;

	const std::string cfg_text = "boss pw 0D\r\nip_restrict = 127.0.0.1\r\n";
	FeedReplies replies;
	opennova::io::CrtRand rand;
	rand.seed(42);
	opennova::AdminServer server(opennova::admincfg::parse(cfg_text.data(), cfg_text.size()), replies, rand);
	opennova::net::AdminTcpServer tcp(server);
	expect(tcp.listen(0, /*loopback_only=*/true), "the console listens");
	std::atomic<bool> stop{false};
	std::thread pump([&] {
		while (!stop) {
			tcp.pump();
			std::this_thread::sleep_for(2ms);
		}
	});

	opennova::net::Endpoint endpoint;
	endpoint.ip = {127, 0, 0, 1};
	endpoint.port = tcp.port();
	opennova::nw_lister::AdminFeed feed;
	feed.start(endpoint, "boss", "pw");
	opennova::nw_lister::AdminSnapshot snapshot;
	for (int i = 0; i < 300 && snapshot.seq == 0; ++i) {
		std::this_thread::sleep_for(10ms);
		snapshot = feed.snapshot();
	}
	expect(snapshot.ok, "the server decrypts the feed's login and the first poll answers");
	expect(snapshot.players.size() == 1 && snapshot.players[0].name == "Alpha" && snapshot.players[0].slot == 2 &&
	               snapshot.players[0].team == "1",
	       "the players, without the host's own slot");
	expect(snapshot.mission == "CP01", "the current mission");
	expect(snapshot.time_left_minutes == 12, "the time left");
	expect(replies.commands == 3, "one command per field, nothing pipelined");

	feed.stop();
	for (int i = 0; i < 300 && !replies.quit; ++i) std::this_thread::sleep_for(10ms);
	expect(replies.quit, "the feed ends its session with QUIT");
	stop = true;
	pump.join();
	expect(tcp.connection_count() == 0 && server.connection_count() == 0, "QUIT frees the server's slot");
	tcp.close();
	opennova::net::shutdown();
	if (g_failures == 0) {
		std::printf("OK: the admin feed logs in, polls and quits\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
