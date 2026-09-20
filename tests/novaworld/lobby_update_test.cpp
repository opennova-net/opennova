// The plaintext host-status heartbeat [orig: Lobby_UpdateServerInfo
// @0x4ff473..0x4ff62c]: builder and parser round-trip, sanitization
// [orig: String_SanitizeForLobby @0x4fe750], the doubled LobbyName/HostKey
// entries, the player suffix and its absence.

#include <net/novaworld/lobby_update.h>

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

opennova::LobbyStatusBlob make_sample_blob() {
	opennova::LobbyStatusBlob blob;
	blob.lobby_name = "jop_2_consumer";
	blob.host_key = "ABC DEF?1234";
	// The Host list in retail SetOrCreate order: BuildHostVarLists' entries
	// first [orig: @0x4d0d08..0x4d0dee], then Lobby_UpdateServerInfo's.
	blob.host_vars = {
		{"LobbyName", "jop_2_consumer"},
		{"ServerName", ""},
		{"Msg", "hello@world"},
		{"MaxPlayers", "16"},
		{"AppId", "4321"},
		{"PLoad", ""},
		{"Exp", "jox01"},
		{"LAN", "0"},
		{"HostKey", "ABC DEF?1234"},
		{"GameType", "Team Deathmatch"},
		{"MissionName", "Camo=Clash"},
		{"Region", "North America"},
		{"Players", "4"},
		{"MI1", "1"}, {"MI2", "2"}, {"MI3", "3"},
		{"Dedicated", "Yes"},
		{"Locked", "No"},
		{"Skins", "Yes"},
		{"TimeLeft", "60"},
		{"Password", "No"},
		{"Tracers", "Yes"},
		{"Mod", "jox01"},
		{"Country", "US"},
		{"Port", "-1"},
		{"AllowPing", "y"},
		{"Age", "0 00:12:34"},
		{"TimeOfDay", "Day"},
		{"AppID", "4321"},
		{"PCIDKey", "16777216"},
		{"GameServerBaffleKey", "4660"},
		{"Stat", "N"},
		{"LevelRange", " "},
		{"BBMode", "0"},
		{"GCC", "US"},
		{"GV", "1.7.5.7"},
		{"Version", "1.7.5.7"},
		{"Ver1", "3"},
		{"Ver2", "2345"},
		{"PBServer", "0"},
	};
	blob.player_names = {"Foo @Bar"};
	return blob;
}

bool check_sanitize() {
	if (!expect(opennova::lobby_sanitize_value("") == "---", "empty -> ---")) return false;
	if (!expect(opennova::lobby_sanitize_value("a b?c@d=e") == "a+b+c+d+e",
			"space ? @ = -> +")) return false;
	if (!expect(opennova::lobby_sanitize_value(" ") == "+", "single space -> +")) return false;
	return true;
}

bool check_update_blob() {
	const auto blob = make_sample_blob();
	const std::string text = opennova::lobby_update_build(blob);
	if (!expect(text.find("jop_2_consumer  HostKey = ABC+DEF+1234") == 0,
			"blob starts with lobby name, two spaces, sanitized HostKey")) return false;
	if (!expect(contains(text, " LobbyName = jop_2_consumer"), "LobbyName repeats as a list entry")) return false;
	if (!expect(contains(text, " HostKey = ABC+DEF+1234 LobbyName"), "HostKey preamble precedes the list walk")) return false;
	if (!expect(text.find(" HostKey = ABC+DEF+1234") != text.rfind(" HostKey = ABC+DEF+1234"),
			"HostKey appears twice (preamble + list entry)")) return false;
	if (!expect(contains(text, " ServerName = ---"), "empty ServerName sanitizes to ---")) return false;
	if (!expect(contains(text, " GameType = Team+Deathmatch"), "GameType sanitized")) return false;
	if (!expect(contains(text, " MissionName = Camo+Clash"), "MissionName sanitized")) return false;
	if (!expect(contains(text, " Region = North+America"), "Region sanitized")) return false;
	if (!expect(contains(text, " Msg = hello+world"), "Msg sanitized")) return false;
	if (!expect(contains(text, " MI1 = 1 MI2 = 2 MI3 = 3"), "MI1/MI2/MI3 present")) return false;
	if (!expect(contains(text, " Port = -1"), "Port = -1")) return false;
	if (!expect(contains(text, " AllowPing = y"), "AllowPing is the y/n character")) return false;
	if (!expect(contains(text, " Age = 0+00:12:34"), "Age sanitized")) return false;
	if (!expect(contains(text, " AppID = 4321"), "AppID present")) return false;
	if (!expect(contains(text, " PCIDKey = 16777216"), "PCIDKey present")) return false;
	if (!expect(contains(text, " GameServerBaffleKey = 4660"), "GameServerBaffleKey present")) return false;
	if (!expect(contains(text, " LevelRange = +"), "one-space LevelRange sanitizes to +")) return false;
	if (!expect(contains(text, " GV = 1.7.5.7 Version = 1.7.5.7"), "GV + Version present")) return false;
	if (!expect(contains(text, " Ver1 = 3 Ver2 = 2345 PBServer = 0"), "Ver1/Ver2/PBServer tail")) return false;
	if (!expect(contains(text, " p=Foo++Bar"), "player suffix sanitized")) return false;
	if (!expect(!contains(text, "Expbits") && !contains(text, "Joicon2") && !contains(text, "PIX"),
			"no non-retail keys")) return false;
	return true;
}

bool check_player_suffix() {
	auto blob = make_sample_blob();
	blob.player_names.clear();
	const std::string none = opennova::lobby_update_build(blob);
	if (!expect(none.size() >= 3 && none.compare(none.size() - 3, 3, " p=") == 0,
			"no players -> a lone \" p=\"")) return false;
	blob.player_names = {"a", "b c"};
	const std::string two = opennova::lobby_update_build(blob);
	if (!expect(two.size() >= 10 && two.compare(two.size() - 10, 10, " p=a p=b+c") == 0,
			"one p= per player")) return false;
	blob.send_player_names = false;
	const std::string off = opennova::lobby_update_build(blob);
	if (!expect(!contains(off, " p="), "send_player_names off -> no suffix")) return false;
	return true;
}

bool check_parse_roundtrip() {
	const auto blob = make_sample_blob();
	const std::string text = opennova::lobby_update_build(blob);
	opennova::LobbyStatusBlob parsed;
	if (!expect(opennova::lobby_update_parse(text, parsed), "parse succeeds")) return false;
	if (!expect(parsed.lobby_name == "jop_2_consumer", "lobby name")) return false;
	if (!expect(parsed.host_key == "ABC+DEF+1234", "host key (sanitized)")) return false;
	if (!expect(parsed.host_vars.size() == blob.host_vars.size(), "every var survives")) return false;
	if (!expect(parsed.host_vars[0].first == "LobbyName", "order preserved")) return false;
	if (!expect(opennova::lobby_status_value(parsed, "gametype") == "Team+Deathmatch",
			"case-insensitive lookup")) return false;
	if (!expect(opennova::lobby_status_value(parsed, "ServerName") == "---", "--- kept")) return false;
	if (!expect(parsed.player_names.size() == 1 && parsed.player_names[0] == "Foo++Bar",
			"player names")) return false;
	if (!expect(parsed.send_player_names, "player suffix seen")) return false;

	opennova::LobbyStatusBlob none;
	if (!expect(opennova::lobby_update_parse("jop_2_consumer  HostKey = K p=", none), "lone p= parses")) return false;
	if (!expect(none.host_vars.empty() && none.player_names.empty() && none.send_player_names,
			"lone p= -> no names, suffix seen")) return false;
	opennova::LobbyStatusBlob off;
	if (!expect(opennova::lobby_update_parse("jop_2_consumer  HostKey = K A = 1", off), "no suffix parses")) return false;
	if (!expect(!off.send_player_names && off.host_vars.size() == 1, "no suffix -> flag off")) return false;
	opennova::LobbyStatusBlob bad;
	if (!expect(!opennova::lobby_update_parse("jop:cus2", bad), "a gate tag is not a blob")) return false;
	if (!expect(!opennova::lobby_update_parse("", bad), "empty is not a blob")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_sanitize()) return 1;
	if (!check_update_blob()) return 1;
	if (!check_player_suffix()) return 1;
	if (!check_parse_roundtrip()) return 1;
	std::printf("OK: lobby_update text KV blob matches the retail host status heartbeat\n");
	return 0;
}
