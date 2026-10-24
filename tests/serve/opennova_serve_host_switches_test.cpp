// opennova-serve's host switches (net parity lane L8, the unrowed host items):
//   - `/maxplayers N` (any case, its next token's CRT atol): a nonzero value replaces what the
//     host file's MaxPlayers line stores, and a file with no MaxPlayers line leaves it unread;
//   - the bind scan's random start: with the quad's random word set and more than one port to
//     try, the first bind is at min + delta * a draw in 0..attempts - 1, past max at max, the
//     retries stepping and wrapping to min; one port, or random clear, starts at min.
// [orig: Game_ParseCommandLineAndInit @0x4A76EF..0x4A7710 -> g_MaxPlayerCommandLineArg
//  @0xB4C4F8; ServerConfig_ApplyHostSetting's MaxPlayers @0x4A6213..0x4A624B (the store
//  @0x4A622E, the switch @0x4A6233..0x4A6243); NapiUdpSocket_CreateAndBind @0x62D2A0 - the
//  count @0x62d3b0, the random start @0x62d3b6..0x62d403, the retries @0x62d440..0x62d454]
#include "server.h"

#include <net/npwire/net_ports.h>
#include <runtime/inmatch/host_file.h>
#include <runtime/inmatch/mission_rotation.h>

#include <formats/gamecfg/game_cfg.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
void expect(bool cond, const char *what) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	++failures;
}

int32_t cap_after(const std::string &text, int32_t max_player_arg) {
	gamecfg::GameCfg cfg;
	cfg.mp_max_players = 64;
	inmatch::HostRotation rotation;
	const std::vector<mission_catalog::Row> catalog;
	(void)inmatch::read_host_file(text.data(), text.size(), cfg, rotation, catalog, max_player_arg);
	return cfg.mp_max_players;
}

void run_the_maxplayers_switch() {
	serve::ServeOptions options;
	std::string error;
	expect(serve::parse_serve_options({"--resource-dir", "x", "/HOST", "h.host", "/MaxPlayers", "12"},
			       options, error) == 0 &&
	               options.max_player_arg == 12,
	       "/maxplayers in any case takes its next token");
	serve::ServeOptions missing;
	expect(serve::parse_serve_options({"--resource-dir", "x", "/maxplayers"}, missing, error) == 1,
	       "a /maxplayers with no value is refused");

	expect(cap_after("MaxPlayers 8\r\n", 0) == 8, "no switch: the file's MaxPlayers");
	expect(cap_after("MaxPlayers 8\r\n", 12) == 12, "the switch replaces the file's MaxPlayers");
	expect(cap_after("GameName \"x\"\r\n", 12) == 64, "no MaxPlayers line: the switch stays unread");
}

void run_the_random_start() {
	const std::vector<uint16_t> plain = lan_host_bind_ports(100, 109, 1);
	expect(plain.size() == 10 && plain.front() == 100 && plain.back() == 109, "random clear: from min");
	const std::vector<uint16_t> drawn = lan_host_bind_ports(100, 109, 1, 7);
	expect(drawn.size() == 10 && drawn[0] == 107 && drawn[2] == 109 && drawn[3] == 100 &&
	               drawn.back() == 106,
	       "a draw of 7 starts at 107, wraps to min past max, and tries every port once");
	const std::vector<uint16_t> stepped = lan_host_bind_ports(100, 119, 3, 5);
	expect(stepped.size() == 6 && stepped[0] == 115 && stepped[1] == 118 && stepped[2] == 100,
	       "the draw counts in steps of delta");
	expect(bind_scan_attempts(100, 109, 1) == 10 && bind_scan_attempts(100, 119, 3) == 6 &&
	               bind_scan_attempts(40000, 40000, 1) == 1,
	       "the scan's attempt count, the draw's range");
}

} // namespace

int main() {
	run_the_maxplayers_switch();
	run_the_random_start();
	if (failures == 0) std::printf("opennova_serve_host_switches: all passed\n");
	return failures == 0 ? 0 : 1;
}
