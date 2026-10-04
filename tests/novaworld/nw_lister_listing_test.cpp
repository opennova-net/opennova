// nw-lister's listing + credentials readers: the JSON a mirrored server's
// listing is written in, folded onto the HostRegistration the host leg sends;
// and the remote-admin login and reply parsers behind --admin.

#include "admin_feed.h"
#include "listing.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace opennova::lister;

int g_failures = 0;

void expect(bool condition, const char *message) {
	if (condition) return;
	std::fprintf(stderr, "FAIL: %s\n", message);
	++g_failures;
}

std::string write_temp(const char *name, const std::string &text) {
	const auto path = std::filesystem::temp_directory_path() / name;
	std::ofstream(path, std::ios::binary) << text;
	return path.string();
}

} // namespace

int main() {
	{
		const std::string path = write_temp("nw_lister_listing_test.json", R"({
  "server_name": "Test Server",
  "msg": "hello",
  "mission": "DM - Killhouse",
  "game_type": "DM",
  "max_players": 64,
  "password": false,
  "exp": "jox01",
  "country": "UK",
  "region": "Desert",
  "time_left_minutes": 9,
  "time_of_day": "Night",
  "dedicated": true,
  "players": ["Alpha", {"name": "Bravo", "slot": 5, "team": "2"}, ""]
})");
		Listing l;
		std::string err;
		expect(load_listing(path, l, err), "a valid listing loads");
		expect(l.reg.server_name == "Test Server", "server_name");
		expect(l.reg.server_message == "hello", "msg");
		expect(l.reg.mission_name == "DM - Killhouse", "mission");
		expect(l.reg.game_type == "DM", "game_type");
		expect(l.reg.max_players == 64, "max_players");
		expect(!l.reg.password, "password");
		expect(l.reg.expansion == "jox01", "exp");
		expect(l.reg.country == "UK", "country");
		expect(l.reg.region_index == 1, "region Desert -> 1");
		expect(l.reg.time_of_day == 4, "time_of_day Night -> 4");
		// Whole minutes must survive the host's /3720 TimeLeft division.
		expect(l.reg.round_time_remaining_ticks / 3720 == 9, "time_left_minutes -> ticks");
		expect(!l.reg.listen_host && l.reg.dedicated_server, "dedicated");
		expect(l.players.size() == 2, "empty player names are dropped");
		if (l.players.size() == 2) {
			expect(l.players[0].player_name == "Alpha" && l.players[0].slot == 0, "string player takes the next slot");
			expect(l.players[1].player_name == "Bravo" && l.players[1].slot == 5 && l.players[1].team == "2",
			       "object player keeps its slot and team");
			expect(l.players[1].type == "0", "player type defaults to 0");
		}
		std::filesystem::remove(path);
	}
	{
		const std::string path = write_temp("nw_lister_listing_bad.json", "{ \"server_name\": ");
		Listing l;
		std::string err;
		expect(!load_listing(path, l, err), "truncated JSON is rejected");
		expect(!err.empty(), "a rejection carries a reason");
		std::filesystem::remove(path);
	}
	{
		const std::string path =
				write_temp("nw_lister_creds_test.txt", "# comment\nNOVAWORLD_USER = \"someone\"\nNOVAWORLD_PASS=secret\n");
		Credentials c;
		std::string err;
		expect(load_credentials(path, c, err), "credentials load");
		expect(c.user == "someone" && c.pass == "secret" && c.present(), "quoted and bare values");
		expect(!c.admin_present(), "no admin login unless given");
		std::filesystem::remove(path);
	}
	{
		const std::string path = write_temp("nw_lister_creds_admin.txt", "ADMIN_USER=boss\nADMIN_PASS='pw'\n");
		Credentials c;
		std::string err;
		expect(load_credentials(path, c, err), "admin credentials load");
		expect(c.admin_user == "boss" && c.admin_pass == "pw" && c.admin_present(), "ADMIN_USER / ADMIN_PASS");
		expect(!c.present(), "an admin login alone is not a NovaWorld account");
		std::filesystem::remove(path);
	}
	{
		// Login answers computed by opennova-net/WolfRAT2's _jo_encrypt for
		// challenge[i] = (i*37+11) & 0xFF (0 -> 1), 32 bytes + NUL.
		std::vector<uint8_t> challenge;
		for (int i = 0; i < 32; ++i) {
			const uint8_t b = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
			challenge.push_back(b ? b : 1);
		}
		challenge.push_back(0);
		auto hex = [](const std::array<uint8_t, 65> &a) {
			std::string out;
			char t[3];
			for (uint8_t b : a) {
				std::snprintf(t, sizeof(t), "%02x", b);
				out += t;
			}
			return out;
		};
		expect(hex(admin_login_response(challenge.data(), challenge.size(), "admin", "secret")) ==
		               "74b58687b819aa6b5c7dce4f00e1f233a445161748a93afbec0d5e53f5e3e528a775464778d96a2b1c3d8e0fc0a1b2f36405d6"
		               "d70869fabbaccd1e9fbe9aafe755",
		       "admin login answer matches WolfRAT2 (admin/secret)");
		expect(hex(admin_login_response(challenge.data(), challenge.size(), "someone", "pw2")) ==
		               "74b58687b819aa6b5c7dce4f00e1f233a445161748a93afbec0d5edf9071b43aa475464778d96a2b1c3d8e0fc0a1b2f36405d6"
		               "d70869fabbaccd830dbf96aff267",
		       "admin login answer matches WolfRAT2 (someone/pw2)");
	}
	{
		// Replies as a live retail server sends them.
		const auto players = parse_admin_players("NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n"
		                                         "Host            \t 0\t 1\t9\t0\t0\t0\n"
		                                         "Hamshop         \t 1\t 1\t8\t7\t2\t235\n"
		                                         "Two Words       \t 3\t 2\t5\t0\t0\t188\n");
		expect(players.size() == 2, "header and the dedicated Host row are not players");
		if (players.size() == 2) {
			expect(players[0].name == "Hamshop" && players[0].slot == 1 && players[0].team == "1", "player row");
			expect(players[1].name == "Two Words" && players[1].slot == 3 && players[1].team == "2",
			       "names keep inner spaces, slots come from the server");
		}
		expect(parse_admin_players("No players.").empty(), "no rows, no players");

		expect(parse_admin_current_mission("0: Mogadishu.bms - () () () <CURRENT MISSION> <>") == "Mogadishu",
		       "current mission without its extension");
		expect(parse_admin_current_mission("0: A.bms - () () () <> <NEXT MISSION>\n"
		                                   "1: AS - Black Rock TAC.npj - (2x) () () <CURRENT MISSION> <>") ==
		               "AS - Black Rock TAC",
		       "the current row, file names with spaces");
		expect(parse_admin_current_mission("No missions in queue.").empty(), "empty queue");

		expect(parse_admin_time_left("Tracers = 1\nGameTime = 21/25\n") == 21, "GameTime remaining/total");
		expect(parse_admin_time_left("GameTime = 0/30") == -1, "nothing left lists as no limit");
		expect(parse_admin_time_left("GameTime = 5/0") == -1, "no limit");
		expect(parse_admin_time_left("Tracers = 1") == -1, "no GameTime row");
	}
	if (g_failures == 0) std::printf("nw_lister_listing: OK\n");
	return g_failures == 0 ? 0 : 1;
}
