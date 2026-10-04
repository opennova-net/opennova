// nw-lister's listing + credentials readers: the JSON a mirrored server's
// listing is written in, folded onto the HostRegistration the host leg sends.

#include "listing.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
		std::filesystem::remove(path);
	}
	if (g_failures == 0) std::printf("nw_lister_listing: OK\n");
	return g_failures == 0 ? 0 : 1;
}
