// The hosted servers' console commands (apps/novaworld_common/hosted/
// console_commands.h): the curated set the console routes may run on a
// server's admin link (service policy), each line composed from checked
// arguments, and what each command's replies (retail's strings, docs/net/
// novaworld-net-re.md §6.9) come to. Crow-free: compiles the hosted sources
// it needs directly, so it runs on every build.

#include "hosted/console_commands.h"

#include <net/admin/admin_protocol.h>

#include "common/test_expect.h"

#include <cstdio>
#include <string>
#include <vector>

namespace hosted = opennova::novaworld_server::hosted;
using hosted::ConsoleVerb;

namespace {

// The verbs every command a route can compose belongs to, and the line each
// starts with: nothing else reaches a server.
const std::vector<std::string> kAllowedPrefixes = {
	"PLAYER LIST", "PLAYER PUNT ", "PLAYER BAN ", "PLAYER SWAPTEAM ", "PLAYER KILL ", "MISSION AVAILABLE",
	"MISSION LIST", "MISSION ADD ", "MISSION REMOVE ", "MISSION SETNEXT ", "MISSION CYCLE", "CHAT GET",
	"CHAT SEND ", "GET GAMESETTINGS", "GET GAMESTATE",
};

bool allowed(const std::string &line) {
	for (const std::string &prefix : kAllowedPrefixes) {
		if (line.compare(0, prefix.size(), prefix) == 0) return true;
	}
	return false;
}

int test_owner_list() {
	for (const ConsoleVerb verb :
			{ ConsoleVerb::PlayerList, ConsoleVerb::PlayerPunt, ConsoleVerb::PlayerBan, ConsoleVerb::PlayerSwapTeam,
					ConsoleVerb::PlayerKill, ConsoleVerb::MissionAvailable, ConsoleVerb::MissionList,
					ConsoleVerb::MissionAdd, ConsoleVerb::MissionRemove, ConsoleVerb::MissionSetNext,
					ConsoleVerb::MissionCycle, ConsoleVerb::ChatGet, ConsoleVerb::ChatSend }) {
		TEST_EXPECT(hosted::owner_may_run(verb));
	}
	TEST_EXPECT(!hosted::owner_may_run(ConsoleVerb::GameSettings));
	// The reads are not recorded; every change is.
	for (const ConsoleVerb verb : { ConsoleVerb::PlayerList, ConsoleVerb::MissionAvailable, ConsoleVerb::MissionList,
				 ConsoleVerb::ChatGet, ConsoleVerb::GameSettings, ConsoleVerb::GameState })
		TEST_EXPECT(!hosted::console_verb_writes(verb));
	for (const ConsoleVerb verb : { ConsoleVerb::PlayerPunt, ConsoleVerb::PlayerBan, ConsoleVerb::PlayerSwapTeam,
				 ConsoleVerb::PlayerKill, ConsoleVerb::MissionAdd, ConsoleVerb::MissionRemove,
				 ConsoleVerb::MissionSetNext, ConsoleVerb::MissionCycle, ConsoleVerb::ChatSend })
		TEST_EXPECT(hosted::console_verb_writes(verb));
	return 0;
}

int test_compose() {
	hosted::ConsoleRefusal why;
	TEST_EXPECT(hosted::console_command(ConsoleVerb::PlayerList)->line == "PLAYER LIST");
	TEST_EXPECT(hosted::console_command(ConsoleVerb::MissionAvailable)->line == "MISSION AVAILABLE");
	TEST_EXPECT(hosted::console_command(ConsoleVerb::MissionList)->line == "MISSION LIST");
	TEST_EXPECT(hosted::console_command(ConsoleVerb::MissionCycle)->line == "MISSION CYCLE");
	TEST_EXPECT(hosted::console_command(ConsoleVerb::ChatGet)->line == "CHAT GET");
	TEST_EXPECT(hosted::console_command(ConsoleVerb::GameSettings)->line == "GET GAMESETTINGS");
	TEST_EXPECT(hosted::console_command(ConsoleVerb::GameState)->line == "GET GAMESTATE");
	TEST_EXPECT(!hosted::console_command(ConsoleVerb::PlayerPunt) && !hosted::console_command(ConsoleVerb::ChatSend));

	// Slots: 1..255; 0 is the host's own, and 256 would read as 0 on the server ((u8)atol).
	TEST_EXPECT(hosted::console_player_command(ConsoleVerb::PlayerPunt, 3, why)->line == "PLAYER PUNT 3");
	TEST_EXPECT(hosted::console_player_command(ConsoleVerb::PlayerBan, 255, why)->line == "PLAYER BAN 255");
	TEST_EXPECT(hosted::console_player_command(ConsoleVerb::PlayerSwapTeam, 1, why)->line == "PLAYER SWAPTEAM 1");
	TEST_EXPECT(hosted::console_player_command(ConsoleVerb::PlayerKill, 9, why)->line == "PLAYER KILL 9");
	for (const uint64_t slot : { uint64_t{ 0 }, uint64_t{ 256 }, uint64_t{ 1 } << 40 }) {
		why = {};
		TEST_EXPECT(!hosted::console_player_command(ConsoleVerb::PlayerPunt, slot, why) && why.error == "invalid_slot");
	}
	TEST_EXPECT(!hosted::console_player_command(ConsoleVerb::ChatSend, 1, why));

	// Rotation indexes below the rotation's cap (32, the start settings' kMissionsMax: policy).
	TEST_EXPECT(hosted::kConsoleRotationMax == 32 && hosted::kConsoleIndexMax == 31);
	TEST_EXPECT(hosted::console_rotation_command(ConsoleVerb::MissionRemove, 0, why)->line == "MISSION REMOVE 0");
	TEST_EXPECT(hosted::console_rotation_command(ConsoleVerb::MissionSetNext, 31, why)->line == "MISSION SETNEXT 31");
	for (const uint64_t index : { uint64_t{ 32 }, uint64_t{ 256 }, uint64_t{ 1 } << 40 }) {
		why = {};
		TEST_EXPECT(!hosted::console_rotation_command(ConsoleVerb::MissionRemove, index, why) &&
				why.error == "invalid_index");
	}

	// MISSION ADD: a .bms name of letters, digits, _ and -, the launch option always given.
	TEST_EXPECT(hosted::console_mission_add("ASH_G11A.BMS", false, why)->line == "MISSION ADD ASH_G11A.BMS 0");
	TEST_EXPECT(hosted::console_mission_add("x-y_1.bms", true, why)->line == "MISSION ADD x-y_1.bms 1");
	TEST_EXPECT(hosted::console_mission_add("A.BMS", false, why)->line == "MISSION ADD A.BMS 0");
	TEST_EXPECT(hosted::console_mission_add(std::string(32, 'm') + ".bms", false, why).has_value());
	for (const std::string &bad : { std::string(), std::string("../a.bms"), std::string("a b.bms"),
				 std::string("a.txt"), std::string("a.bms ONESHOT"), std::string("a.bms\n"), std::string("CYCLE"),
				 std::string(".bms"), std::string(33, 'm') + ".bms", std::string("a.bms\0x", 7),
				 std::string("a\x7f.bms"), std::string("a\tb.bms"), std::string("\xC2\x85.bms") }) {
		why = {};
		TEST_EXPECT(!hosted::console_mission_add(bad, false, why) && why.error == "invalid_mission");
	}

	// CHAT SEND: cp1252 text, no control byte, not blank, 59 bytes at most, trimmed.
	TEST_EXPECT(hosted::console_chat_send("hello there", why)->line == "CHAT SEND hello there");
	TEST_EXPECT(hosted::console_chat_send("  padded  ", why)->line == "CHAT SEND padded");
	// "café" goes out in cp1252: 0xE9.
	TEST_EXPECT(hosted::console_chat_send("caf\xC3\xA9", why)->line == std::string("CHAT SEND caf\xE9"));
	TEST_EXPECT(hosted::console_chat_send(std::string(59, 'x'), why).has_value());
	for (const std::string &bad : { std::string(), std::string("   "), std::string(60, 'x'), std::string("two\nlines"),
				 std::string("tab\there"), std::string("bell\x07"), std::string("c1 \xC2\x85"),
				 std::string("del\x7f"), std::string("nul\0x", 5),
				 std::string("emoji \xF0\x9F\x98\x80"), std::string("not utf8 \xE9") }) {
		why = {};
		TEST_EXPECT(!hosted::console_chat_send(bad, why) && why.error == "invalid_text");
	}

	// Every line a route can compose is on the list, and one packet carries it.
	std::vector<std::string> lines;
	for (const ConsoleVerb verb : { ConsoleVerb::PlayerList, ConsoleVerb::MissionAvailable, ConsoleVerb::MissionList,
				 ConsoleVerb::MissionCycle, ConsoleVerb::ChatGet, ConsoleVerb::GameSettings, ConsoleVerb::GameState })
		lines.push_back(hosted::console_command(verb)->line);
	lines.push_back(hosted::console_player_command(ConsoleVerb::PlayerBan, 7, why)->line);
	lines.push_back(hosted::console_mission_add("a.bms", true, why)->line);
	lines.push_back(hosted::console_chat_send(std::string(59, 'z'), why)->line);
	for (const std::string &line : lines) {
		TEST_EXPECT(allowed(line));
		TEST_EXPECT(line.size() <= 1015);
		// And its reply count is one the link reads exactly (no quiet gap).
		TEST_EXPECT(opennova::admin_reply_plan(line).framing != opennova::AdminReplyFraming::QuietGap);
	}
	return 0;
}

int test_outcomes() {
	using V = ConsoleVerb;
	const auto status = [](V verb, std::vector<std::string> replies) {
		return hosted::console_outcome(verb, replies).status;
	};
	const auto error = [](V verb, std::vector<std::string> replies) {
		return hosted::console_outcome(verb, replies).error;
	};
	TEST_EXPECT(status(V::PlayerList, { "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n" }) == 200);
	TEST_EXPECT(error(V::PlayerList, { "ERROR - Not in Game State." }) == "not_in_game");
	TEST_EXPECT(error(V::PlayerList, { "ERROR - Something else." }) == "admin_error");
	TEST_EXPECT(status(V::GameState, { "OK - Current State = Menus" }) == 200);
	TEST_EXPECT(error(V::GameState, { "OK - Current State = Elsewhere" }) == "admin_error");
	TEST_EXPECT(hosted::console_scene({ "OK - Current State = Game" }) == "Game");
	TEST_EXPECT(hosted::console_scene({ "OK - Current State = Unknown" }) == "Unknown");
	TEST_EXPECT(hosted::console_scene({ "ERROR - x" }).empty() && hosted::console_scene({}).empty());
	TEST_EXPECT(status(V::PlayerPunt, { "OK - Player punted." }) == 200);
	TEST_EXPECT(error(V::PlayerPunt, { "ERROR - Player Not Found." }) == "player_not_found");
	TEST_EXPECT(error(V::PlayerPunt, { "ERROR - Cannot punt Host." }) == "invalid_slot");
	TEST_EXPECT(status(V::PlayerSwapTeam, { "OK - Player Swapped." }) == 200);
	TEST_EXPECT(status(V::PlayerKill, { "OK - Player Killed." }) == 200);
	TEST_EXPECT(error(V::PlayerKill, { "OK - Player punted." }) == "admin_error");
	TEST_EXPECT(status(V::PlayerBan, { "OK - Banned Player #1 \"Ann\".", "OK - Player Banned." }) == 200);
	TEST_EXPECT(status(V::PlayerBan, { "OK - Player Banned." }) == 200);
	TEST_EXPECT(error(V::PlayerBan, { "ERROR - Player #2 \"Bob\" doesn't have a PCID." }) == "no_pcid");
	TEST_EXPECT(error(V::PlayerBan, { "ERROR - Player #2 \"Bob\" not connected to a network." }) == "not_connected");
	TEST_EXPECT(error(V::PlayerBan, { "ERROR - Player Not Found." }) == "player_not_found");
	TEST_EXPECT(status(V::MissionAdd, { "OK - Entry Added\n" }) == 200);
	TEST_EXPECT(error(V::MissionAdd, { "ERROR - File Not Found." }) == "mission_not_found");
	TEST_EXPECT(status(V::MissionRemove, { "OK - Mission Removed." }) == 200);
	TEST_EXPECT(status(V::MissionSetNext, { "OK - Next Mission Set." }) == 200);
	TEST_EXPECT(status(V::MissionCycle, { "OK - Server is cycling..." }) == 200);
	TEST_EXPECT(error(V::MissionCycle, { "ERROR - Must be in Game State to cycle server." }) == "not_in_game");
	TEST_EXPECT(status(V::ChatSend, { "OK - Chat sent." }) == 200);
	TEST_EXPECT(status(V::ChatGet, { "" }) == 200 && status(V::MissionAvailable, { "" }) == 200);
	TEST_EXPECT(status(V::MissionList, { "No missions in queue." }) == 200);
	// A usage reply (a missing right) or no reply at all is the server's fault: 502.
	TEST_EXPECT(status(V::PlayerList, { "USAGE - [QUIT | GET]" }) == 502);
	TEST_EXPECT(status(V::ChatSend, {}) == 502);
	// The refusals carry our own words, never the server's text.
	TEST_EXPECT(hosted::console_outcome(V::PlayerBan, { "ERROR - Player #2 \"Bob\" doesn't have a PCID." })
					.message.find("Bob") == std::string::npos);
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "owner_list", test_owner_list },
		{ "compose", test_compose },
		{ "outcomes", test_outcomes },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted console commands\n");
	return 0;
}
