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
// (game_config_block.h): its session creates take the mpreset word from it, a
// ServerCommand's config change lands in it, and its host screen shows the
// block and writes it back at START (host_settings.h), the host request seeded
// from the block (host_screen_from_block).

#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/host_file.h>
#include <runtime/inmatch/host_session.h> // HostConfig
#include <runtime/inmatch/server_admin_command.h>

#include <optional>
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

namespace opennova::playersav {
struct ProfileRecord;
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
// The same mapping over the block as it stands, its cap unclamped: the game
// shell's host request, which the session create clamps when it runs
// (session_player_cap), as retail's create reads the block its host screen
// wrote [orig: UI_HandleHostSessionStart @0x556E36 / @0x556E72 ->
// CNapiGameSession_BuildAndCreateSession @0x5694D0].
HostScreenState host_screen_from_block(const gamecfg::GameCfg &cfg);

// The session socket's bind scan over the cfg block's port quads, as the
// socket open picks them: a NovaWorld session, either side, the mpnovaworld
// quad and its random word; a LAN authority the mplanserver quad with random
// 0; a LAN non-authority the mplanclient quad and its random word. `first`, when
// set, is the scan's first port in place of the quad's min (a launch
// override). A set random word starts a scan of more than one port at
// min + delta * the PRNG's draw in 0..attempts - 1 (net_ports.h
// bind_scan_attempts / lan_host_bind_ports carry the scan's witness).
// [orig: CNapiNetwork_OpenTransportSocket @0x4C6A40 - the network type
//  @0x4C6A84..0x4C6A8D, the NovaWorld arm @0x4C6AF6..0x4C6B08, the LAN arm's
//  authority test @0x4C6AD4..0x4C6AD7 -> the server quad @0x4C6AA2..0x4C6AB4
//  (random 0), the client quad @0x4C6AD9..0x4C6AF0; NapiUdpSocket_CreateAndBind
//  @0x62D2A0]
std::vector<uint16_t> transport_bind_ports(const gamecfg::GameCfg &cfg, NetworkType type,
		bool authority, std::optional<uint32_t> first = std::nullopt);

// The cfg block's words every session create reads, single player's and a
// host's alike: mpreset (a nonzero word ends the create, server_session.h),
// the lineup queue the 0x42 gate admits past the cap and its size, and the
// join validator's duplicate-PCID word. host_session_settings copies them with
// the rest of the block; the game shell's in-process hosts take them alone.
// [orig: CNapiGameSession_BuildAndCreateSession -- game_settings.use_lineup_queue
//  @0x569632, lineup_queue_size @0x569646; the mpreset exit @0x4C97E7;
//  puntDuplicatePcids_330, read by Server_ValidatePlayerJoinRequest @0x512771]
void apply_session_create_words(GameConfig &config, const gamecfg::GameCfg &cfg);

// What the game shell's in-process host is configured from: a LAN host's
// host-screen session config with its cap, server type and network type, or
// (none) the single-player listen server's mission game type; the player's
// profile record and the process's cfg block when the shell has them.
struct ShellHostSource {
	const GameConfig *lan_config = nullptr; // the host screen's; null = single player
	uint32_t lan_max_players = 0;           // the cap session_player_cap left
	bool lan_serve_and_play = true;         // false = the dedicated server type
	NetworkType lan_network_type = NetworkType::Lan;
	uint32_t sp_game_type = 0; // the mission's (game_type::for_mission_attribs)
	const playersav::ProfileRecord *profile = nullptr;
	const gamecfg::GameCfg *cfg_block = nullptr;
};

// The game shell's in-process host record, the faithful §5.0 mode-3 listen
// server: a LAN host's from its host-screen config (its cap, its dedicated
// word from the server type, a real socket); else single player's, the
// profile record's session words (singleplayer_game_config), socketless and
// always serve-and-play. Either takes the cfg block's create words
// (apply_session_create_words) and the profile's inverse OPTIONS_AUTOMEDIC
// word. The embedder adds its own values (the title it speaks, the game
// root, its character selection).
// [orig: SinglePlayer_StartMission @0x561af0; the dedicated word the slot
//  limit adds the host's own slot for, Game_ApplySessionSettingsToGlobals
//  @0x551B20..0x551B35]
HostConfig shell_host_config(const ShellHostSource &source);

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
// A host's connection mode by its server type: a serve-and-play listen host
// is host and client (3), a Serve Only host host alone (1)
// [orig: CGameSession_SetConnectionMode @0x4c49f0; napi_np_server_ctx.h
//  ConnectionMode]. The mode a session-less context takes before the match
// binds (run_host_server_command).
ConnectionMode host_connection_mode(bool serve_and_play);

ServerCommandOutcome run_host_server_command(NapiNPServerCtx *ctx, world::World *world,
		ConnectionMode pre_session_mode, gamecfg::GameCfg *block, std::string_view verb,
		std::string_view target_suffix, const std::vector<std::string> &args);

} // namespace opennova::inmatch
