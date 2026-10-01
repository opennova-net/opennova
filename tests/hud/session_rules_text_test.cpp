// The CMAP RULES tab's text (hud/session_rules_text.h) and its sources: the
// S2C 0x58 session-status fold with the retail truncations and the elapsed
// read, the S2C 0x7E briefing strings with the strncpy run-on, the replica
// dispatch arms (and the 0x0F reset), and the composition itself — the
// status block, the win conditions, the rules key by game type and team, the
// briefing, the gated points / penalties and the stat explanations.
// [orig: Overlay_BuildEndGameStatsText @0x54a240; HUD_BuildRulesAndBriefingText
//  @0x5b92d0; HUD_FormatEndGameConditionText @0x5bce80;
//  SessionStatus_ParseFromBuffer @0x530ed0; NapiNPClientMsg_ServerConfigStrings
//  @0x425e20]
#include <runtime/hud/session_rules_text.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/session_status.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/world/world.h>
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

static GameTextLookup table_of(std::map<std::string, std::string> rows) {
	return [rows](const char *section, const char *key, const char *fallback) {
		const auto it = rows.find(std::string(section) + "/" + key);
		return it != rows.end() ? it->second : std::string(fallback);
	};
}

static bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

static void check_session_status_fold() {
	// The fold keeps 31 / 63 characters, the three bytes, the uptime and the
	// stats, then the first 8 pairs whose key is 9 or less.
	SessionStatusBlock block;
	block.server_name = std::string(40, 'S');
	block.mission_name = std::string(70, 'M');
	block.byte0 = 0x20;
	block.byte1 = 2;
	block.byte2 = 16;
	block.uptime_ms = 5000;
	block.stat_values[3] = 5;
	block.stat_values[4] = -3;
	const uint8_t keys[] = {1, 12, 2, 3, 4, 10, 5, 6, 7, 8, 9};
	for (uint8_t k : keys) block.kv.push_back({k, uint32_t(k) * 10u});
	const replication::ClientSessionStatus s = replication::fold_session_status(block, 1000);
	CHECK(s.valid);
	CHECK(s.server_name == std::string(31, 'S'));
	CHECK(s.mission_name == std::string(63, 'M'));
	CHECK(s.game_type_byte == 0x20 && s.score_table == 2 && s.max_players == 16);
	CHECK(s.uptime_ms == 5000 && s.stamp_ms == 1000);
	CHECK(s.stats[3] == 5 && s.stats[4] == -3);
	CHECK(s.options.size() == 8);
	CHECK(s.options[0].key == 1 && s.options[1].key == 2 && s.options[6].key == 7 &&
			s.options[7].key == 8 && s.options[7].value == 80);
	// Elapsed: the uptime plus the time since the stamp; 0 while not valid.
	CHECK(replication::session_status_elapsed_ms(s, 4000) == 8000);
	CHECK(replication::session_status_elapsed_ms(replication::ClientSessionStatus{}, 4000) == 0);
}

static void check_server_config_strings() {
	replication::ClientServerConfigStrings strings;
	const std::vector<uint8_t> body = {'B', '3', 0, 'B', '2', 0};
	replication::fold_server_config_strings(body, strings);
	CHECK(strings.first == "B3" && strings.second == "B2");
	CHECK(replication::server_config_first_text(strings) == "B3");
	CHECK(replication::server_config_second_text(strings) == "B2");
	// No NUL: the first runs to the body end and the second is empty.
	replication::fold_server_config_strings({'X', 'Y'}, strings);
	CHECK(strings.first == "XY" && strings.second.empty());
	// A second string of 1024+ characters leaves byte_A86120 unterminated: its
	// read runs on into byte_A86520.
	strings.first = "tail";
	strings.second = std::string(1030, 'b');
	CHECK(replication::server_config_second_text(strings) == std::string(1024, 'b') + "tail");
	strings.first = std::string(1100, 'a');
	CHECK(replication::server_config_first_text(strings).size() == 1024);
}

static void check_replica_arms() {
	// The authority's own report, through the client's 0x58 arm.
	inmatch::GameConfig config;
	config.server_name = "Host";
	config.mission_name = "Mission";
	config.game_type = game_type::kTeamDeathmatch;
	config.max_players = 12;
	config.score_limit = 50;
	world::World w;
	const std::vector<uint8_t> body = inmatch::serialize_session_status(config, 61000, true, &w);
	replication::ClientReplicaPipeline pipeline;
	pipeline.state().local_clock_ms = 32;
	pipeline.apply(s2c::SESSION_STATUS, body);
	const replication::ClientSessionStatus &s = pipeline.state().session_status;
	CHECK(s.valid && s.server_name == "Host" && s.mission_name == "Mission");
	CHECK(s.max_players == 12 && s.uptime_ms == 61000 && s.stamp_ms == 32);
	CHECK(!s.options.empty() && s.options[0].key == 1 && s.options[0].value == 50);
	// A truncated body still lands valid with what it carried (the lenient
	// cursor): only the names.
	pipeline.apply(s2c::SESSION_STATUS, {'A', 0, 'B', 0});
	CHECK(pipeline.state().session_status.valid &&
			pipeline.state().session_status.server_name == "A" &&
			pipeline.state().session_status.uptime_ms == 0 &&
			pipeline.state().session_status.options.empty());
	pipeline.apply(s2c::SERVER_CONFIG_STRINGS, {'x', 0, 'y', 'z', 0});
	CHECK(pipeline.state().server_config_strings.first == "x");
	CHECK(pipeline.state().server_config_strings.second == "yz");
}

static void check_co_op_key_nine() {
	// Co-op's key 9 is the defined-subgoal count, sent only when nonzero; key
	// 8 needs a session [orig: Server_BuildStatusReport @0x530cb0..0x530ce1,
	// @0x530e71].
	inmatch::GameConfig config;
	config.game_type = game_type::kCoop;
	config.respawn_time = 30;
	world::World w;
	SessionStatusBlock block;
	std::vector<uint8_t> body = inmatch::serialize_session_status(config, 0, true, &w);
	CHECK(decode_session_status(body.data(), body.size(), block));
	CHECK(block.kv.size() == 1 && block.kv[0].key == 8);
	w.script.subgoals.win_text_ids[1] = 7;
	w.script.subgoals.win_text_ids[2] = 8;
	w.script.subgoals.win_text_ids[3] = 0xFF;
	w.script.subgoals.win_text_ids[4] = 9;
	body = inmatch::serialize_session_status(config, 0, false, &w);
	CHECK(decode_session_status(body.data(), body.size(), block));
	CHECK(block.kv.size() == 1 && block.kv[0].key == 9 && block.kv[0].value == 2);
}

static void check_composition() {
	const GameTextLookup table = table_of({
			{"Overlays/STROVER_SERVERNAME", "Server:"},
			{"Overlays/STROVER_MISSIONNAME", "Mission:"},
			{"Overlays/STROVER_UPTIME", "Uptime:"},
			{"Overlays/STROVER_GAMETYPE", "Game type:"},
			{"Overlays/STROVER64", "Team Deathmatch"},
			{"Overlays/STROVER_MAXPLAYERSALLOWED", "Max:"},
			{"Overlays/STROVER_NUMPLAYERS", "Num:"},
			{"Overlays/STROVER_ENDGAMECOND_TITLEWIN", "Win:"},
			{"Overlays/STROVER_ENDGAMECOND_ENEMYKILLS", "%ld kills"},
			{"Overlays/STROVER_ENDGAMECOND_TIMELIMIT", "%ld minutes"},
			{"Overlays/STROVER_RULES_TDM_B", "Blue TDM rules"},
			{"Overlays/STROVER_RULES_PSPGAMES", "PSP rules"},
			{"Overlays/STROVER_RULEPOINTS", "Points:"},
			{"Overlays/STROVER_RULEPENALTIES", "Penalties:"},
			{"Overlays/STROVER_STATVAR03", "Enemy kill"},
			{"Overlays/STROVER_STATVAREXP03", "Kill an enemy."},
			{"Overlays/STROVER_STATVAR29", "Snipe min"},
			{"Overlays/METERS", "m"},
	});
	SessionStatusView status;
	status.valid = true;
	status.server_name = "Host";
	status.mission_name = "Bridge";
	status.max_players = 16;
	status.elapsed_ms = 90061000; // 1 day 01:01:01
	status.stats[3] = 5;          // a point
	status.stats[4] = -3;         // a penalty, fallback name without "!" in its explanation
	status.stats[29] = 50;        // a distance
	status.stats[12] = 7;         // gated: no stat 31 / 32 outside KOTH
	status.options = {{1, 30}, {8, 20}};
	RulesBriefingInputs inputs;
	inputs.game_type = game_type::kTeamDeathmatch;
	inputs.local_team = 1;
	inputs.spawn_zones = true;
	inputs.authority = true;
	inputs.briefing2 = "Take the bridge.";
	std::string out;
	CHECK(build_end_game_stats_text(status, 9, inputs, table, out));
	const std::string expected_head =
			"Server:\tHost\r\n"
			"Mission:\tBridge\r\n"
			"Uptime:\t1 01:01:01\r\n"
			"Game type:\tTeam Deathmatch\r\n"
			"Max:\t16\r\n"
			"Num:\t9\r\n"
			"\r\n"
			"Win:\t30 kills\r\n"
			"\t20 minutes\r\n"
			"\r\n\r\n\r\n"
			"Blue TDM rules\r\n\r\nPSP rules\r\n"
			"Take the bridge."
			"\r\n\r\n\r\n\r\n"
			"Points:\r\n"
			"Enemy kill:\t+5\r\n"
			"Snipe min:\t50 m\r\n"
			"\r\n"
			"Penalties:\r\n"
			"!STROVER_STATVAR04:\t-3\r\n"
			"\r\n\r\n\r\n";
	if (out.compare(0, expected_head.size(), expected_head) != 0) {
		std::string shown;
		for (char c : out.substr(0, expected_head.size() + 40))
			shown += c == '\r' ? std::string("\\r") : c == '\n' ? std::string("\\n\n")
					: c == '\t' ? std::string("\\t") : std::string(1, c);
		std::printf("--- built text ---\n%s\n---\n", shown.c_str());
	}
	CHECK(out.compare(0, expected_head.size(), expected_head) == 0);
	CHECK(contains(out, "<b>Enemy kill<-b>\r\nKill an enemy.\r\n\r\n"));
	// Stat 12 is explained (no gate there) though its points line was gated.
	CHECK(contains(out, "<b>!STROVER_STATVAR12<-b>\r\n!STROVER_STATVAREXP12\r\n\r\n"));
	CHECK(!contains(out, "!STROVER_STATVAR12:"));
	// The negative walk's fallbacks carry no "!".
	CHECK(contains(out, "<b>STROVER_STATVAR04<-b>\r\nSTROVER_STATVAREXP04\r\n\r\n"));

	// KOTH: stat 12 shows (in seconds) when stat 33 is set.
	status.stats[33] = 1;
	inputs.game_type = game_type::kKingOfTheHill;
	CHECK(build_end_game_stats_text(status, 9, inputs, table, out));
	CHECK(contains(out, "!STROVER_STATVAR12:\t7 !seconds\r\n"));
	// KOTH's key misses to its "!" fallback: no team suffix, no PSP paragraph.
	CHECK(contains(out, "\t20 minutes\r\n\r\n\r\n\r\n!\r\nTake the bridge."));
	// Stat 36 is gated on stat 35.
	status.stats[36] = 2;
	CHECK(build_end_game_stats_text(status, 9, inputs, table, out));
	CHECK(!contains(out, "!STROVER_STATVAR36:"));
	status.stats[35] = 1;
	CHECK(build_end_game_stats_text(status, 9, inputs, table, out));
	CHECK(contains(out, "!STROVER_STATVAR36:\t2 !seconds\r\n"));

	// Not valid: nothing is built.
	status.valid = false;
	out = "kept";
	CHECK(!build_end_game_stats_text(status, 9, inputs, table, out));
	CHECK(out == "kept");
}

static void check_rules_and_briefing() {
	const GameTextLookup empty = table_of({});
	RulesBriefingInputs in;
	std::string out;
	// Co-op: no rules and no line break before the briefing.
	in.game_type = game_type::kCoop;
	in.authority = true;
	in.briefing3 = "Three";
	in.briefing2 = "Two";
	append_rules_and_briefing_text(out, in, false, empty);
	CHECK(out == "Two");
	out.clear();
	append_rules_and_briefing_text(out, in, true, empty);
	CHECK(out == "Three");
	out.clear();
	in.briefing3.clear();
	append_rules_and_briefing_text(out, in, true, empty);
	CHECK(out == "Two");
	// CTF team 2: the "_R" key, no PSP paragraph even with spawn zones.
	out.clear();
	in.game_type = game_type::kCaptureTheFlag;
	in.local_team = 2;
	in.spawn_zones = true;
	append_rules_and_briefing_text(out, in, false, empty);
	CHECK(out == "!\r\nTwo");
	// A client reads the 0x7E strings: the second, or the first under the flag
	// when it is not empty.
	out.clear();
	in.authority = false;
	in.game_type = game_type::kSearchAndDestroy;
	in.local_team = 0; // "_G"
	in.spawn_zones = false;
	in.config_first = "First";
	in.config_second = "Second";
	append_rules_and_briefing_text(out, in, false, table_of({
			{"Overlays/STROVER_RULES_SD_G", "SD green"}}));
	CHECK(out == "SD green\r\nSecond");
	out.clear();
	append_rules_and_briefing_text(out, in, true, empty);
	CHECK(out == "!\r\nFirst");
	out.clear();
	in.config_first.clear();
	append_rules_and_briefing_text(out, in, true, empty);
	CHECK(out == "!\r\nSecond");
	// An unlisted game type: no rules, then the line break.
	out.clear();
	in.game_type = game_type::kFlagMe;
	append_rules_and_briefing_text(out, in, false, empty);
	CHECK(out == "\r\nSecond");
	// The condition formats: a table format, else "!?? %ld".
	CHECK(format_end_game_condition(8, 20, table_of({
			{"Overlays/STROVER_ENDGAMECOND_TIMELIMIT", "%ld min"}})) == "20 min");
	CHECK(format_end_game_condition(0, 5, empty) == "!?? 5");
}

int main() {
	check_session_status_fold();
	check_server_config_strings();
	check_replica_arms();
	check_co_op_key_nine();
	check_composition();
	check_rules_and_briefing();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("session_rules_text_test OK\n");
	return 0;
}
