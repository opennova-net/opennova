// The retail host file (runtime/inmatch/host_file.h): the game.cfg walk it is
// read through (io::for_each_config_file_line), every ServerConfig_ApplyHostSetting
// arm onto the host-screen state, and the Mission lines' rotation seed.
#include <base/gameprofile/game_type.h>
#include <base/io/ascii_config.h>
#include <formats/mission/bms.h>
#include <runtime/inmatch/host_file.h>

#include "common/test_expect.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;

namespace {

std::vector<std::string> first_tokens(const std::string &text) {
	std::vector<std::string> out;
	io::for_each_config_file_line(text.data(), text.size(), [&](io::ConfigTokens &line) {
		out.push_back(std::string(line.token(0)) + "|" + line.token(1));
	});
	return out;
}

mission_catalog::Row row(const char *file, bms::AttribFlags mode, bool loose = false) {
	mission_catalog::Row r;
	r.file = file;
	r.game_mode = static_cast<uint32_t>(mode);
	r.loose = loose;
	return r;
}

} // namespace

int main() {
	// --- the walk: CR LF reads as one line end, a lone LF ends a line too, a
	//     comment or a '/'-led first token skips, quotes keep spaces, and the
	//     tail line keeps its last byte (fgets, not the CR LF split).
	{
		const std::vector<std::string> lines = first_tokens(
				"// host file\r\n"
				"GameName \"My Server\"\r\n"
				"\r\n"
				"MaxPlayers 24 ; trailing comment\n"
				"/MaxPlayers 99\n"
				"Mission,ABC.BMS,1\n"
				"KillLimit 7");
		TEST_EXPECT(lines.size() == 4);
		TEST_EXPECT(lines[0] == "GameName|My Server");
		TEST_EXPECT(lines[1] == "MaxPlayers|24");
		TEST_EXPECT(lines[2] == "Mission|ABC.BMS");
		TEST_EXPECT(lines[3] == "KillLimit|7");
	}
	// --- a line longer than fgets' 1023 characters arrives in pieces; the
	//     second piece is a line of its own.
	{
		const std::string text = "GameName " + std::string(1100, 'x') + "\nTracers 0\n";
		std::vector<std::string> lines = first_tokens(text);
		TEST_EXPECT(lines.size() == 3);
		TEST_EXPECT(lines[1] == std::string(1100 - (1023 - 9), 'x') + "|");
		TEST_EXPECT(lines[2] == "Tracers|0");
	}

	const std::vector<mission_catalog::Row> catalog = {
		row("CTF_A.BMS", bms::AttribFlags::CaptureTheFlag),
		row("COOP_B.BMS", bms::AttribFlags::Coop, /*loose=*/true),
		row("DM_C.BMS", bms::AttribFlags::Deathmatch),
	};

	// --- the host-screen keys: case-insensitive, the dialog's caps and rule
	//     bits, atol values, MaxPlayers without the dialog's 64 clamp.
	{
		HostScreenState host;
		MissionRotation rotation;
		TEST_EXPECT(host.player_limit == 64 && host.use_lineup_queue == 1 &&
				host.lineup_queue_size == 100 && host.game_type_setting == 0);
		TEST_EXPECT(host.config.score_limit == game_rules::kDefaultScoreLimit);
		const std::string text =
				"gamename \"A server name well past the thirty-two byte cap\"\n"
				"MPHostGamePassword secret\n"
				"ServerMessage \"Welcome, all\"\n"
				"GameLocation USA\n"
				"ConnectionSpeed 3\n"
				"Replay 0\n"
				"Delay 12\n"
				"Respawn 9\n"
				"Time_Limit 25\n"
				"KillLimit 500\n"
				"MaxScore 7\n"
				"MaxPlayers 100\n"
				"UseLineUpQueue 0\n"
				"LineUpQueueSize 40\n"
				"MaxFFKills 4\n"
				"TakeoverTime -1\n"
				"TeamFF 0\n"
				"FriendlyTag 1\n"
				"FFWarning 0\n"
				"TeamChoose 0\n"
				"ClaymorePref 1\n"
				"Tracers 1\n"
				"MPHostSidePasswordA blue\n"
				"MPHostSidePasswordB red\n"
				"GameType 65540\n"
				"NotAKey 1\n";
		const HostFileReport report = read_host_file(text.data(), text.size(), host, rotation, catalog);
		TEST_EXPECT(report.lines == 26);
		TEST_EXPECT(report.unknown_keys.size() == 1 && report.unknown_keys[0] == "NotAKey");
		TEST_EXPECT(host.config.server_name == "A server name well past the thir"); // strncpy 0x20
		TEST_EXPECT(host.config.server_password == "secret");
		TEST_EXPECT(host.config.custom_text == "Welcome, all");
		TEST_EXPECT(host.config.country == "USA");
		TEST_EXPECT(host.config.connection_speed == 3);
		TEST_EXPECT(host.config.replay_enabled == 0);
		TEST_EXPECT(host.config.start_delay == 12);
		TEST_EXPECT(host.config.respawn_timeout == 9);
		TEST_EXPECT(host.config.respawn_time == 25);
		TEST_EXPECT(host.config.score_limit == 65000u); // the 500-point sentinel
		TEST_EXPECT(host.config.max_score == 7);
		TEST_EXPECT(host.player_limit == 100);
		TEST_EXPECT(host.use_lineup_queue == 0 && host.lineup_queue_size == 40);
		TEST_EXPECT(host.config.max_friendly_kills == 4);
		TEST_EXPECT(host.config.capture_duration_seconds == -1);
		const uint32_t attrib = host.config.mp_attributes;
		TEST_EXPECT((attrib & GameConfig::kMpAttribNoFriendlyFire) != 0);  // TeamFF 0
		TEST_EXPECT((attrib & GameConfig::kMpAttribNoFriendlyTag) == 0);   // FriendlyTag 1
		TEST_EXPECT((attrib & GameConfig::kMpAttribFFWarningSuppress) != 0); // FFWarning 0
		TEST_EXPECT((attrib & GameConfig::kMpAttribTeamChoose) == 0);      // TeamChoose 0
		TEST_EXPECT((attrib & GameConfig::kMpAttribClaymorePref) != 0);    // ClaymorePref 1
		TEST_EXPECT((attrib & GameConfig::kMpAttribNoTracers) == 0);       // Tracers 1
		TEST_EXPECT(host.config.side_a_password == "blue" && host.config.side_b_password == "red");
		TEST_EXPECT(host.game_type_setting == 65540);
		TEST_EXPECT(host.serve_and_play); // the file never writes SERVERTYPE
		TEST_EXPECT(rotation.entries.empty() && rotation.current() == nullptr);
	}

	// --- the Mission lines: a catalog row joins the rotation, an unknown file
	//     changes nothing; the last accepted line names the starting map and the
	//     cursor; the launch option survives only on a team, non-objective row.
	{
		HostScreenState host;
		MissionRotation rotation;
		const std::string text =
				"Mission ctf_a.bms 1\n"
				"Mission MISSING.BMS 1\n"
				"Mission COOP_B.BMS 1\n"
				"Mission dm_c.bms 1\n"
				"Mission CTF_A.BMS\n"
				"Mission COOP_B.BMS 1\n";
		const HostFileReport report = read_host_file(text.data(), text.size(), host, rotation, catalog);
		TEST_EXPECT(report.unknown_missions.size() == 1 && report.unknown_missions[0] == "MISSING.BMS");
		TEST_EXPECT(rotation.entries.size() == 5);
		TEST_EXPECT(rotation.entries[0].catalog_index == 0 && rotation.entries[1].catalog_index == 1 &&
				rotation.entries[2].catalog_index == 2 && rotation.entries[3].catalog_index == 0 &&
				rotation.entries[4].catalog_index == 1);
		// The cursor lands on the FIRST rotation entry with the last line's name.
		TEST_EXPECT(rotation.cursor == 1 && rotation.current() == &rotation.entries[1]);
		TEST_EXPECT(rotation.alt_cursor == -1);
		TEST_EXPECT(rotation.map_file == "COOP_B.BMS");
		TEST_EXPECT(rotation.map_source_is_loose);
		TEST_EXPECT(rotation.map_game_type == game_type::kObjectiveCoop);
		// CTF (team, no objective bit) keeps its option -- the last CTF line set
		// it to 0; objective Co-op and solo DM lose theirs.
		TEST_EXPECT(rotation.launch_options.size() == catalog.size());
		TEST_EXPECT(rotation.launch_options[0] == 0);
		TEST_EXPECT(rotation.launch_options[1] == 0 && rotation.map_launch_option == 0);
		TEST_EXPECT(rotation.launch_options[2] == 0);

		HostScreenState host2;
		MissionRotation rotation2;
		const std::string ctf = "Mission CTF_A.BMS 1\n";
		read_host_file(ctf.data(), ctf.size(), host2, rotation2, catalog);
		TEST_EXPECT(rotation2.launch_options[0] == 1 && rotation2.map_launch_option == 1);
		TEST_EXPECT(rotation2.map_game_type == game_type::kCaptureTheFlag);
		TEST_EXPECT(!rotation2.map_source_is_loose);
	}

	// --- the sample host file the apps zip ships (apps/serve/example.host):
	//     every key is a host-file key, and every setting it writes is the
	//     host screen's default, so the sample leaves the defaults standing.
	{
		std::ifstream in(OPENNOVA_EXAMPLE_HOST, std::ios::binary);
		TEST_EXPECT(static_cast<bool>(in));
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const std::vector<mission_catalog::Row> sample_catalog = {
			row("EXAMPLE01.BMS", bms::AttribFlags::TeamDeathmatch),
			row("EXAMPLE02.BMS", bms::AttribFlags::CaptureTheFlag),
		};
		HostScreenState host;
		HostScreenState defaults;
		defaults.config.server_name = "Untitled";
		defaults.config.country = "USA";
		MissionRotation rotation;
		const HostFileReport report =
				read_host_file(text.data(), text.size(), host, rotation, sample_catalog);
		TEST_EXPECT(report.unknown_keys.empty());
		TEST_EXPECT(report.unknown_missions.empty());
		TEST_EXPECT(host.config.server_name == defaults.config.server_name);
		TEST_EXPECT(host.config.custom_text == defaults.config.custom_text);
		TEST_EXPECT(host.config.country == defaults.config.country);
		TEST_EXPECT(host.config.server_password.empty() && host.config.side_a_password.empty() &&
				host.config.side_b_password.empty());
		TEST_EXPECT(host.config.mp_attributes == defaults.config.mp_attributes);
		TEST_EXPECT(host.config.connection_speed == defaults.config.connection_speed);
		TEST_EXPECT(host.config.replay_enabled == defaults.config.replay_enabled);
		TEST_EXPECT(host.config.start_delay == defaults.config.start_delay);
		TEST_EXPECT(host.config.respawn_timeout == defaults.config.respawn_timeout);
		TEST_EXPECT(host.config.respawn_time == defaults.config.respawn_time);
		TEST_EXPECT(host.config.score_limit == defaults.config.score_limit);
		TEST_EXPECT(host.config.max_score == defaults.config.max_score);
		TEST_EXPECT(host.config.max_friendly_kills == defaults.config.max_friendly_kills);
		TEST_EXPECT(host.config.capture_duration_seconds == defaults.config.capture_duration_seconds);
		TEST_EXPECT(host.player_limit == defaults.player_limit);
		TEST_EXPECT(host.game_type_setting == defaults.game_type_setting);
		TEST_EXPECT(host.use_lineup_queue == defaults.use_lineup_queue);
		TEST_EXPECT(host.lineup_queue_size == defaults.lineup_queue_size);
		// Two rotation entries; the last line names the starting map.
		TEST_EXPECT(rotation.entries.size() == 2);
		TEST_EXPECT(rotation.map_file == "EXAMPLE02.BMS");
	}
	std::printf("host_file: ok\n");
	return 0;
}
