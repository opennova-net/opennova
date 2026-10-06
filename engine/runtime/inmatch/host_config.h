#pragma once

// game.cfg onto the host session: the cfg block (gamecfg::GameCfg, the
// in-memory g_GameConfigState) is what a NovaLogic host reads its session
// from. The block loads from the working directory's game.cfg, the host
// screen or the host file write it, and the session takes it through one
// apply when the session is created and again at every mission start
// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 calls
//  Game_ApplySessionSettingsToGlobals @0x551500 at @0x5695B8;
//  Game_StartMission calls it at @0x524662]. opennova-serve runs the dead
// /HOST path over it (ADR 0051 d2); opennova.exe's hosts still seed their
// session from the host screen alone (D-NET-335's open half).

#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/host_file.h>

namespace opennova::def {
struct DefWeaponsFile;
}

namespace opennova::rtxt {
struct File;
}

namespace opennova::inmatch {

// The weapon table the avail_wpn keys address, from the parsed weapon.def:
// row 0 is the `None` row WeaponDef_LoadAll seeds, then every weapon.def entry
// an `end` closed, in file order. A row that ends while a parent's
// loadout_subclasses countdown runs is a subclass row: its properties past
// `startrounds` are skipped, so it is never selectable and starts no
// countdown of its own.
// [orig: WeaponDef_LoadAll @0x54DD10 -- the memset @0x54DD1C, the count 1
//  @0x54DD3B, "None" @0x54DD45, the weapon.def walk @0x54DD58;
//  WeaponDef_ParseProperty @0x54D730 -- the `weapon` row @0x54DCC4..0x54DD02,
//  the `end` count @0x54D788..0x54D7D3 (the countdown @0x54D79A..0x54D7BF),
//  the subclass skip @0x54D8F1, loadout_selectable @0x54D9F0,
//  loadout_subclasses @0x54DA1E]
gamecfg::WeaponRoster game_cfg_weapon_roster(const def::DefWeaponsFile &weapons);

// The defaults' two localized strings from the mounted gametext.bin: its
// Menu/UNTITLED and Menu/USERMESSAGE entries, each falling back to the `!`
// text when the table is not loaded or lacks the key.
// [orig: Config_SetDefaults @0x54D1A1, @0x54D240 through
//  GameText_GetStringWithFallback @0x51EB90]
gamecfg::DefaultTexts game_cfg_default_texts(const rtxt::File *gametext);

// The session a host creates from the cfg block, in session as the authority:
// CNapiGameSession_BuildAndCreateSession's in-session cap on mpmaxplayers
// (1..65, written back into the block, so the exit's save carries it), then
// Game_ApplySessionSettingsToGlobals' session arms and the NapiGameSettings
// copy into GameConfig, and the block words the session's handlers read
// directly. Two words are not mapped: the class allow mask (S2C 0x76), which
// the apply builds from the class availability words and the current catalog
// row's class word (+0x1118, not in mission_catalog::Row), and the armory's
// weapon availability table (WeaponDef_BuildItemRestrictionTable); both keep
// GameConfig's stock values. The session's game type is the starting map's
// (Game_StartMission), which the embedder sets from the rotation.
// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0;
//  Game_ApplySessionSettingsToGlobals @0x551500; per field in host_config.cpp]
HostScreenState host_session_settings(gamecfg::GameCfg &cfg);

} // namespace opennova::inmatch
