// The process's cfg block (runtime/inmatch/game_config_block.h) and its
// ServerCommand side (runtime/inmatch/host_config.h): the three reads over the
// working directory's game.cfg (the start's, the mount's, the reload's), the
// mpreset exit at a read, the save points' write, and a ServerCommand before
// and after the match binds, its config change landing in the block.
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/game_config_block.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h>

#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;

namespace {

void write_text(const std::string &path, const std::string &text) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << text;
}

std::string read_text(const std::string &path) {
	const std::vector<uint8_t> bytes = test_io::read_file(path);
	return std::string(bytes.begin(), bytes.end());
}

gamecfg::WeaponRoster rifle_roster() {
	return { { "None", 0 }, { "WPN_PISTOL", 1 }, { "WPN_RIFLE", 1 } };
}

} // namespace

int main() {
	// --- No file: the defaults, nothing read (the version check's -1 discards
	//     the empty read too); the save writes the whole block and a read of it
	//     takes the same words back.
	{
		test_temp::TempDir dir("game_config_block_fresh");
		GameConfigBlock block(dir.file("game.cfg"));
		TEST_EXPECT(block.load());
		TEST_EXPECT(!block.file_read() && block.version_reset() && !block.reset_exit());
		TEST_EXPECT(block.cfg().version == gamecfg::kConfigVersion);
		TEST_EXPECT(block.cfg().game_name == "!Untitled"); // the start's `!` default text
		block.cfg().sfx_level = 77;
		block.cfg().game_name = "Saved Host";
		std::string error;
		TEST_EXPECT(block.save(error));
		const std::string written = read_text(dir.file("game.cfg"));
		TEST_EXPECT(written.find("SFXLevel             = 77\r\n") != std::string::npos);
		TEST_EXPECT(
				written.find("game_name               = \"Saved Host\"\r\n") != std::string::npos);
		TEST_EXPECT(written.find("mpreset = \"0\"\r\n") != std::string::npos);

		GameConfigBlock again(dir.file("game.cfg"));
		TEST_EXPECT(again.load());
		TEST_EXPECT(again.file_read() && !again.version_reset());
		TEST_EXPECT(again.cfg().sfx_level == 77 && again.cfg().game_name == "Saved Host");
	}

	// --- The start's read has no weapon table, so every avail_wpn line drops;
	//     the mount's read takes them by row with the gametext default texts.
	//     Both keep player_index and hw3d_deviceno across their defaults.
	{
		test_temp::TempDir dir("game_config_block_mount");
		write_text(dir.file("game.cfg"),
				"version = 29\r\navail_wpn_rifle = 2\r\nplayer_index = 3\r\nhw3d_deviceno = 1\r\n");
		GameConfigBlock block(dir.file("game.cfg"));
		TEST_EXPECT(block.load());
		const gamecfg::GameCfg stock = gamecfg::defaults();
		TEST_EXPECT(*gamecfg::weapon_slot(block.cfg(), 2) == *gamecfg::weapon_slot(stock, 2));
		TEST_EXPECT(block.cfg().player_index == 3 && block.cfg().hw3d_deviceno == 1);
		block.set_weapon_roster(rifle_roster());
		gamecfg::DefaultTexts texts;
		texts.untitled = "Untitled";
		block.set_default_texts(texts);
		TEST_EXPECT(block.load());
		TEST_EXPECT(*gamecfg::weapon_slot(block.cfg(), 2) == 2);
		TEST_EXPECT(block.cfg().game_name == "Untitled");
		TEST_EXPECT(block.cfg().player_index == 3 && block.cfg().hw3d_deviceno == 1);
		// The save writes the selectable rows from the same table.
		std::string error;
		TEST_EXPECT(block.save(error));
		TEST_EXPECT(read_text(dir.file("game.cfg")).find("avail_wpn_RIFLE            = 2\r\n") !=
				std::string::npos);
	}

	// --- A version other than 29 discards the read for the defaults (the
	//     first launch's video test stands in on that, the embedder's).
	{
		test_temp::TempDir dir("game_config_block_version");
		write_text(dir.file("game.cfg"), "version = 28\r\nSFXLevel = 5\r\n");
		GameConfigBlock block(dir.file("game.cfg"));
		TEST_EXPECT(block.load());
		TEST_EXPECT(block.file_read() && block.version_reset());
		TEST_EXPECT(block.cfg().sfx_level == 192);
	}

	// --- A file that sets mpreset ends the process at the read, ahead of the
	//     version check: load() is false and writes nothing.
	{
		test_temp::TempDir dir("game_config_block_reset");
		const std::string text = "mpreset = 1\r\n";
		write_text(dir.file("game.cfg"), text);
		GameConfigBlock block(dir.file("game.cfg"));
		TEST_EXPECT(!block.load());
		TEST_EXPECT(block.reset_exit() && block.file_read());
		TEST_EXPECT(read_text(dir.file("game.cfg")) == text);
	}

	// --- The main menu's entries. The first runs the mount's read over its
	//     tables; with no file it runs the video test and saves at once, and
	//     the entry's own read then takes the saved words back.
	{
		test_temp::TempDir dir("game_config_block_menu_fresh");
		GameConfigBlock block(dir.file("game.cfg"));
		TEST_EXPECT(block.load()); // the start's read
		GameConfigBlock::MenuMount mount;
		mount.id = "game|";
		mount.weapons = rifle_roster();
		mount.texts.untitled = "Untitled";
		mount.desktop_width = 1920;
		mount.desktop_height = 1080;
		std::string error;
		TEST_EXPECT(block.enter_main_menu(mount, error));
		TEST_EXPECT(block.video_test_ran() && block.menu_mount() == "game|");
		TEST_EXPECT(block.cfg().display_16x9 == 1); // a widescreen desktop's row
		TEST_EXPECT(block.cfg().object_polydetail == 2 && block.cfg().texfilter_level == 1);
		TEST_EXPECT(block.cfg().particle_density == 2 && block.cfg().texcompression_level == 1);
		TEST_EXPECT(block.cfg().game_name == "Untitled");
		TEST_EXPECT(block.file_read() && !block.version_reset()); // the entry read the saved file
		const std::string saved = read_text(dir.file("game.cfg"));
		TEST_EXPECT(saved.find("display_16x9         = 1\r\n") != std::string::npos);

		// A later entry over the same mount reads the file again, and a set
		// mpreset ends the process there.
		write_text(dir.file("game.cfg"), "version = 29\r\nmusic_volume = 33\r\n");
		TEST_EXPECT(block.enter_main_menu(mount, error));
		TEST_EXPECT(block.cfg().music_volume == 33 && !block.video_test_ran());
		write_text(dir.file("game.cfg"), "version = 29\r\nmpreset = 1\r\n");
		TEST_EXPECT(!block.enter_main_menu(mount, error));
		TEST_EXPECT(block.reset_exit());
	}

	// --- A file of the right version keeps its words: no video test.
	{
		test_temp::TempDir dir("game_config_block_menu_kept");
		write_text(dir.file("game.cfg"), "version = 29\r\ndisplay_16x9 = 0\r\nobject_polydetail = 3\r\n");
		GameConfigBlock block(dir.file("game.cfg"));
		TEST_EXPECT(block.load());
		GameConfigBlock::MenuMount mount;
		mount.desktop_width = 1920;
		mount.desktop_height = 1080;
		std::string error;
		TEST_EXPECT(block.enter_main_menu(mount, error));
		TEST_EXPECT(!block.video_test_ran());
		TEST_EXPECT(block.cfg().display_16x9 == 0 && block.cfg().object_polydetail == 3);
	}

	// --- The reload (an entry over another mount, or the Mods list's switch)
	//     saves first with the table as it stands, then reads, then takes the
	//     new mount's table: a word set in memory (a SetMPReset) is written and
	//     ends the process at the read.
	{
		test_temp::TempDir dir("game_config_block_reload");
		GameConfigBlock block(dir.file("game.cfg"));
		TEST_EXPECT(block.load());
		GameConfigBlock::MenuMount first;
		first.id = "bundled|";
		first.weapons = rifle_roster();
		std::string error;
		TEST_EXPECT(block.enter_main_menu(first, error));
		*gamecfg::weapon_slot(block.cfg(), 2) = 2;
		block.cfg().music_volume = 12;
		GameConfigBlock::MenuMount retail;
		retail.id = "retail|";
		retail.weapons = {{"None", 0}, {"WPN_KNIFE", 1}};
		TEST_EXPECT(block.enter_main_menu(retail, error));
		TEST_EXPECT(block.menu_mount() == "retail|");
		TEST_EXPECT(block.cfg().music_volume == 12 && block.file_read());
		// The save before the read wrote the old table's rows.
		TEST_EXPECT(read_text(dir.file("game.cfg")).find("avail_wpn_RIFLE ") != std::string::npos);
		TEST_EXPECT(block.weapon_roster().size() == 2 && block.weapon_roster()[1].name == "WPN_KNIFE");
		block.cfg().mp_reset = 1;
		TEST_EXPECT(!block.reload_main_menu(retail, error));
		TEST_EXPECT(block.reset_exit());
		TEST_EXPECT(
				read_text(dir.file("game.cfg")).find("mpreset = \"1\"\r\n") != std::string::npos);
	}

	// --- The words by their keys, without case, as the reader finds them;
	//     a key the reader stores elsewhere names no int word.
	{
		gamecfg::GameCfg cfg = gamecfg::defaults();
		TEST_EXPECT(gamecfg::int_word(cfg, "SFXLevel") == &cfg.sfx_level);
		TEST_EXPECT(gamecfg::int_word(cfg, "sfxlevel") == &cfg.sfx_level);
		TEST_EXPECT(gamecfg::int_word(cfg, "MPRESET") == &cfg.mp_reset);
		TEST_EXPECT(gamecfg::int_word(cfg, "xhair_appearance") == &cfg.xhair_appearance);
		TEST_EXPECT(gamecfg::int_word(cfg, "game_name") == nullptr);
		TEST_EXPECT(gamecfg::int_word(cfg, "gamma") == nullptr);
		TEST_EXPECT(gamecfg::int_word(cfg, "remote_admin_port") == nullptr);
		TEST_EXPECT(gamecfg::int_word(cfg, "side_req") == nullptr); // a dead compare
		TEST_EXPECT(gamecfg::int_word(cfg, "no_such_key") == nullptr);
		const gamecfg::GameCfg &view = cfg;
		TEST_EXPECT(gamecfg::int_word(view, "hud_detail") == &cfg.hud_detail);
	}

	// --- The ServerCommand verbs' block writes: each verb its own field, any
	//     other verb nothing.
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		GameConfig config;
		config.server_name = "Renamed";
		config.custom_text = "Hello";
		config.multiplayer_reset = 1;
		store_server_command_config(block, "TextChatServer", config);
		TEST_EXPECT(block.game_name == "!Untitled" && block.mp_reset == 0);
		store_server_command_config(block, "setservername", config);
		TEST_EXPECT(block.game_name == "Renamed" && block.servermsg == "!Put your message here");
		store_server_command_config(block, "SetServerMsg", config);
		TEST_EXPECT(block.servermsg == "Hello" && block.mp_reset == 0);
		store_server_command_config(block, "SetMPReset", config);
		TEST_EXPECT(block.mp_reset == 1);
	}

	// --- Before the match binds (no context), a command runs on a session-less
	//     one: SetMPReset alone passes, writing the block; SetServerName and
	//     TextChatServer are refused and write nothing.
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		const ServerCommandOutcome name = run_host_server_command(nullptr, nullptr,
				ConnectionMode::HostClient, &block, "SetServerName", "", { "Renamed" });
		TEST_EXPECT(!name.handled && !name.config_changed && block.game_name == "!Untitled");
		const ServerCommandOutcome chat = run_host_server_command(nullptr, nullptr,
				ConnectionMode::HostClient, &block, "TextChatServer", "", { "hi" });
		TEST_EXPECT(!chat.handled);
		const ServerCommandOutcome reset = run_host_server_command(
				nullptr, nullptr, ConnectionMode::HostClient, &block, "SetMPReset", "", { "1" });
		TEST_EXPECT(reset.handled && reset.config_changed && block.mp_reset == 1);
		// The word is atol's: a nonzero value of either sign, and 0 clears it.
		(void)run_host_server_command(
				nullptr, nullptr, ConnectionMode::HostOnly, &block, "SetMPReset", "", { "-4" });
		TEST_EXPECT(block.mp_reset == -4);
		(void)run_host_server_command(
				nullptr, nullptr, ConnectionMode::HostOnly, &block, "SetMPReset", "", { "0" });
		TEST_EXPECT(block.mp_reset == 0);
		// No block: the command still runs, and nothing is stored.
		const ServerCommandOutcome bare = run_host_server_command(
				nullptr, nullptr, ConnectionMode::HostOnly, nullptr, "SetMPReset", "", { "1" });
		TEST_EXPECT(bare.handled && bare.config_changed);
	}

	// --- Once the match is bound, the command runs on the hosting context:
	//     SetServerName passes there and both homes take the name.
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		NapiNPServerCtx ctx;
		set_connection_mode(ctx, ConnectionMode::HostClient);
		ctx.is_in_session = 1;
		const ServerCommandOutcome name = run_host_server_command(&ctx, nullptr,
				ConnectionMode::HostClient, &block, "SetServerName", "", { "Renamed" });
		TEST_EXPECT(name.handled && name.config_changed);
		TEST_EXPECT(ctx.config.server_name == "Renamed" && block.game_name == "Renamed");
		const ServerCommandOutcome reset = run_host_server_command(
				&ctx, nullptr, ConnectionMode::HostClient, &block, "SetMPReset", "", { "1" });
		TEST_EXPECT(reset.handled && ctx.config.multiplayer_reset == 1 && block.mp_reset == 1);
	}

	std::printf("game_config_block: ok\n");
	return 0;
}
