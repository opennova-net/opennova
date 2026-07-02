#include <novaworld/lobby_update.h>

#include <cstdio>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

opennova::LobbyServerInfo make_sample_info() {
	opennova::LobbyServerInfo info;
	info.lobby_name = "NovaWorld";
	info.host_key = "ABC DEF?1234";
	info.host_did = "DID-42";
	info.server_name = "";
	info.game_type = "Team Deathmatch";
	info.mission_name = "Camo=Clash";
	info.region = "North America";
	info.players = 4;
	info.max_players = 16;
	info.mi1 = 1;
	info.mi2 = 2;
	info.mi3 = 3;
	info.dedicated = true;
	info.locked = false;
	info.skins_allowed = true;
	info.password_protected = false;
	info.tracers_disabled = false;
	info.time_left = "60";
	info.country = "US";
	info.tod = "Day";
	info.access_code_list = "";
	info.app_id = 16;
	info.pcid_key = 0xDEADBEEF;
	info.game_server_baffle_key = 0x1234;
	info.stat = "+";
	info.bb_mode = 0;
	info.gv = "1.0.0.9";
	info.version = "1.0.0.9";
	info.country_name = "United States";
	info.lang = "en";
	info.timezone_bias = -300;
	info.pb_server = false;
	info.allow_ping = 121;
	info.msg = "hello@world";
	info.mod = "jox01";
	info.gcc = "US";
	info.player_names = {"Foo @Bar"};
	return info;
}

bool check_update_blob() {
	const auto info = make_sample_info();
	const std::string blob = opennova::lobby_update_build(info);
	if (!expect(blob.find("NovaWorld  HostKey = ABC+DEF+1234") == 0,
			"blob starts with lobby name and two spaces before sanitized HostKey")) return false;
	if (!expect(contains(blob, " ServerName = ---"), "empty ServerName sanitizes to ---")) return false;
	if (!expect(contains(blob, " GameType = Team+Deathmatch"), "GameType sanitized")) return false;
	if (!expect(contains(blob, " MissionName = Camo+Clash"), "MissionName sanitized")) return false;
	if (!expect(contains(blob, " Region = North+America"), "Region sanitized")) return false;
	if (!expect(contains(blob, " Players = 4"), "Players count")) return false;
	if (!expect(contains(blob, " MaxPlayers = 16"), "MaxPlayers count")) return false;
	if (!expect(contains(blob, " Dedicated = Y"), "Dedicated uses STRNOVA yes token")) return false;
	if (!expect(contains(blob, " Locked = N"), "Locked uses STRNOVA no token")) return false;
	if (!expect(contains(blob, " Country = US"), "Country code")) return false;
	if (!expect(contains(blob, " Msg = hello+world"), "Msg field sanitized")) return false;
	if (!expect(contains(blob, " Age = "), "Age replaces Uptime")) return false;
	if (!expect(contains(blob, " TimeOfDay = Day"), "TimeOfDay")) return false;
	if (!expect(contains(blob, " Port = -1"), "Port = -1 (constant per binary)")) return false;
	if (!expect(contains(blob, " AllowPing = 121"), "AllowPing with ping-enabled value 121")) return false;
	if (!expect(contains(blob, " Mod = jox01"), "Mod field")) return false;
	if (!expect(contains(blob, " GCC = US"), "GCC field")) return false;
	if (!expect(contains(blob, " Ver1 = 3"), "Ver1 retail constant")) return false;
	if (!expect(contains(blob, " Ver2 = 2345"), "Ver2 retail constant")) return false;
	if (!expect(contains(blob, " PBServer = 0"), "PBServer disabled")) return false;
	if (!expect(contains(blob, " p=Foo++Bar"), "player suffix sanitized")) return false;
	if (!expect(!contains(blob, " HostDID "), "HostDID omitted")) return false;
	if (!expect(!contains(blob, " AccessCodeList "), "AccessCodeList omitted")) return false;
	if (!expect(!contains(blob, " Uptime "), "Uptime omitted")) return false;
	if (!expect(!contains(blob, " TimezoneBias "), "TimezoneBias omitted")) return false;
	if (!expect(blob.find("DELETE") == std::string::npos, "no fabricated delete suffix")) return false;
	return true;
}

bool check_level_range_always_space() {
	auto info = make_sample_info();
	info.level_range = "";
	const std::string blob = opennova::lobby_update_build(info);
	if (!expect(contains(blob, " Stat = N"), "Stat always N")) return false;
	if (!expect(contains(blob, " LevelRange =  "), "empty LevelRange emits one-space value")) return false;
	// With a level range set retail still publishes the single-space placeholder.
	info.level_range = "5-15";
	const std::string blob2 = opennova::lobby_update_build(info);
	if (!expect(contains(blob2, " LevelRange =  "),
			"non-empty LevelRange still emits one-space placeholder")) return false;
	return true;
}

bool check_extra_pairs() {
	auto info = make_sample_info();
	info.extra_pairs.push_back({"Custom Key", "Custom=Value"});
	const std::string blob = opennova::lobby_update_build(info);
	if (!expect(contains(blob, " Custom+Key = Custom+Value"),
			"extra_pairs sanitized at tail")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_update_blob()) return 1;
	if (!check_level_range_always_space()) return 1;
	if (!check_extra_pairs()) return 1;
	std::printf("OK: lobby_update text KV blob matches retail host registration shape\n");
	return 0;
}
