// The retail host file (runtime/inmatch/host_file.h): the game.cfg walk it is
// read through (io::for_each_config_file_line), every ServerConfig_ApplyHostSetting
// arm onto the game.cfg block, and the Mission lines' rotation seed.
#include <base/gameprofile/game_type.h>
#include <base/io/ascii_config.h>
#include <formats/gamecfg/game_cfg.h>
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

	// --- the keys onto the cfg block: case-insensitive, each arm's strncpy
	//     count, atol values, the mpattrib switches, MaxPlayers unclamped.
	{
		gamecfg::GameCfg cfg = gamecfg::defaults();
		cfg.country = "Germany"; // GameLocation's strncpy 3 leaves the tail
		HostRotation rotation;
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
				"MPHostSidePasswordB abcdefghijklmnopqrstu\n"
				"GameType 65540\n"
				"NotAKey 1\n";
		const HostFileReport report = read_host_file(text.data(), text.size(), cfg, rotation, catalog);
		TEST_EXPECT(report.lines == 26);
		TEST_EXPECT(report.unknown_keys.size() == 1 && report.unknown_keys[0] == "NotAKey");
		// strncpy 0x20 into char[32]: the field holds 31 (D-GAMECFG-1's bound).
		TEST_EXPECT(cfg.game_name == "A server name well past the thi");
		TEST_EXPECT(cfg.mp_host_game_password == "secret");
		TEST_EXPECT(cfg.servermsg == "Welcome, all");
		TEST_EXPECT(cfg.country == "USAmany"); // three bytes, no NUL, the old tail reads on
		TEST_EXPECT(cfg.nwisptype == 3);
		TEST_EXPECT(cfg.replay == 0);
		TEST_EXPECT(cfg.startdelay == 12);
		TEST_EXPECT(cfg.timeout == 9);
		TEST_EXPECT(cfg.time_limit == 25);
		TEST_EXPECT(cfg.max_kills == 500); // the sentinel is the apply's, not the arm's
		TEST_EXPECT(cfg.max_score == 7);
		TEST_EXPECT(cfg.mp_max_players == 100);
		TEST_EXPECT(cfg.mp_use_lineup_queue == 0 && cfg.mp_lineup_queue_size == 40);
		TEST_EXPECT(cfg.numallowablefriendlykills == 4);
		TEST_EXPECT(cfg.teamchange_time == -1);
		const uint32_t attrib = static_cast<uint32_t>(cfg.mpattrib);
		TEST_EXPECT((attrib & GameConfig::kMpAttribNoFriendlyFire) != 0);  // TeamFF 0
		TEST_EXPECT((attrib & GameConfig::kMpAttribNoFriendlyTag) == 0);   // FriendlyTag 1
		TEST_EXPECT((attrib & GameConfig::kMpAttribFFWarningSuppress) != 0); // FFWarning 0
		TEST_EXPECT((attrib & GameConfig::kMpAttribTeamChoose) == 0);      // TeamChoose 0
		TEST_EXPECT((attrib & GameConfig::kMpAttribClaymorePref) != 0);    // ClaymorePref 1
		TEST_EXPECT((attrib & GameConfig::kMpAttribNoTracers) == 0);       // Tracers 1
		TEST_EXPECT(cfg.mp_host_side_password_a == "blue");
		TEST_EXPECT(cfg.mp_host_side_password_b == "abcdefghijklmnop"); // strncpy 0x11 into [17]
		TEST_EXPECT(cfg.mp_gametype == 65540);
		TEST_EXPECT(cfg.dedicated == 0); // the file never writes SERVERTYPE
		TEST_EXPECT(rotation.list.count == 0 && rotation.list.current() == nullptr);

		// A short text value copies its NUL: the old tail is gone.
		const std::string short_location = "GameLocation UK\n";
		read_host_file(short_location.data(), short_location.size(), cfg, rotation, catalog);
		TEST_EXPECT(cfg.country == "UK");
	}

	// --- the Mission lines: a catalog row joins the rotation, an unknown file
	//     changes nothing; the last accepted line names the starting map and the
	//     cursor; the launch option survives only on a team, non-objective row.
	{
		gamecfg::GameCfg cfg = gamecfg::defaults();
		HostRotation rotation;
		const std::string text =
				"Mission ctf_a.bms 1\n"
				"Mission MISSING.BMS 1\n"
				"Mission COOP_B.BMS 1\n"
				"Mission dm_c.bms 1\n"
				"Mission CTF_A.BMS\n"
				"Mission COOP_B.BMS 1\n";
		const HostFileReport report = read_host_file(text.data(), text.size(), cfg, rotation, catalog);
		TEST_EXPECT(report.unknown_missions.size() == 1 && report.unknown_missions[0] == "MISSING.BMS");
		TEST_EXPECT(rotation.list.count == 5);
		const MissionRotation &list = rotation.list;
		TEST_EXPECT(list.entry(0).catalog_index == 0 && list.entry(1).catalog_index == 1 &&
				list.entry(2).catalog_index == 2 && list.entry(3).catalog_index == 0 &&
				list.entry(4).catalog_index == 1);
		TEST_EXPECT(list.entry(0).flag == 0 && list.entry(4).flag == 0);
		// The cursor lands on the FIRST rotation entry with the last line's name.
		TEST_EXPECT(list.cursor == 1 && list.current() == &list.entry(1));
		TEST_EXPECT(list.alt_cursor == -1);
		TEST_EXPECT(list.map_file == "COOP_B.BMS");
		TEST_EXPECT(list.map_source_is_loose);
		TEST_EXPECT(list.map_game_type == game_type::kObjectiveCoop);
		// The starting row's mode is the previous-mode word auto-balance reads
		// [orig: @0x4A658D].
		TEST_EXPECT(rotation.previous_game_type == game_type::kObjectiveCoop);
		// CTF (team, no objective bit) keeps its option -- the last CTF line set
		// it to 0; objective Co-op and solo DM lose theirs.
		TEST_EXPECT(list.launch_options.size() == catalog.size());
		TEST_EXPECT(list.launch_options[0] == 0);
		TEST_EXPECT(list.launch_options[1] == 0 && list.map_launch_option == 0);
		TEST_EXPECT(list.launch_options[2] == 0);

		gamecfg::GameCfg cfg2 = gamecfg::defaults();
		HostRotation rotation2;
		const std::string ctf = "Mission CTF_A.BMS 1\n";
		read_host_file(ctf.data(), ctf.size(), cfg2, rotation2, catalog);
		TEST_EXPECT(rotation2.list.launch_options[0] == 1 && rotation2.list.map_launch_option == 1);
		TEST_EXPECT(rotation2.list.map_game_type == game_type::kCaptureTheFlag);
		TEST_EXPECT(!rotation2.list.map_source_is_loose);
	}

	// --- the sample host file the apps zip ships (apps/serve/example.host):
	//     every key is a host-file key, and every setting it writes but the
	//     location is the game's default (JO:CA's gametext names), so over a
	//     defaults block the sample leaves the block as it was.
	{
		std::ifstream in(OPENNOVA_EXAMPLE_HOST, std::ios::binary);
		TEST_EXPECT(static_cast<bool>(in));
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const std::vector<mission_catalog::Row> sample_catalog = {
			row("EXAMPLE01.BMS", bms::AttribFlags::TeamDeathmatch),
			row("EXAMPLE02.BMS", bms::AttribFlags::CaptureTheFlag),
		};
		gamecfg::DefaultTexts texts;
		texts.untitled = "Untitled";
		texts.user_message = "Put your message here.";
		gamecfg::GameCfg cfg = gamecfg::defaults(texts);
		gamecfg::GameCfg expected = gamecfg::defaults(texts);
		expected.country = "USA";
		HostRotation rotation;
		const HostFileReport report =
				read_host_file(text.data(), text.size(), cfg, rotation, sample_catalog);
		TEST_EXPECT(report.unknown_keys.empty());
		TEST_EXPECT(report.unknown_missions.empty());
		// The block writes back to the same text: no field the file names moved.
		TEST_EXPECT(gamecfg::write(cfg, {}) == gamecfg::write(expected, {}));
		// Two rotation entries; the last line names the starting map.
		TEST_EXPECT(rotation.list.count == 2);
		TEST_EXPECT(rotation.list.map_file == "EXAMPLE02.BMS");
	}
	std::printf("host_file: ok\n");
	return 0;
}
