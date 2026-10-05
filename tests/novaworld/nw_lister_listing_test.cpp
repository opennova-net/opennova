// opennova-nw-lister's listing + credentials readers: the JSON a mirrored server's listing is written in,
// folded onto the HostRegistration the host leg sends, and the KEY=VALUE credentials file.

#include "listing.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace opennova::nw_lister;

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
		const opennova::HostRegistration &r = l.columns;
		expect(r.server_name == "Test Server", "server_name");
		expect(r.server_message == "hello", "msg");
		expect(r.mission_name == "DM - Killhouse", "mission");
		expect(r.game_type == "DM", "game_type");
		expect(r.max_players == 64, "max_players");
		expect(!r.password, "password");
		expect(r.expansion == "jox01", "exp");
		expect(r.country == "UK", "country");
		expect(r.region_index == 1, "region Desert -> 1");
		expect(r.time_of_day == 4, "time_of_day Night -> 4");
		// Whole minutes must survive the host's /3720 TimeLeft division.
		expect(r.round_time_remaining_ticks / 3720 == 9, "time_left_minutes -> ticks");
		expect(!r.listen_host && r.dedicated_server, "dedicated");
		expect(l.players.size() == 2, "empty player names are dropped");
		if (l.players.size() == 2) {
			expect(l.players[0].player_name == "Alpha" && l.players[0].slot == -1,
			       "a bare name leaves its slot to the lister");
			expect(l.players[1].player_name == "Bravo" && l.players[1].slot == 5 && l.players[1].team == "2",
			       "an object player keeps its slot and team");
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
	if (g_failures == 0) std::printf("nw_lister_listing: OK\n");
	return g_failures == 0 ? 0 : 1;
}
