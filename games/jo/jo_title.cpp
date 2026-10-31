#include "jo.h"

#include <formats/gamecfg/game_cfg.h>
#include <formats/playersav/weapon_sav.h> // kJointOpsSinglePlayerKit, kJointOpsClassKits

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

// Jointops.exe's HUD label fonts: Arial12b / Arial14n under 641 pixels, Arial14b / 14n under 801,
// Arial16b / 16n above, over 640 / 800 / 1024; Impac22b and Impac38b over 800.
// [orig: HUD_InitAllFonts @0x51EE20 - the widths @0x51EE7E / @0x51EEAB, the names
//  @0x51EE8A..0x51EF0E, the scale @0x51EF26, Impac22b @0x51EF48 and Impac38b @0x51EF7B over 800
//  @0x51EF62, the divide's magic 0x51EB851F @0x51EF53..0x51EF62 shifting by 8 (`sar edx, 8`
//  @0x51EF5A)]
constexpr TitleHudFontTier kHudFontTiers[] = {
	{640, "Arial14n.fnt", "Arial12b.fnt", 640},
	{800, "Arial14n.fnt", "Arial14b.fnt", 800},
	{0, "Arial16n.fnt", "Arial16b.fnt", 1024},
};
constexpr TitleHudFonts kHudFonts{kHudFontTiers, "Impac22b.fnt", "Impac38b.fnt", 800};

// Jointops.exe's F12 map legend: its 26 records, 40 design units a row (DFX2-89).
// [orig: HUD_DrawHelpScreenIcons @0x497480 - the records from aHudIconPerson @0x8154F8 (44 bytes:
//  name, cell +0x20, size +0x24, colour +0x28) to the empty name @0x815970, the row step
//  @0x497524]
constexpr TitleHudMapLegendIcon kHudMapLegendIcons[] = {
	{"hud_icon_person", 3, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_medic", 0, 3.0f, 0xFFFFFFFFu},
	{"hud_icon_persondead", 8, 1.0f, 0x7F2F3F7Fu},
	{"hud_icon_personmedicable", 14, 1.0f, 0x7F2F3F7Fu},
	{"hud_icon_waypoint", 1, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_psp", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_lfp_attack", 26, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_lfp_defend", 27, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_emplaced", 4, 0.8f, 0x7F7F6200u},
	{"hud_icon_emplaced2", 12, 0.8f, 0x7F7F6200u},
	{"hud_icon_vehicle", 10, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_watervehicle", 15, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_helo", 11, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_motorcycle", 25, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_farp", 5, 1.0f, 0x7F7F6200u},
	{"hud_icon_armoury", 13, 0.8f, 0x7F207F20u},
	{"hud_icon_bldg", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_bridge", 9, 1.0f, 0x7F207F20u},
	{"hud_icon_ridewanted", 23, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_enemyloc", 24, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_recentspeaker", 28, 0.8f, 0x7F7F7F7Fu},
	{" ", 0, 0.0f, 0x00000000u},
	{"hud_icon_flag", 2, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_flagbay", 6, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrmed", 16, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrammo", 17, 1.0f, 0x7F7F7F7Fu},
};
constexpr TitleHudMapLegend kHudMapLegend{
		kHudMapLegendIcons, static_cast<int>(sizeof(kHudMapLegendIcons) / sizeof(kHudMapLegendIcons[0])),
		40};

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
	// [orig: HostDialog_PopulateWeaponRestrictionList @0x556600 (@0x556611);
	//  HostDialog_PopulateClassListCombo @0x556700 (@0x556712); UI_RegisterHostSettingsCallbacks
	//  @0x5580f0 (each restriction control's screen)]
	t.host_screen = "MULTI_PLAYER_HOST";
	// Jointops.exe mounts the expansion its command line names (`/exp` and `/mod` are one name, the
	// last one wins), its archives over the base game's [orig: Game_ParseCommandLineAndInit
	// @ 0x4a76ac (/exp, /mod), Expansion_LoadAssets @ 0x4a4730].
	t.expansions = true;
	t.read_defs = kReadDefs;
	t.hud_fonts = &kHudFonts;
	// The catalog sorted by file name [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 - the
	//  qsort @ 0x5635f0 over Mission_CompareMapNames @ 0x5628e0].
	t.mission_list_order = MissionListOrder::FileName;
	// The profile files' magic, "FPBC" [orig: PlayerProfile_SaveToFiles @ 0x54be00 - the header
	//  @ 0x54be2e; PlayerProfile_LoadAllFromDisk @ 0x54f4d0 - edi = 0x43425046 @ 0x54f590, the
	//  player.sav gate @ 0x54f5b4, the weapon.sav gate @ 0x54f6ef].
	t.profile_magic = 0x43425046u;
	// The `/maxplayers N` switch [orig: Game_ParseCommandLineAndInit @ 0x4a7310 - "/maxplayers"
	//  @ 0x4a76ef..0x4a76ff, j__atol into g_MaxPlayerCommandLineArg @ 0x4a7710;
	//  ServerConfig_ApplyHostSetting @ 0x4a6000 - the MaxPlayers line's override
	//  @ 0x4a6233..0x4a6243].
	t.max_players_switch = true;
	// A fresh profile's kit pages, the eleven static pages the format holds with their witness
	// [orig: PlayerProfile_InitDefaults @ 0x54bb40 - @ 0x54bced, @ 0x54bd1a..0x54bdd1].
	t.profile_single_player_kit = playersav::kJointOpsSinglePlayerKit;
	t.profile_class_kits = playersav::kJointOpsClassKits;
	// DFX2-61: the Mods list's base row [orig: Options_PopulateModList @ 0x559fb0 (the literal
	//  @0x7C7114, read @ 0x559fe2)].
	t.mod_list_base = "Joint Operations: Typhoon Rising";
	t.hud_map_legend = &kHudMapLegend;
	// DFX2-43: the cube's eye at least 10 units over the terrain [orig: EnvCube_Update @0x6106A0 -
	//  `add eax, 0A0000h` @0x61072B over Terrain_GetHeightAtPosition, the raise @0x610739..0x61073B].
	t.env_cube_terrain_clearance = 0xA0000;
	return t;
}

} // namespace

const Title &jo_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
