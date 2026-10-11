#pragma once

// The process's cfg block: the one game.cfg state a NovaLogic process keeps
// from its start to its exit, where every reader and writer of the player's
// and the host's settings meets [orig: g_GameConfigState @0x25506B8, the
// 2624-byte block]. The record is docs/gamecfg/game-cfg-re.md.
//
// THE READS. The block reads the working directory's game.cfg three ways,
// each one Game_LoadConfig over the defaults [orig: Game_LoadConfig @0x551480]:
//  - at the start, before weapon.def and gametext.bin load: no weapon table,
//    so every avail_wpn line drops, and the `!` default texts
//    [orig: Game_Run @0x4A7FBB];
//  - once the mount is up, with the weapon table and the gametext default
//    texts [orig: Game_InitSubsystems: gametext.bin @0x4A6FED, WeaponDef_LoadAll
//    @0x4A70A6, the read @0x4A70AB];
//  - at a menu expansion or mod reload, which saves first and reads through
//    the menu's init [orig: Game_ReloadExpansionAndMods @0x552710: Game_SaveConfig
//    @0x55272A, Menu_InitShellResources @0x552500 called @0x55281A, its read
//    @0x552534]. That call is Menu_InitShellResources' only one, so no other
//    menu entry reads the file. The weapon table is still the one the save
//    wrote with: the reload clears it after the read (@0x55281F..0x55282B).
// Every read keeps player_index and hw3d_deviceno across its defaults
// [orig: Config_SetDefaults @0x54D033, @0x54D0C4; hw3d_deviceno lives outside
// the block]. A read whose file sets mpreset ends the process with exit code 0
// before anything after it runs [orig: @0x5514A1..0x5514AC, crt_exit(0)]: load()
// reports it and the embedder exits, writing nothing.
//
// THE SAVES. Game_SaveConfig writes the whole block back [orig: @0x54C490];
// its call sites are the embedder's save points: the main loop's return
// (Game_Run @0x4A7FFF), the main menu's EXIT (@0x555776, beside the player
// profile's save @0x555771), the first launch's video test (@0x53EDB6), the
// reload (@0x55272A), the in-game options' Accept (@0x555310), a host start
// (HostDialog_StartSession @0x558769 after the host screen's read, the LAN
// screen's UI_HandleHostSessionStart @0x556E72), every multiplayer mission
// start (PreMenu_Init @0x5693E0, the save @0x5693F4), single player's mission
// start (SinglePlayer_StartMission @0x561BB2, ahead of its session create
// @0x561E65), the NovaWorld ServerCommand (@0x4D2DDF, @0x4D2E2D), the remote
// admin's SET (@0x405B6E, @0x4061AE) and the dead /HOST path (@0x4A6604). A
// process the mpreset word ends never reaches Game_Run's save.
//
// opennova-serve and the game shell each keep one block for the process.

#include <formats/gamecfg/game_cfg.h>

#include <string>

namespace opennova::inmatch {

class GameConfigBlock {
public:
	// `path` is the file every read opens and the save writes: game.cfg in
	// the process's working directory, as retail opens it by its bare name
	// [orig: the string @0x7D3E5C, read @0x551499 and written @0x54C4AA].
	explicit GameConfigBlock(std::string path = gamecfg::kFileName);

	const std::string &path() const { return path_; }
	void set_path(std::string path) { path_ = std::move(path); }

	gamecfg::GameCfg &cfg() { return cfg_; }
	const gamecfg::GameCfg &cfg() const { return cfg_; }

	// The weapon table the reads address the avail_wpn keys through and the
	// save writes them from: empty until weapon.def loads, as at the start.
	const gamecfg::WeaponRoster &weapon_roster() const { return roster_; }
	void set_weapon_roster(gamecfg::WeaponRoster roster) { roster_ = std::move(roster); }
	// The texts the defaults copy: the `!` fallbacks until gametext.bin loads.
	void set_default_texts(gamecfg::DefaultTexts texts) { texts_ = std::move(texts); }
	// The `/LAN` command-line switch every read applies [orig: Game_LoadConfig
	// @0x5514B1..0x5514BA].
	void set_lan_switch(bool on) { lan_switch_ = on; }

	// Game_LoadConfig over the file, keeping the block's player_index and
	// hw3d_deviceno. False when the file set mpreset: the process ends there
	// with exit code 0 and the caller writes nothing; the block then holds the
	// read as it stood at the exit.
	bool load();
	// Game_SaveConfig. False (with `error`) when the file does not open, where
	// retail writes nothing, silently.
	bool save(std::string &error) const;
	// The expansion or mod reload: the save with the weapon table as it
	// stands, then the read. False as load() is; `error` is the save's.
	bool reload(std::string &error);

	// The last read: whether the file opened, whether its version discarded
	// it for the defaults, and whether it set mpreset.
	bool file_read() const { return file_read_; }
	bool version_reset() const { return version_reset_; }
	bool reset_exit() const { return reset_exit_; }

private:
	std::string path_;
	gamecfg::GameCfg cfg_ = gamecfg::defaults();
	gamecfg::WeaponRoster roster_;
	gamecfg::DefaultTexts texts_;
	bool lan_switch_ = false;
	bool file_read_ = false;
	bool version_reset_ = false;
	bool reset_exit_ = false;
};

} // namespace opennova::inmatch
