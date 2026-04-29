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
	info.host_key = "ABCDEF1234";
	info.host_did = "DID-42";
	info.server_name = "TestServer";
	info.game_type = "Team Deathmatch";
	info.mission_name = "CamoClash";
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
	return info;
}

bool check_delete_blob() {
	const auto info = make_sample_info();
	const std::string blob = opennova::lobby_update_build(info, /*is_delete=*/true);
	if (!expect(blob.rfind(" Port = -1 DELETE") != std::string::npos,
			"delete form ends with ' Port = -1 DELETE'")) return false;
	if (!expect(blob.find("NovaWorld ") == 0, "blob starts with '<lobbyname> '")) return false;
	if (!expect(blob.find("HostKey = ABCDEF1234") != std::string::npos,
			"HostKey present")) return false;
	if (!expect(blob.find("ServerName") == std::string::npos,
			"delete form omits update-only keys (no ServerName)")) return false;
	return true;
}

bool check_update_blob() {
	const auto info = make_sample_info();
	const std::string blob = opennova::lobby_update_build(info, /*is_delete=*/false);
	if (!expect(blob.find("NovaWorld ") == 0, "blob starts with lobby name")) return false;
	if (!expect(contains(blob, "HostKey = ABCDEF1234"), "HostKey field")) return false;
	if (!expect(contains(blob, " ServerName = TestServer"), "ServerName field")) return false;
	if (!expect(contains(blob, " GameType = Team Deathmatch"), "GameType field")) return false;
	if (!expect(contains(blob, " MissionName = CamoClash"), "MissionName field")) return false;
	if (!expect(contains(blob, " Region = North America"), "Region field")) return false;
	if (!expect(contains(blob, " Players = 4"), "Players count")) return false;
	if (!expect(contains(blob, " MaxPlayers = 16"), "MaxPlayers count")) return false;
	if (!expect(contains(blob, " Dedicated = Yes"), "Dedicated Yes/No")) return false;
	if (!expect(contains(blob, " Locked = No"), "Locked No")) return false;
	if (!expect(contains(blob, " Country = US"), "Country code")) return false;
	if (!expect(contains(blob, " TimeOfDay = Day"), "TimeOfDay")) return false;
	if (!expect(contains(blob, " Port = -1"), "Port = -1 (constant per binary)")) return false;
	if (!expect(contains(blob, " AllowPing = 121"), "AllowPing with ping-enabled value 121")) return false;
	if (!expect(contains(blob, " Ver2 = 2780"), "Ver2 version constant")) return false;
	if (!expect(contains(blob, " PBServer = 0"), "PBServer disabled")) return false;
	return true;
}

bool check_level_range_empty_omits() {
	auto info = make_sample_info();
	info.level_range = "";
	const std::string blob = opennova::lobby_update_build(info, false);
	if (!expect(!contains(blob, " LevelRange"),
			"empty LevelRange is omitted (matches original's 'XX' branch)")) return false;
	// With a level range set it should appear.
	info.level_range = "5-15";
	const std::string blob2 = opennova::lobby_update_build(info, false);
	if (!expect(contains(blob2, " LevelRange = 5-15"),
			"non-empty LevelRange emitted")) return false;
	return true;
}

bool check_extra_pairs() {
	auto info = make_sample_info();
	info.extra_pairs.push_back({"CustomKey", "CustomValue"});
	const std::string blob = opennova::lobby_update_build(info, false);
	if (!expect(contains(blob, " CustomKey = CustomValue"),
			"extra_pairs serialized at tail")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_delete_blob()) return 1;
	if (!check_update_blob()) return 1;
	if (!check_level_range_empty_omits()) return 1;
	if (!check_extra_pairs()) return 1;
	std::printf("OK: lobby_update text KV blob matches Lobby_UpdateServerInfo structure\n");
	return 0;
}
