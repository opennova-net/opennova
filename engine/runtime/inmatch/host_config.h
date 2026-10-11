#pragma once

// game.cfg onto the host session: the cfg block (gamecfg::GameCfg, the
// in-memory g_GameConfigState) is what a NovaLogic host reads its session
// from. The block loads from the working directory's game.cfg, the host
// screen or the host file write it, and the session takes it through one
// apply when the session is created and again at every mission start
// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 calls
//  Game_ApplySessionSettingsToGlobals @0x551500 at @0x5695B8;
//  Game_StartMission calls it at @0x524662]. opennova-serve runs the dead
// /HOST path over it (ADR 0051 d2). The game shell keeps the block too
// (game_config_block.h): its session creates take the mpreset word from it and
// a ServerCommand's config change lands in it, but its hosts still seed their
// session from the host screen, which neither reads nor writes the block
// (D-NET-335's open half).

#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/host_file.h>
#include <runtime/inmatch/server_admin_command.h>

#include <string>
#include <string_view>
#include <vector>

namespace opennova::def {
struct DefWeaponsFile;
}

namespace opennova::rtxt {
struct File;
}

namespace opennova {
class ResourceIndex;
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
// The same table from the mounted install's weapon.def, read through the
// index (empty when the mount has none or it does not parse).
gamecfg::WeaponRoster game_cfg_weapon_roster(const ResourceIndex &index);

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

// The cfg block's side of a ServerCommand that changed the config: the verb's
// block field takes the value the executor left in the session config
// (SetServerName game_name +0x3A5, SetServerMsg servermsg +0x560, SetMPReset
// mpreset +0x344), which the caller then saves; SetMPReset touches only the
// block. Any other verb writes nothing.
// [orig: CNapiGameSession_HandleServerCommand — SetServerName's block copy
//  @0x4D2CFC, SetServerMsg's @0x4D2DAD, SetMPReset's @0x4D2E28; Game_SaveConfig
//  @0x4D2DDF and @0x4D2E2D]
void store_server_command_config(
		gamecfg::GameCfg &block, std::string_view verb, const GameConfig &config);

// A ServerCommand at a host, over its cfg block. Once the host's match is
// bound, the command runs on its hosting context `ctx` and its `world`; before
// that (the hosting wait, from the hosting request until the session create:
// `ctx` null) it runs on a session-less context, because retail's statement
// dispatch hands every ServerCommand to the handler whatever the session's
// state. That context holds `pre_session_mode` (the host's connection mode)
// and a clear is_in_session (the executor's stand-in for ctx+0x68, which only
// CNapiGameSession_CreateSession sets), so every verb's gate refuses it but
// SetMPReset's token count. A config change lands in `block` (null: none) by
// store_server_command_config; the caller saves it (Game_SaveConfig) and
// republishes the changed columns.
// [orig: CNapiGameSession_DispatchServerStatement @0x4D18A0 (no state test);
//  CNapiGameSession_HandleServerCommand @0x4D22F0 — SetMPReset's lone token
//  count @0x4D2E12, the other verbs' ctx+0x68 tests (SetServerName's
//  @0x4D2CE9); the latch CNapiGameSession_CreateSession sets @0x4C9D16]
ServerCommandOutcome run_host_server_command(NapiNPServerCtx *ctx, world::World *world,
		ConnectionMode pre_session_mode, gamecfg::GameCfg *block, std::string_view verb,
		std::string_view target_suffix, const std::vector<std::string> &args);

} // namespace opennova::inmatch
