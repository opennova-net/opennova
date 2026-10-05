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
// but four writes the same host-screen global as one MULTI_PLAYER_HOST control
// (the table in host_file.cpp), so the file is a text form of the host screen;
// `GameType`, `UseLineUpQueue` and `LineUpQueueSize` write the globals game.cfg
// keeps as `mp_gametype`, `mpuselineupqueue` and `mplineupqueuesize`; and
// `Mission <file> <launch option>` appends the named catalog mission to the map
// rotation. An unknown key, or a Mission naming no catalog row, changes nothing.
// [orig: Game_ParseCommandLineAndInit @0x4A7743..0x4A7775 (`/HOST`: the next
//  token to g_HostFileName @0xB4C5B4, g_HostFileSwitch @0xB4C5A8 = 1, a second instance
//  allowed); Game_HostMultiplayerSession @0x4A65A0 (no caller); the parse
//  @0x4A65A0..0x4A65AA; ServerConfig_ApplyHostSetting @0x4A6000]

#include <runtime/inmatch/game_config.h>
#include <runtime/mission/mission_catalog.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::io {
struct ConfigTokens;
}

namespace opennova::inmatch {

// The session rule baseline a fresh host installs before the host screen (or
// the host file) writes over it: Config_SetDefaults' rule values, the set the
// game's host screen seeds from (godot/game/world/host_session_config.gd
// installs the same game_rules constants). GameConfig's own defaults keep the
// rule fields zero so a bare test config stays inert.
// [orig: Config_SetDefaults @0x54D030 -- engine/base/gameprofile/game_type.h
//  game_rules carries each value's cite]
GameConfig host_screen_default_config();

// The host-screen block of g_GameConfigState the keys write: the dialog
// read's three outputs (apply_host_dialog_control) and the three globals the
// dialog never writes. The defaults are the cfg table's
// [orig: the MULTIPLAYER table @0x833050 -- mpmaxplayers "64" @0x833278,
//  mpuselineupqueue "1" @0x833290, mplineupqueuesize "100" @0x8332A8; the
//  rest of the block zeroed by Config_SetDefaults' memset @0x54D039..0x54D046,
//  mp_gametype among it].
struct HostScreenState {
	GameConfig config = host_screen_default_config();
	int32_t player_limit = 64;       // MAX_PLAYERS [orig: dword_2550AAC]
	bool serve_and_play = true;      // SERVERTYPE 0 [orig: dword_2550AB8]
	int32_t game_type_setting = 0;   // [orig: dword_2550AA8, game.cfg mp_gametype]
	int32_t use_lineup_queue = 1;    // [orig: dword_2550AB0]
	int32_t lineup_queue_size = 100; // [orig: dword_2550AB4]
};

// The map rotation the Mission lines seed (g_MissionRotation @0xC86FDC:
// (catalog index, flag) pairs with a cursor and an alternate cursor). The
// round-end advance over it is D-NET-331, unported; this is the seed.
// `launch_options` is the catalog entries' +0x1140 word, kept beside the
// catalog it indexes (0 for a row no line named).
struct MissionRotationEntry {
	size_t catalog_index = 0;
	uint32_t flag = 0;
};
struct MissionRotation {
	std::vector<MissionRotationEntry> entries;
	int32_t cursor = -1;
	int32_t alt_cursor = -1;
	std::vector<int32_t> launch_options;
	// What the last accepted Mission line stamped: g_MapFileName, the loose
	// word and the launch option the mission starts with, and the session code
	// word from its catalog row [orig: @0x4A6537..0x4A658D].
	std::string map_file;
	bool map_source_is_loose = false;
	uint8_t map_launch_option = 0;
	uint32_t map_game_type = 0;
	// The entry the cursor names, else null.
	const MissionRotationEntry *current() const;
};

// The result of one read: what the file changed and what it named that
// matched nothing (retail ignores both kinds silently; the embedder may log).
struct HostFileReport {
	size_t lines = 0;
	std::vector<std::string> unknown_keys;
	std::vector<std::string> unknown_missions;
};

// One tokenized line onto the host state and the rotation, retail's per-key
// arms in their order. False for a key no arm owns.
bool apply_host_file_line(const io::ConfigTokens &line, HostScreenState &host,
		MissionRotation &rotation, const std::vector<mission_catalog::Row> &catalog,
		HostFileReport *report = nullptr);

// The whole file through the game.cfg walk.
HostFileReport read_host_file(const char *text, size_t size, HostScreenState &host,
		MissionRotation &rotation, const std::vector<mission_catalog::Row> &catalog);

} // namespace opennova::inmatch
