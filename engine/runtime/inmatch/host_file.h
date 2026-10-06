#pragma once

// The retail host file: the text a NovaLogic dedicated server reads its host
// settings and its map rotation from. Jointops.exe still carries the whole
// reader -- `/HOST <file>` stores the file name, and
// Game_HostMultiplayerSession hands that file to the game.cfg walk with
// ServerConfig_ApplyHostSetting as the line callback -- but nothing calls
// Game_HostMultiplayerSession, so retail never reads one (ADR 0051).
// opennova-serve reads it as its config.
//
// A line is `<Key> <value>`: the game.cfg tokenizer splits on spaces, commas
// and tabs, so a value with spaces is quoted, and `//` or `;` starts a comment
// (io::for_each_config_file_line). Keys match case-insensitively. Every key
// but `Mission` writes one field of the game.cfg block (gamecfg::GameCfg, the
// table in host_file.cpp), the block the host screen also reads and writes,
// so the file is a text form of the host screen over the working directory's
// game.cfg; the session takes the block through the one apply
// (host_config.h host_session_settings). `Mission <file> <launch option>`
// appends the named catalog mission to the map rotation. An unknown key, or a
// Mission naming no catalog row, changes nothing.
// [orig: Game_ParseCommandLineAndInit @0x4A7743..0x4A7775 (`/HOST`: the next
//  token to g_HostFileName @0xB4C5B4, g_HostFileSwitch @0xB4C5A8 = 1, a second instance
//  allowed); Game_HostMultiplayerSession @0x4A65A0 (no caller); the parse
//  @0x4A65A0..0x4A65AA; ServerConfig_ApplyHostSetting @0x4A6000]

#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/mission/mission_catalog.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::io {
struct ConfigTokens;
}

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::inmatch {

// The session rule baseline the game's host screen installs before its read
// writes over it: Config_SetDefaults' rule values, the set the screen seeds
// from (godot/game/world/host_session_config.gd takes it from here). The
// game.cfg apply over the defaults block lands on the same values
// (host_session_settings; ctest npruntime_host_config holds them equal).
// GameConfig's own defaults keep the rule fields zero so a bare test config
// stays inert.
// [orig: Config_SetDefaults @0x54D030 -- engine/base/gameprofile/game_type.h
//  game_rules carries each value's cite]
GameConfig host_screen_default_config();

// The session a host runs from: the session config and the words of the cfg
// block the session reads beside it. The game's host screen fills it
// through its read (apply_host_dialog_control); opennova-serve maps the
// game.cfg block onto it (host_config.h host_session_settings). The defaults
// are the cfg table's [orig: the MULTIPLAYER table @0x833050 -- mpmaxplayers
// "64" @0x833278, mpuselineupqueue "1" @0x833290, mplineupqueuesize "100"
// @0x8332A8; the rest of the block zeroed by Config_SetDefaults' memset
// @0x54D039..0x54D046, mp_gametype among it].
struct HostScreenState {
	GameConfig config = host_screen_default_config();
	int32_t player_limit = 64;       // MAX_PLAYERS [orig: dword_2550AAC]
	bool serve_and_play = true;      // SERVERTYPE 0 [orig: dword_2550AB8]
	int32_t game_type_setting = 0;   // [orig: dword_2550AA8, game.cfg mp_gametype]
	int32_t use_lineup_queue = 1;    // [orig: dword_2550AB0]
	int32_t lineup_queue_size = 100; // [orig: dword_2550AB4]
};

// The result of one read: what the file changed and what it named that
// matched nothing (retail ignores both kinds silently; the embedder may log).
struct HostFileReport {
	size_t lines = 0;
	std::vector<std::string> unknown_keys;
	std::vector<std::string> unknown_missions;
};

// One tokenized line onto the cfg block and the rotation, retail's per-key
// arms in their order. False for a key no arm owns.
bool apply_host_file_line(const io::ConfigTokens &line, gamecfg::GameCfg &cfg,
		HostRotation &rotation, const std::vector<mission_catalog::Row> &catalog,
		HostFileReport *report = nullptr);

// The whole file through the game.cfg walk, over the block as it stands (the
// game.cfg the process read, else its defaults).
HostFileReport read_host_file(const char *text, size_t size, gamecfg::GameCfg &cfg,
		HostRotation &rotation, const std::vector<mission_catalog::Row> &catalog);

} // namespace opennova::inmatch
