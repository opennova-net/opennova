#include "jo.h"

#include <formats/gamecfg/game_cfg.h>

namespace opennova::games {

namespace {

// Every .def name Jointops.exe opens, each where it opens it (the name's every push into a loader;
// the binary holds no other .def name, and none it builds).
constexpr const char *kReadDefs[] = {
	"Avatars.def", // [orig: CAvatarDefs_Init @ 0x57B1D0]
	// [orig: Game_InitSubsystems @ 0x4A713C; Game_ReloadExpansionAndMods @ 0x5527D9;
	//  Expansion_ReloadAllAssets @ 0x56841B; Expansion_SwitchTo @ 0x5689DD]
	"SndProf.def",
	"ammo.def", // [orig: Game_StartMission @ 0x52548A]
	"charattr.def", // [orig: CharAttr_LoadFromDef @ 0x412172]
	"hudfx.def", // [orig: HUD_InitOverlaySystem @ 0x5A462E]
	"hudpos.def", // [orig: HUD_InitOverlaySystem @ 0x5A492C]
	// [orig: Game_InitSubsystems @ 0x4A719E; Game_ReloadExpansionAndMods @ 0x5527E8;
	//  Expansion_ReloadAllAssets @ 0x56842F; Expansion_SwitchTo @ 0x5689EC]
	"items.def",
	"overcast.def", // [orig: Environment_LoadTimeOfDayConfig @ 0x57DBFD]
	"powerup.def", // [orig: Game_StartMission @ 0x5256CD]
	"reverb.def", // [orig: Audio_LoadReverbDefs @ 0x766D8E]
	// [orig: WeaponDef_LoadAll @ 0x54DD36; Game_StartMission @ 0x5254B8;
	//  Game_ReloadExpansionAndMods @ 0x552845]
	"weapon.def",
	// hud.def's one push is in a loader nothing calls or points at, so JO never reads it [orig:
	// @ 0x5BE21E, the function @ 0x5BE210], as the second hudpos.def push [orig: @ 0x5A246B, the
	// function @ 0x5A2450].
	nullptr,
};

Title make() {
	Title t;
	t.code = "jo";
	t.display_name = "Joint Operations";
	// [orig: Game_ParseCommandLineAndInit @0x4A7310 (the title sprintf @0x4A7D30..0x4A7D3C),
	//  off_8154D8's literal, into g_TitleText]
	t.title_text = "Joint Operations: Typhoon Rising";
	// The program JO:CA's install ships beside its archives (the name the in-match identity's AP
	// carries, the running program's own: docs/titles/dfx2-vs-jo-re.md §6.1, DFX2-5).
	t.executable = "Jointops.exe";
	// The command file JO:CA's UPDATE.WIZ names (docs/update/updater-re.md §2). A CMD names no
	// territory placeholder, so the territory never changes it.
	t.command_file = "JOPTR.CMD";
	// Jointops.exe's cfg table (gamecfg::config_var_rows).
	t.config_table = gamecfg::ConfigTable::Jo;
	// DFX2-8: a session of at most 64, 65 with a dedicated host's slot: the config default's clamp
	// 1..64, the session apply's published cap and the session create's 65, the host leg's 64 / 65
	// and the slot table's 1..65; the host screen's cap word 64.
	// [orig: Config_SetDefaults @ 0x54d030 (@0x54d0ab..0x54d0b0); Game_ApplySessionSettingsToGlobals
	//  @ 0x551500 (@0x551b43..0x551b48); CNapiGameSession_ConnectOrHost @ 0x4d4f10
	//  (@0x4d5065..0x4d507b); CNapiGameSession_BuildAndCreateSession @ 0x5694d0
	//  (@0x569567..0x56956c); Server_AllocatePlayerSlotTable @ 0x51c180 (@0x51c195)]
	// [orig: g_HostScreenMaxPlayers @0x83C79C, read by HostDialog_ReadSettings @ 0x555940
	//  (@0x555c20..0x555c2d), UI_MapSliderIndexToMaxPlayers @ 0x555f00 and
	//  UI_PopulateHostSettingsFromConfig @ 0x555fe0 (@0x55631d, @0x55635a)]
	t.max_players = 64;
	t.max_players_host_screen = 64;
	// Jointops.exe mounts the expansion its command line names (`/exp` and `/mod` are one name, the
	// last one wins), its archives over the base game's [orig: Game_ParseCommandLineAndInit
	// @ 0x4a76ac (/exp, /mod), Expansion_LoadAssets @ 0x4a4730].
	t.expansions = true;
	t.read_defs = kReadDefs;
	return t;
}

} // namespace

const Title &jo_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
