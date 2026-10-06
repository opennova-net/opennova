// game.cfg roundtrip: the minted fixtures/gamecfg/synth_game.cfg (written by
// tests/fixtures/minimal_gamecfg_gen.cpp through gamecfg::write) loads through
// Game_LoadConfig's port and writes back byte for byte, in memory and through
// load_file / save_file; the defaults write as Game_SaveConfig writes a fresh
// state; and the activesrvr.txt marker is the dead host path's one line.
#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include "gamecfg/gamecfg_synth.h"

#include <formats/gamecfg/game_cfg.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

using namespace opennova::gamecfg;

namespace {

bool starts_with(const std::string &text, const std::string &prefix) {
	return text.compare(0, prefix.size(), prefix) == 0;
}

bool contains(const std::string &text, const std::string &needle) {
	return text.find(needle) != std::string::npos;
}

int fixture_roundtrip(const std::string &fixture) {
	std::string committed;
	TEST_EXPECT(test_io::read_file_text(fixture, committed));
	TEST_EXPECT(!committed.empty());
	// CR LF throughout (the "w" stream's text mode), no bare LF.
	for (size_t i = 0; i < committed.size(); ++i)
		if (committed[i] == '\n') TEST_EXPECT(i > 0 && committed[i - 1] == '\r');

	LoadOptions options;
	options.weapons = gamecfg_synth::roster();
	const LoadResult loaded = load(committed.data(), committed.size(), options);
	TEST_EXPECT(loaded.file_read);
	TEST_EXPECT(!loaded.version_reset);
	TEST_EXPECT(!loaded.reset_exit);
	TEST_EXPECT(write(loaded.cfg, options.weapons) == committed);

	// The pinned values the generator authored.
	const GameCfg &c = loaded.cfg;
	TEST_EXPECT(c.version == kConfigVersion);
	TEST_EXPECT(c.hw3d_deviceno == 1);
	TEST_EXPECT(c.hw3d_name == "Synthetic Adapter 3000");
	TEST_EXPECT(c.gamma == 1.5f);
	TEST_EXPECT(c.mp_ip_address_string == "192.168.7.20");
	TEST_EXPECT(c.mp_access_code_list == "alpha, bravo");
	TEST_EXPECT(c.mp_max_packet_size == 1200);
	TEST_EXPECT(c.mp_tod_continuity == 1);
	TEST_EXPECT(c.servermsg == "Welcome to the synthetic server, play fair");
	TEST_EXPECT(c.mpvoting_percent == 0.75f);
	TEST_EXPECT(c.balance_join_percent == 0.25f);
	TEST_EXPECT(c.remote_admin_port == 4711);
	TEST_EXPECT(c.class_availability[kClassRifleman] == 2);
	TEST_EXPECT(c.weapon_availability[0] == 1 && c.weapon_availability[1] == 2);
	TEST_EXPECT(c.weapon_availability[2] == 3); // the unselectable row has no line
	TEST_EXPECT(c.weapon_availability[3] == 0);
	TEST_EXPECT(c.player_index == 2);
	TEST_EXPECT(c.country == "NZ");
	// The write-only keys are in the text and at their defaults in the model.
	TEST_EXPECT(contains(committed, "flagreturntime          = 210\r\n"));
	TEST_EXPECT(contains(committed, "side_req                = -1\r\n"));
	TEST_EXPECT(c.flag_return_time == 210 && c.side_req == -1 && c.side_password.empty());
	TEST_EXPECT(contains(committed, "avail_wpn_SynPistol        = 1\r\n"));
	TEST_EXPECT(contains(committed, "avail_wpn_SynLauncherLong  = 0\r\n"));
	TEST_EXPECT(!contains(committed, "SynHidden"));

	// Through the files.
	const std::filesystem::path out =
			std::filesystem::temp_directory_path() / test_paths_unique("opennova_gamecfg_roundtrip", ".cfg");
	std::string error;
	TEST_EXPECT(save_file(out.string(), loaded.cfg, options.weapons, error));
	std::string saved;
	TEST_EXPECT(test_io::read_file_text(out.string(), saved));
	TEST_EXPECT(saved == committed);
	const LoadResult reloaded = load_file(out.string(), options);
	TEST_EXPECT(reloaded.file_read);
	TEST_EXPECT(write(reloaded.cfg, options.weapons) == committed);
	std::error_code ec;
	std::filesystem::remove(out, ec);

	// A missing file is the defaults, silently.
	const LoadResult missing = load_file(out.string(), options);
	TEST_EXPECT(!missing.file_read);
	TEST_EXPECT(missing.version_reset);
	TEST_EXPECT(missing.cfg.version == kConfigVersion);
	return 0;
}

int defaults_write() {
	// A fresh state's write, against the bytes Game_SaveConfig's literals give.
	GameCfg cfg = defaults();
	clamp_graphics_options(cfg);
	const std::string text = write(cfg, {});
	TEST_EXPECT(starts_with(text,
			"// game.cfg\r\n//\r\n\r\n// GENERAL\r\nversion           = 29\r\n\r\n// DISPLAY\r\n"
			"windowed             = 0\r\nhw3d_deviceno        = 0\r\nhw3d_name            = \"\"\r\n"
			"hw3d_guid            = \"\"\r\nvideo_res            = \r\ngamma                = 1.0\r\n"));
	TEST_EXPECT(contains(text, "water_quality        = 1\r\n")); // the clamp's floor
	TEST_EXPECT(contains(text, "// CONTROLS\r\n"));
	TEST_EXPECT(contains(text, "hud_detail           = 0\r\n\r\n// MULTIPLAYER\r\n"
			"mpipaddressstring = \"0.0.0.0\"\r\nmpgateserverlocalportmin = \"49152\"\r\n"));
	TEST_EXPECT(contains(text, "mpmaxplayers = \"64\"\r\n"));
	TEST_EXPECT(contains(text, "mptodcontinuity = \"0\"\r\n")); // Config_SetDefaults overrides the row
	TEST_EXPECT(contains(text, "mpreset = \"0\"\r\nmp_eula_accepted        = -1\r\n"));
	TEST_EXPECT(contains(text, "game_name               = \"!Untitled\"\r\n"));
	TEST_EXPECT(contains(text, "servermsg               = \"!Put your message here\"\r\n"));
	TEST_EXPECT(contains(text, "mpvoting_percent        = 0.660000\r\n"));
	TEST_EXPECT(contains(text, "xhair_color             = 16777215\r\n"));
	TEST_EXPECT(contains(text, "balance_join_percent    = 0.500000\r\n"));
	TEST_EXPECT(contains(text, "\r\n// MULTIPLAYER WEAPON AVAILABILITY (0=NEVER, 1=ALWAYS, 2=ARMORY ONLY, "
			"3=MISSION DEFAULT)\r\n\r\n// MAP\r\n"));
	const std::string tail = "AltLock                 = 1\r\nAltLockType             = 1\r\n\r\n";
	TEST_EXPECT(text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0);
	return 0;
}

int active_server_marker() {
	const std::filesystem::path marker =
			std::filesystem::temp_directory_path() / test_paths_unique("opennova_activesrvr", ".txt");
	TEST_EXPECT(write_active_server_marker(marker.string()));
	std::string text;
	TEST_EXPECT(test_io::read_file_text(marker.string(), text));
	TEST_EXPECT(text == "\nThis directory has a server running in it that did not exit cleanly or is running\n");
	TEST_EXPECT(write_active_server_marker(marker.string())); // replaces, never appends
	TEST_EXPECT(test_io::read_file_text(marker.string(), text));
	TEST_EXPECT(text == kActiveServerMarkerText);
	remove_active_server_marker(marker.string());
	TEST_EXPECT(!std::filesystem::exists(marker));
	remove_active_server_marker(marker.string()); // a missing file is not an error
	return 0;
}

} // namespace

int main() {
	const std::string fixture = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/gamecfg/synth_game.cfg";
	if (fixture_roundtrip(fixture) != 0) return 1;
	if (defaults_write() != 0) return 1;
	if (active_server_marker() != 0) return 1;
	std::printf("OK: gamecfg roundtrip\n");
	return 0;
}
