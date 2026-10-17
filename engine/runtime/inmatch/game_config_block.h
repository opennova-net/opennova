#pragma once

// The process's cfg block: the one game.cfg state a NovaLogic process keeps
// from its start to its exit, where every reader and writer of the player's
// and the host's settings meets [orig: g_GameConfigState @0x25506B8, the
// 2624-byte block]. The record is docs/gamecfg/game-cfg-re.md.
//
// THE READS. Each read is Game_LoadConfig over the defaults
// [orig: Game_LoadConfig @0x551480]:
//  - at the start, before weapon.def and gametext.bin load: no weapon table,
//    so every avail_wpn line drops, and the `!` default texts
//    [orig: Game_Run @0x4A7FBB];
//  - once the mount is up, with the weapon table and the gametext default
//    texts [orig: Game_InitSubsystems: gametext.bin @0x4A6FED, WeaponDef_LoadAll
//    @0x4A70A6, the read @0x4A70AB], followed on a fresh profile by the first
//    launch's video test and its save (D-GAMECFG-3);
//  - at every main-menu entry: the MainMenu scene's init reads it
//    [orig: Menu_InitShellResources @0x552500, the read @0x552534, the init slot
//    of the MainMenu scene descriptor 0x83B400 (+0x20); the scene loop
//    @0x52B630 calls a scene's init @0x52B70D at every change of scene
//    (@0x52BB01..0x52BB13, the pending scene the push @0x52BB30 stores), and
//    MainMenu is pushed at the boot @0x4A7FE8, after a mission @0x5686FE and
//    from the PreMenu @0x56A3E8];
//  - at a menu expansion or mod reload, which saves first and reads through
//    the same init [orig: Game_ReloadExpansionAndMods @0x552710: Game_SaveConfig
//    @0x55272A, Menu_InitShellResources called @0x55281A]. The weapon table is
//    still the one the save wrote with: the reload clears it after the read
//    (@0x55281F..0x55282B).
// A join's expansion switch reads nothing: it hands the block the new weapon
// table (switch_expansion), and the main-menu entry after the match is a
// plain read.
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
// reload (@0x55272A), the in-game options' Accept (@0x555310), the LAN host
// screen's start (UI_HandleHostSessionStart @0x556E72, after its
// HostDialog_ReadSettings @0x556E36), the NovaWorld page's launch, a host's
// or a join's (HostDialog_StartSession @0x558769, after its read @0x558764;
// its one caller, screen event 7 @0x54F321, saves the profile first
// @0x54F317), every multiplayer mission
// start (PreMenu_Init @0x5693E0, the save @0x5693F4), single player's mission
// start (SinglePlayer_StartMission @0x561BB2, ahead of its session create
// @0x561E65), the NovaWorld ServerCommand (@0x4D2DDF, @0x4D2E2D), the remote
// admin's SET (@0x405B6E, @0x4061AE) and the dead /HOST path (@0x4A6604). A
// process the mpreset word ends never reaches Game_Run's save.
//
// opennova-serve and the game shell each keep one block for the process.

#include <formats/gamecfg/game_cfg.h>

#include <cstdint>
#include <string>

namespace opennova::inmatch {

// The first launch's video test, its words written into `cfg`: the aspect
// word from the primary desktop's ratio and the quality words the test writes
// on a capable device (renderer/aspect_ratio.h, object_lod.h, texture_filter.h,
// particle_density.h, texture_compression.h, device_texture.h's terrain texture
// detail carry each word's witness). Its other words (the terrain polygon,
// shadow, water and antialias rungs, the gamma) have no consumer here and keep
// the defaults' values.
// [orig: Game_RunVideoTestDialog @0x53EC10 from Game_InitSubsystems @0x4A7125]
void run_video_test(gamecfg::GameCfg &cfg, int32_t desktop_width, int32_t desktop_height);

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

	// Game_LoadConfig over the file, keeping the block's player_index and
	// hw3d_deviceno. False when the file set mpreset: the process ends there
	// with exit code 0 and the caller writes nothing; the block then holds the
	// read as it stood at the exit.
	bool load();
	// The `/LAN` switch: every read after it forces mpnovaworldhostlanonly to
	// 1 past the mpreset exit, which a version reset's defaults then undo
	// (gamecfg::LoadOptions::lan_switch). The process's command line sets it
	// before the start's read and every read takes it [orig: g_LanSwitch, set
	// by Game_ParseCommandLineAndInit @0x4A74EB, read by every Game_LoadConfig
	// @0x5514B1].
	void set_lan_switch(bool on) { lan_switch_ = on; }
	// The title's cfg table (gamecfg::config_var_rows): every read takes it, and the block as it
	// stands takes it at once (its defaults, the table's).
	void set_config_table(gamecfg::ConfigTable table) {
		table_ = table;
		if (cfg_.table != table)
			cfg_ = gamecfg::defaults(texts_, cfg_.player_index, cfg_.hw3d_deviceno, table);
	}
	gamecfg::ConfigTable config_table() const { return table_; }
	bool lan_switch() const { return lan_switch_; }
	// Game_SaveConfig. False (with `error`) when the file does not open, where
	// retail writes nothing, silently.
	bool save(std::string &error) const;

	// A mounted game as the main menu sees it: its identity (an embedder's
	// name for the root and its expansion), its weapon.def table, its gametext
	// default texts, and the primary desktop the video test samples.
	struct MenuMount {
		std::string id;
		gamecfg::WeaponRoster weapons;
		gamecfg::DefaultTexts texts;
		int32_t desktop_width = 0;
		int32_t desktop_height = 0;
	};
	// A main-menu entry's reads. The process's first entry runs the mount's
	// read first (the subsystems' read, over `mount`'s tables) and, when that
	// read found no file or discarded it for its version, the video test and
	// its save (D-GAMECFG-3: retail's trigger is the cfg's adapter name or GUID
	// against the device's, which a missing file never matches); an entry over
	// a mount other than the block's (the last entry's, reload's or join
	// switch's) is the reload (reload_main_menu); every entry reads the file.
	// False when a read set mpreset: the process exits with code 0 there.
	// `error` is any save's.
	bool enter_main_menu(const MenuMount &mount, std::string &error);
	// The expansion or mod reload: the save with the weapon table as it
	// stands, the menu's read, then `mount`'s tables for the reads and saves
	// after. False as load() is.
	bool reload_main_menu(const MenuMount &mount, std::string &error);
	// A join's switch to the host's expansion: the new mount's weapon table
	// for the reads and saves after, with no read and no save, so the next
	// main-menu entry over that mount is a plain read. The switch rebuilds the
	// weapon table and reloads the profile, and it neither reads game.cfg nor
	// raises the reload flag MainMenu's update tests, which only the front-end
	// Options ACCEPT sets. The default texts stay: gametext.bin loads once.
	// [orig: Expansion_SwitchTo @0x5688C0: the profile save @0x56891A,
	//  WeaponDef_LoadAll @0x5689AF, the profile load @0x5689B4; the reload flag
	//  dword_252DD90 written only @0x55AD4F, @0x55AD5B and @0x55291D;
	//  gametext.bin's one store @0x4A6FED]
	void switch_expansion(std::string id, gamecfg::WeaponRoster weapons);

	// The last read: whether the file opened, whether its version discarded
	// it for the defaults, and whether it set mpreset.
	bool file_read() const { return file_read_; }
	bool version_reset() const { return version_reset_; }
	bool reset_exit() const { return reset_exit_; }
	// The first entry ran the video test (a fresh profile).
	bool video_test_ran() const { return video_test_ran_; }
	// The mount the block's tables are (the last main-menu entry's, reload's
	// or join switch's; "" before the first).
	const std::string &menu_mount() const { return menu_mount_; }

private:
	std::string path_;
	gamecfg::GameCfg cfg_ = gamecfg::defaults();
	gamecfg::WeaponRoster roster_;
	gamecfg::DefaultTexts texts_;
	std::string menu_mount_;
	bool menu_entered_ = false;
	bool file_read_ = false;
	bool version_reset_ = false;
	bool reset_exit_ = false;
	bool video_test_ran_ = false;
	bool lan_switch_ = false;
	gamecfg::ConfigTable table_ = gamecfg::ConfigTable::Jo;
};

} // namespace opennova::inmatch
