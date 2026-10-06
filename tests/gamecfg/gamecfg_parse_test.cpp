// game.cfg reader edge cases over hand-authored text: comments and quoting
// through the shared tokenizer, the value as token 2, unknown and dead keys,
// the cfg table's precedence and case folding, the per-key folds, the version
// fallback to the defaults (player_index kept), mpreset, the /LAN switch, the
// string bounds, avail_wpn against a roster, the graphics clamp, and the CRT
// fixed-point print the writer uses for its floats. The witnesses are cited in
// engine/formats/gamecfg/game_cfg.{h,cpp} and docs/gamecfg/game-cfg-re.md.
#include "common/test_expect.h"

#include <formats/gamecfg/game_cfg.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

using namespace opennova::gamecfg;

namespace {

// The text as a load reads it: version 29 first unless the case is about it.
LoadResult load_text(const std::string &text, const LoadOptions &options = {}) {
	return load(text.data(), text.size(), options);
}

std::string with_version(const std::string &body) {
	return "version = 29\r\n" + body;
}

int defaults_match_config_set_defaults() {
	const GameCfg d = defaults();
	TEST_EXPECT(d.version == 29);
	TEST_EXPECT(d.gamma == 1.0f);
	TEST_EXPECT(d.lock_framerate == 1 && d.force_vsync == 0 && d.reduce_mouselag == 1);
	TEST_EXPECT(d.windows_volume == 255 && d.music_volume == 192 && d.audio_rate == 44100);
	TEST_EXPECT(d.showgun == 3 && d.hud_color_index == 2);
	TEST_EXPECT(d.mp_eula_accepted == -1 && d.mpattrib == 14850);
	TEST_EXPECT(d.game_name == "!Untitled" && d.servermsg == "!Put your message here");
	TEST_EXPECT(d.internet_address == "0.0.0.0" && d.mp_ip_address_string == "0.0.0.0");
	TEST_EXPECT(d.max_team_lives == 100 && d.max_kills == 50 && d.max_score == 5 && d.side_req == -1);
	TEST_EXPECT(d.nwisptype == 5 && d.lanmode == 1 && d.maxping == 1500);
	TEST_EXPECT(d.flag_return_time == 210 && d.flag_reset_time == 420);
	// Float objects, not literals: an x87 build may hold a literal at long double
	// precision, where a stored float has been rounded.
	const float sixty_six = 0.66f;
	TEST_EXPECT(d.mpvoting_percent == sixty_six && d.balance_join_percent == 0.5f);
	TEST_EXPECT(d.xhair_color == 0xFFFFFF && d.armory_reuse_time == 30 && d.farp_reuse_time == 90);
	TEST_EXPECT(d.play_exit_credits == -1 && d.alt_lock == 1 && d.alt_lock_type == 1);
	TEST_EXPECT(d.mp_gate_port_max == 65536 && d.mp_lan_server_port_max == 32787);
	TEST_EXPECT(d.mp_max_players == 64 && d.mp_use_lineup_queue == 1 && d.mp_lineup_queue_size == 100);
	TEST_EXPECT(d.mp_max_packet_size == 1300 && d.mp_host_punt_same_pcids == 1);
	TEST_EXPECT(d.mp_tod_continuity == 0); // the table says 1; Config_SetDefaults overwrites it
	TEST_EXPECT(d.weapon_availability[0] == 3 && d.weapon_availability[kWeaponAvailabilityCount - 1] == 3);
	TEST_EXPECT(d.class_availability[0] == 1 && d.class_availability[kClassEngineer] == 1);
	TEST_EXPECT(d.water_quality == 0); // the defaults; the load's clamp raises it to 1

	DefaultTexts texts;
	texts.untitled = "Untitled";
	texts.user_message = "Put your message here.";
	const GameCfg localized = defaults(texts, 4, 1);
	TEST_EXPECT(localized.game_name == "Untitled" && localized.servermsg == "Put your message here.");
	TEST_EXPECT(localized.player_index == 4 && localized.hw3d_deviceno == 1);
	return 0;
}

int comments_and_quoting() {
	const LoadResult r = load_text(
			"// game.cfg\r\n"
			"; a semicolon comment\r\n"
			"version           = 29   // a trailing comment\r\n"
			"game_name = \"Server, with a comma\" ; and a tail\r\n"
			"country = \"N;Z\"\r\n"
			"   servermsg\t=\t\"keep // this\"\r\n"
			"/max_kills = 1\r\n" // a first token starting with '/' never reaches the reader
			"\r\n"
			"msg = \"two words\"\n" // LF alone ends a line as CR LF does
			"max_score = 9");     // no line end at all
	TEST_EXPECT(r.file_read && !r.version_reset);
	TEST_EXPECT(r.cfg.game_name == "Server, with a comma");
	TEST_EXPECT(r.cfg.country == "N;Z");
	TEST_EXPECT(r.cfg.servermsg == "keep // this");
	TEST_EXPECT(r.cfg.max_kills == 50);
	TEST_EXPECT(r.cfg.msg == "two words");
	TEST_EXPECT(r.cfg.max_score == 9);
	return 0;
}

int value_is_token_two() {
	const LoadResult r = load_text(with_version(
			"max_kills 99\r\n"        // no '=': the value is token 2, empty -> atol("") = 0
			"max_score : 7 8\r\n"     // anything in the '=' slot
			"time_limit = 12 13\r\n"  // a fourth token is ignored
			"koth_limit =\r\n"));     // an empty value
	TEST_EXPECT(r.cfg.max_kills == 0);
	TEST_EXPECT(r.cfg.max_score == 7);
	TEST_EXPECT(r.cfg.time_limit == 12);
	TEST_EXPECT(r.cfg.koth_limit == 0);
	return 0;
}

int unknown_and_dead_keys() {
	const std::string base = write(load_text(with_version("")).cfg, {});
	const LoadResult r = load_text(with_version(
			"bogus_key = 5\r\n"
			"zulu = 1\r\n"           // past the 'x' arm
			"1st = 2\r\n"            // no letter arm
			"multiplayer = 5\r\n"    // the cfg table's header row: matched, assigns nothing
			"side_password = \"x\"\r\n" // compared only under 't': dead
			"side_req = 3\r\n"       // likewise
			"flagreturntime = 99\r\n")); // the writer's spelling; the reader's is mp_flagreturntime
	TEST_EXPECT(write(r.cfg, {}) == base);

	const LoadResult live = load_text(with_version("mp_flagreturntime = 99\r\nmp_flagresettime = 4321\r\n"));
	TEST_EXPECT(live.cfg.flag_return_time == 99);
	TEST_EXPECT(live.cfg.flag_reset_time == 4321);
	const std::string text = write(live.cfg, {});
	TEST_EXPECT(text.find("flagreturntime          = 99\r\n") != std::string::npos);
	TEST_EXPECT(text.find("4321") == std::string::npos); // mp_flagresettime is never written
	return 0;
}

int case_folding_and_table_first() {
	const LoadResult r = load_text(with_version(
			"MAX_KILLS = 7\r\n"
			"MpMaxPlayers = \"12\"\r\n"
			"MPHOSTGAMEPASSWORD = \"secret\"\r\n"
			"mpuselineupqueue = \"5\"\r\n"   // bool: nonzero -> 1
			"mpreset = \"0\"\r\n"
			"sfxlevel = 10\r\n"));
	TEST_EXPECT(r.cfg.max_kills == 7);
	TEST_EXPECT(r.cfg.mp_max_players == 12);
	TEST_EXPECT(r.cfg.mp_host_game_password == "secret");
	TEST_EXPECT(r.cfg.mp_use_lineup_queue == 1);
	TEST_EXPECT(r.cfg.sfx_level == 10);
	return 0;
}

int folds_and_clamps() {
	LoadResult r = load_text(with_version(
			"startdelay = 999\r\nnwisptype = -1\r\nlanmode = 9\r\n"
			"terrain_polydetail = 9\r\nwater_quality = 0\r\nantialias_mode = 32\r\n"
			"particle_density = 5\r\nshadow_quality = -4\r\nshader_usage_level = 77\r\n"
			"gamma = 3.0\r\nmpmaxplayers = \"100\"\r\n"
			"remote_admin_port = 70000\r\nmax_kills = 99999999999\r\nmax_score = -99999999999\r\n"));
	TEST_EXPECT(r.cfg.startdelay == 300);
	TEST_EXPECT(r.cfg.nwisptype == 18);
	TEST_EXPECT(r.cfg.lanmode == 2);
	TEST_EXPECT(r.cfg.terrain_polydetail == 3);
	TEST_EXPECT(r.cfg.water_quality == 1);
	TEST_EXPECT(r.cfg.antialias_mode == 16);
	TEST_EXPECT(r.cfg.particle_density == 2);
	TEST_EXPECT(r.cfg.shadow_quality == 0);
	TEST_EXPECT(r.cfg.shader_usage_level == 77); // not clamped
	TEST_EXPECT(r.cfg.gamma == 2.0f);
	TEST_EXPECT(r.cfg.mp_max_players == 64);
	TEST_EXPECT(r.cfg.remote_admin_port == 70000 - 65536); // a 16-bit store
	TEST_EXPECT(r.cfg.max_kills == std::numeric_limits<int32_t>::max()); // strtol saturates
	TEST_EXPECT(r.cfg.max_score == std::numeric_limits<int32_t>::min());

	r = load_text(with_version("startdelay = -5\r\nnwisptype = 19\r\nlanmode = 4\r\ngamma = 0.1\r\nmpmaxplayers = \"0\"\r\n"));
	TEST_EXPECT(r.cfg.startdelay == 0);
	TEST_EXPECT(r.cfg.nwisptype == 0);
	TEST_EXPECT(r.cfg.lanmode == 4);
	TEST_EXPECT(r.cfg.gamma == 0.5f);
	TEST_EXPECT(r.cfg.mp_max_players == 1);

	r = load_text(with_version("hw3d_res1024x768 = 1\r\nhw3d_res800x600 = 2\r\nHW3D_RES2048X1536 = 1\r\n"));
	TEST_EXPECT(r.cfg.display_device_flags == (0x4u | 0x2000u)); // only a value of exactly 1 raises its bit
	TEST_EXPECT(write(r.cfg, {}).find("hw3d_res") == std::string::npos);
	return 0;
}

int version_fallback() {
	// A wrong version: everything back to the defaults, but player_index (and
	// the device number, outside the reset block) as read.
	LoadResult r = load_text("version = 28\r\nmax_kills = 7\r\nplayer_index = 3\r\nhw3d_deviceno = 1\r\n");
	TEST_EXPECT(r.file_read && r.version_reset);
	TEST_EXPECT(r.cfg.version == 29);
	TEST_EXPECT(r.cfg.max_kills == 50);
	TEST_EXPECT(r.cfg.player_index == 3);
	TEST_EXPECT(r.cfg.hw3d_deviceno == 1);
	TEST_EXPECT(r.cfg.water_quality == 1); // the clamp still runs

	// No version line at all: the -1 the load starts from fails the check.
	r = load_text("max_kills = 7\r\n");
	TEST_EXPECT(r.version_reset && r.cfg.max_kills == 50);

	// A missing file: the defaults, the prior player_index kept.
	LoadOptions options;
	options.player_index = 5;
	r = load(nullptr, 0, options);
	TEST_EXPECT(!r.file_read && r.version_reset && r.cfg.player_index == 5);
	return 0;
}

int reset_and_lan_switch() {
	LoadResult r = load_text("version = 28\r\nmpreset = \"1\"\r\n");
	TEST_EXPECT(r.reset_exit);
	TEST_EXPECT(!r.version_reset); // retail exits before the version check

	LoadOptions options;
	options.lan_switch = true;
	r = load_text(with_version("mpnovaworldhostlanonly = \"0\"\r\n"), options);
	TEST_EXPECT(r.cfg.mp_novaworld_host_lan_only == 1);
	return 0;
}

int string_bounds() {
	const std::string twenty = "ABCDEFGHIJKLMNOPQRST";
	const std::string two_hundred(200, 'm');
	LoadResult r = load_text(with_version(
			"join_password = \"" + twenty + "\"\r\n" +
			"mphostgamepassword = \"" + twenty + "\"\r\n" +
			"mphostspectatorpassword = \"" + two_hundred + "\"\r\n" +
			"servermsg = \"" + two_hundred + "\"\r\n" +
			"video_res = 1920x1080x32@60Hz\r\n" +
			"country = \"ABCDEFGHIJ\"\r\n" +
			"hw3d_name = \"" + two_hundred + "\"\r\n" +
			"game_name = \"\"\r\n"));
	TEST_EXPECT(r.cfg.join_password == twenty.substr(0, 16));          // strncpy 16
	TEST_EXPECT(r.cfg.mp_host_game_password == twenty.substr(0, 16));  // Napi_CopyString 17
	TEST_EXPECT(r.cfg.mp_host_spectator_password.size() == 63);        // Napi_CopyString 64
	TEST_EXPECT(r.cfg.servermsg.size() == 127);                        // Napi_CopyString 128
	TEST_EXPECT(r.cfg.video_res == "1920x1080x32@60H");                // strncpy 16
	TEST_EXPECT(r.cfg.country == "ABCDEFG");                           // field less one (D-GAMECFG-1)
	TEST_EXPECT(r.cfg.hw3d_name.size() == 31);                         // likewise
	TEST_EXPECT(r.cfg.game_name == "!Untitled");                       // an empty value is ignored
	return 0;
}

int weapon_availability() {
	LoadOptions options;
	options.weapons = { { "WPN_Alpha", 1 }, { "wpn_beta", 0 }, { "XYZ", 1 } };
	LoadResult r = load_text(with_version(
			"avail_wpn_ALPHA = 1\r\n"   // matched from the key's sixth character, case-insensitively
			"avail_wpn_beta = 2\r\n"    // an unselectable row still reads
			"avail_wpn_gamma = 0\r\n"   // no such row
			"avail_wpnalpha = 0\r\n"    // `wpnalpha` names no row
			"avail_class_sniper = 0\r\navail_class_engineer = 2\r\n"),
			options);
	TEST_EXPECT(r.cfg.weapon_availability[0] == 1);
	TEST_EXPECT(r.cfg.weapon_availability[1] == 2);
	TEST_EXPECT(r.cfg.weapon_availability[2] == 3);
	TEST_EXPECT(r.cfg.class_availability[kClassSniper] == 0);
	TEST_EXPECT(r.cfg.class_availability[kClassEngineer] == 2);
	const std::string text = write(r.cfg, options.weapons);
	TEST_EXPECT(text.find("avail_wpn_Alpha            = 1\r\n") != std::string::npos);
	TEST_EXPECT(text.find("avail_wpn_beta") == std::string::npos);  // not loadout_selectable
	TEST_EXPECT(text.find("avail_wpn_                 = 3\r\n") != std::string::npos); // a short name

	// The boot read runs before weapon.def: no roster, no avail_wpn line lands.
	r = load_text(with_version("avail_wpn_alpha = 1\r\n"));
	TEST_EXPECT(r.cfg.weapon_availability[0] == 3);

	// Rows past the 255 words alias the class words that follow them.
	WeaponRoster many(257);
	for (size_t i = 0; i < many.size(); ++i) many[i].name = "WPN_W" + std::to_string(i);
	options.weapons = many;
	r = load_text(with_version("avail_wpn_W256 = 0\r\n"), options);
	TEST_EXPECT(r.cfg.class_availability[1] == 0);

	// Past the class words, retail's unchecked store would leave the record
	// (D-GAMECFG-1): the port drops it, and nothing in the record changes.
	WeaponRoster past(266);
	for (size_t i = 0; i < past.size(); ++i) past[i].name = "WPN_W" + std::to_string(i);
	options.weapons = past;
	TEST_EXPECT(weapon_slot(r.cfg, 264) != nullptr);
	TEST_EXPECT(weapon_slot(r.cfg, 265) == nullptr);
	const GameCfg before = load_text(with_version(""), options).cfg;
	r = load_text(with_version("avail_wpn_W265 = 0\r\n"), options);
	TEST_EXPECT(std::equal(std::begin(r.cfg.class_availability), std::end(r.cfg.class_availability),
			std::begin(before.class_availability)));
	TEST_EXPECT(std::equal(std::begin(r.cfg.weapon_availability), std::end(r.cfg.weapon_availability),
			std::begin(before.weapon_availability)));
	return 0;
}

// fgets reads at most 1023 characters, so a longer line splits and its tail
// is read as a line of its own: a commented-out line whose tail is a setting
// applies that setting, as in retail.
int long_line_splits() {
	const std::string head = "//" + std::string(1021, 'x'); // exactly 1023 characters
	const LoadResult r = load_text(with_version(head + "max_kills = 7\r\n"));
	TEST_EXPECT(r.cfg.max_kills == 7);
	// One character shorter, the whole line is the comment.
	const LoadResult whole = load_text(with_version("//" + std::string(1020, 'x') + "max_kills = 7\r\n"));
	TEST_EXPECT(whole.cfg.max_kills == defaults().max_kills);
	return 0;
}

int duplicates_and_floats() {
	const LoadResult r = load_text(with_version(
			"max_kills = 5\r\nmax_kills = 6\r\n"
			"mpvoting_percent = 0.5\r\nbalance_join_percent = \"1e-1\"\r\n"));
	TEST_EXPECT(r.cfg.max_kills == 6);
	TEST_EXPECT(r.cfg.mpvoting_percent == 0.5f);
	const float tenth = 0.1f;
	TEST_EXPECT(r.cfg.balance_join_percent == tenth);
	const std::string text = write(r.cfg, {});
	TEST_EXPECT(text.find("mpvoting_percent        = 0.500000\r\n") != std::string::npos);
	TEST_EXPECT(text.find("balance_join_percent    = 0.100000\r\n") != std::string::npos);
	return 0;
}

int crt_fixed_print() {
	TEST_EXPECT(format_fixed(1.0, 1) == "1.0");
	TEST_EXPECT(format_fixed(0.25, 1) == "0.3");  // half up on the digit string
	TEST_EXPECT(format_fixed(1.25, 1) == "1.3");
	TEST_EXPECT(format_fixed(0.0078125, 6) == "0.007813");
	TEST_EXPECT(format_fixed(9.96, 1) == "10.0"); // the carry moves the point
	const float sixty_six = 0.66f;
	const float big = 1e20f;
	TEST_EXPECT(format_fixed(static_cast<double>(sixty_six), 6) == "0.660000");
	TEST_EXPECT(format_fixed(1e-9, 6) == "0.000000");
	TEST_EXPECT(format_fixed(6e-7, 6) == "0.000001");
	TEST_EXPECT(format_fixed(-0.0, 6) == "-0.000000");
	TEST_EXPECT(format_fixed(-2.5, 6) == "-2.500000");
	TEST_EXPECT(format_fixed(static_cast<double>(big), 6) == "100000002004087730000.000000"); // 17 digits
	const double inf = std::numeric_limits<double>::infinity();
	TEST_EXPECT(format_fixed(inf, 6) == "1.#INF00");
	TEST_EXPECT(format_fixed(-inf, 6) == "-1.#INF00");
	TEST_EXPECT(format_fixed(inf, 1) == "1.$");
	TEST_EXPECT(format_fixed(inf, 2) == "1.#J");
	TEST_EXPECT(format_fixed(std::numeric_limits<double>::quiet_NaN(), 6) == "1.#QNAN0");
	TEST_EXPECT(format_fixed(-std::numeric_limits<double>::quiet_NaN(), 6) == "-1.#IND00");
	return 0;
}

} // namespace

int main() {
	if (defaults_match_config_set_defaults() != 0) return 1;
	if (comments_and_quoting() != 0) return 1;
	if (value_is_token_two() != 0) return 1;
	if (unknown_and_dead_keys() != 0) return 1;
	if (case_folding_and_table_first() != 0) return 1;
	if (folds_and_clamps() != 0) return 1;
	if (version_fallback() != 0) return 1;
	if (reset_and_lan_switch() != 0) return 1;
	if (string_bounds() != 0) return 1;
	if (weapon_availability() != 0) return 1;
	if (long_line_splits() != 0) return 1;
	if (duplicates_and_floats() != 0) return 1;
	if (crt_fixed_print() != 0) return 1;
	std::printf("OK: gamecfg parse\n");
	return 0;
}
