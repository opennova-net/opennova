#include "dfx2.h"

#include <formats/gamecfg/game_cfg.h>

namespace opennova::games {

namespace {

// The files DFX2's stock data never ships, which its program still reads unchecked: its archives
// hold no cmap.mnu and no vehicle.mnu (resource.pff, localres.pff, language.pff and med.pff of
// the Steam install, read 2026-10-26), and dfx2.exe loads them where JO does
// [orig: Game_StartMission @0x521530 (@0x5234a5, @0x5234c1) and Input_HandleActionBinding
// @0x49ad50 (@0x49b988) for cmap.mnu; Input_HandleActionBinding @0x49ad50 (@0x49b8ff) and
// @0x4dd4c0 (@0x4ddbf3) for vehicle.mnu (dfx2.exe)]: the commander key and a vehicle's loadout
// open nothing, as the game ships. Nor does it ship nw_cdata.coo, the menu browser's cookie jar
// (in no archive, nor beside dfx2.exe in the Steam install), which it loads where JO does, its
// HRESULT ignored, and writes itself [orig: Menu_InitShellResources @0x54f750 (@0x54f887),
// UI_SaveCookieFileIfDirty @0x554d40 (dfx2.exe)].
constexpr const char *kAbsentResourceRoles[] = { "cmap_menu", "vehicle_menu", "nw_cdata", nullptr };

// dfx2.exe's HUD label fonts: Joint Operations' three tiers, then Arial20b / 20n to 1280 pixels,
// Arial28b / 28n to 1600 and Arial32b / 32n above, over 1280 / 1600 / 1920; Impac44b and Impac76b
// over 1600 (DFX2-80).
// [orig: HUD_InitAllFonts @0x51BD70 (dfx2.exe) - the widths @0x51BDCE / @0x51BDFE / @0x51BE2E /
//  @0x51BE5B / @0x51BE88, the names @0x51BDDA..0x51BECD, the scale @0x51BEE1, Impac44b @0x51BF0D
//  and Impac76b @0x51BF40 over 1600 @0x51BF2E / @0x51BF53, the divide's magic 0x51EB851F
//  @0x51BF12..0x51BF21 shifting by 9 (`sar edx, 9` @0x51BF19)]
constexpr TitleHudFontTier kHudFontTiers[] = {
	{640, "Arial14n.fnt", "Arial12b.fnt", 640},
	{800, "Arial14n.fnt", "Arial14b.fnt", 800},
	{1024, "Arial16n.fnt", "Arial16b.fnt", 1024},
	{1280, "Arial20n.fnt", "Arial20b.fnt", 1280},
	{1600, "Arial28n.fnt", "Arial28b.fnt", 1600},
	{0, "Arial32n.fnt", "Arial32b.fnt", 1920},
};
constexpr TitleHudFonts kHudFonts{kHudFontTiers, "Impac44b.fnt", "Impac76b.fnt", 1600};

// dfx2.exe's static kit pages, each a null-terminated list of the page's weapon names (every entry
// "<name> -1 -1 -1").
// The single-player page: WPN_MK23 where Jointops.exe's names WPN_colt45; WPN_M4 is on the page and
// absent from DFX2's data, which retail drops the same way (docs/titles/dfx2-vs-jo-re.md DFX2-33).
// [orig: Buffer_CopyUntilDoubleNull(restrictionData, 0x832368, 2048) @0x52188f / @0x54ec2a, the
//  profile's copy @0x548f67 (dfx2.exe)]
constexpr const char *kSinglePlayerKit[] = { "WPN_M4AUTO", "WPN_M4", "WPN_MK23", "WPN_AT4",
	"WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM", "WPN_KNIFE", nullptr };
// DFX2-29: a fresh profile's class pages. On each side classes 7 and 8 take the XM4AUTO and M4AUTO
// pages (each side its own copy of the same names), classes 5, 6 and 9 an empty page: static
// buffers nothing writes, each read by the copy alone. Jointops.exe gives every class its own page
// per side (playersav::default_kit_page).
// [orig: PlayerProfile_InitDefaults @0x548db0 - side 0 @0x548ff9..0x549051: byte_254D468,
//  byte_254C468, the page at 0x832b68, the page at 0x833b68, byte_254A468; side 1
//  @0x548f94..0x548fec: byte_254CC68, byte_254BC68, the page at 0x833368, the page at 0x834368,
//  byte_2549C68 (dfx2.exe)]
constexpr const char *kXm4AutoKit[] = { "WPN_XM4AUTO", "WPN_XM4", "WPN_XMK23", "WPN_XAT4",
	"WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM", "WPN_KNIFE", nullptr };
constexpr const char *kM4AutoKit[] = { "WPN_M4AUTO", "WPN_MK23", "WPN_SATCHEL_CHARGE",
	"WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM", "WPN_KNIFE", nullptr };
constexpr const char *kEmptyKit[] = { nullptr };
// Side 0's classes 5..9, then side 1's (Title::profile_class_kits).
constexpr const char *const *kProfileClassKits[] = {
	kEmptyKit, kEmptyKit, kXm4AutoKit, kM4AutoKit, kEmptyKit,
	kEmptyKit, kEmptyKit, kXm4AutoKit, kM4AutoKit, kEmptyKit,
};

// dfx2.exe's F12 map legend: 20 records, no medic, personmedicable, lfp, farp or armoury entry and
// a pwrweap one, 52 design units a row (DFX2-89).
// [orig: sub_497490 @0x497490 (dfx2.exe) - the records from aHudIconPerson @0x8144F8 to the empty
//  name @0x814868, the row step `mov [esp+8Ch], 34h` @0x497534]
constexpr TitleHudMapLegendIcon kHudMapLegendIcons[] = {
	{"hud_icon_person", 3, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_persondead", 8, 1.0f, 0x7F2F3F7Fu},
	{"hud_icon_waypoint", 1, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_psp", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_emplaced", 4, 0.8f, 0x7F7F6200u},
	{"hud_icon_emplaced2", 12, 0.8f, 0x7F7F6200u},
	{"hud_icon_vehicle", 10, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_watervehicle", 15, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_helo", 11, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_motorcycle", 25, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_bldg", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_bridge", 9, 1.0f, 0x7F207F20u},
	{"hud_icon_enemyloc", 24, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_recentspeaker", 28, 0.8f, 0x7F7F7F7Fu},
	{"hud_icon_ridewanted", 23, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrweap", 29, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrmed", 16, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrammo", 17, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_flag", 2, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_flagbay", 6, 1.0f, 0x7F7F7F7Fu},
};
constexpr TitleHudMapLegend kHudMapLegend{
		kHudMapLegendIcons, static_cast<int>(sizeof(kHudMapLegendIcons) / sizeof(kHudMapLegendIcons[0])),
		52};

Title make() {
	Title t;
	t.code = "dfx2";
	t.display_name = "Delta Force Xtreme 2";
	// [orig: Game_ParseCommandLineAndInit @0x4A7360 (dfx2.exe) - off_8144D8's literal into
	//  g_TitleText @0x4A7D60..0x4A7D6C, the twin of Game_ParseCommandLineAndInit @0x4A7310 (the
	//  title sprintf @0x4A7D30..0x4A7D3C)]
	t.title_text = "DeltaForce: Xtreme 2";
	// The program the DFX2 install ships beside its archives (docs/titles/dfx2-vs-jo-re.md §6.1,
	// DFX2-5).
	t.executable = "dfx2.exe";
	// The command file the Steam DFX2 install's UPDATE.WIZ names (CMD=DFX2.CMD,
	// PRODUCT_LOGIN=DFX2).
	t.command_file = "DFX2.CMD";
	// dfx2.exe's cfg table, one row longer than Jointops.exe's: the mpdfxcategory its host lists
	// print as DFXCATEGORY1 (gamecfg::config_var_rows, DFX2-7).
	t.config_table = gamecfg::ConfigTable::Dfx2;
	t.absent_resource_roles = kAbsentResourceRoles;
	// DFX2-8: a session of at most 250, 251 with a dedicated host's slot, at every site that holds
	// JO's 64 / 65; the host screen's cap word 100 (which its NovaWorld screen replaces with the
	// browser variable NWHL when the service sets one, a screen a headless host lacks).
	// [orig: Config_SetDefaults @0x54a2c0 (@0x54a33c..0x54a343); Game_ApplySessionSettingsToGlobals
	//  @0x54e750 (@0x54ed93..0x54ed9a); CNapiGameSession_ConnectOrHost @0x4d5100
	//  (@0x4d5255..0x4d526f); CNapiGameSession_BuildAndCreateSession @0x567707..0x56770e;
	//  Server_AllocatePlayerSlotTable @0x5191b0 (@0x5191c5) (dfx2.exe)]
	// [orig: g_HostMaxPlayersCap @0x835f0c, read by HostDialog_ReadSettings @0x553af0 (@0x553dd0),
	//  UI_MapSliderIndexToMaxPlayers @0x5540b0 and the populate's reads @0x5544cd / @0x55450a; the
	//  NWHL replace @0x5575ce in UI_ProcessLANSessionStateMachine @0x556ff0 (dfx2.exe)]
	t.max_players = 250;
	t.max_players_host_screen = 100;
	// [orig: HostDialog_PopulateWeaponRestrictionList @0x5547b0 (@0x5547c1);
	//  HostDialog_PopulateClassListCombo @0x5548b0 (@0x5548c2); @0x5562c0 (each restriction
	//  control's registration, @0x556318..0x55640b) (dfx2.exe)]
	t.host_screen = "NW_MULTI_PLAYER";
	// DFX2-36: the EXPPOINTS element and its 0x81 fade.
	// [orig: sub_5A6D20 @0x5A6D20 (@0x5A6EDE..0x5A6EE7); sub_59B7E0 @0x59B7E0; sub_59B730 called
	//  @0x4223C5 (dfx2.exe)]
	t.hud_exp_points = true;
	// DFX2-91: the lone-gunner panel [orig: sub_5BEEA0 @0x5BEFBA..0x5BEFEC; sub_5B8050 @0x5B8050
	//  (dfx2.exe)].
	t.hud_lone_gunner_panel = true;
	// DFX2-92: the waypoint's tint is the active colour halved, at the level band
	// [orig: sub_58EDF0 @0x58EDF0 (@0x58EDFE..0x58EE0A) (dfx2.exe)].
	t.hud_waypoint_active_tint = true;
	// DFX2-93: a done objective's mark is a green square [orig: sub_5B9890 @0x5B9890
	//  (@0x5B9AFA..0x5B9B12) (dfx2.exe)].
	t.hud_objective_done_fill = true;
	// DFX2-94: no pre-round armory line [orig: sub_5BCD20 @0x5BCD20 (@0x5BCDAA..0x5BCE30)
	//  (dfx2.exe)].
	t.hud_drops_armory_prompt = true;
	// DFX2-41 / DFX2-38: the DEATH screen's activation fills the three category lists, its close
	// on the triggers' clear applies them, and the serializer writes every grenade position
	// [orig: sub_550690 @0x550690 (Armory_ResolveSelectedClass @0x550ae0,
	//  UI_PopulateThreeCategoryLists @0x550ae5), run by UI_DispatchScreenEvent @0x54c231;
	//  sub_550BA0 @0x550ba0, called @0x5c9c09; WeaponLoadout_SerializeSelectionsToBuffer
	//  @0x564850 (@0x564b5c..0x564c20) (dfx2.exe)]. Jointops.exe's close calls nothing there and
	// its serializer tests each GRENADE_AMMO control [orig: nullsub_137 @0x5cac97;
	//  WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 (@0x565bcf..0x565be5)].
	t.death_screen_loadout = true;
	t.hud_fonts = &kHudFonts;
	// DFX2-22: the catalog sorted by title [orig: MissionList_ScanAndBuildFromFiles @0x5620d0 -
	//  the qsort @0x562550 over CompareFunction @0x561840, _stricmp on entry+1044 (dfx2.exe)].
	t.mission_list_order = MissionListOrder::Title;
	// DFX2-4: the profile files' magic, "2XFD" [orig: PlayerProfile_SaveToFiles @0x549080 - the
	//  header @0x5490ae; PlayerProfile_LoadAllFromDisk @0x54c720 - edi = 0x44465832 @0x54c7dc,
	//  the player.sav gate @0x54c800, the weapon.sav gate @0x54c93f (dfx2.exe)].
	t.profile_magic = 0x44465832u;
	// Record 0's fresh name, "Bravo 1", where Jointops.exe reads the account's [orig:
	//  PlayerProfile_LoadAllFromDisk @0x54c720 - the eight bytes @0x7d2d84 copied over the name
	//  @0x54c770..0x54c78a; no GetUserNameA call (dfx2.exe)].
	t.profile_first_name = "Bravo 1";
	// No `/maxplayers` switch: the walk goes from "/noreload" to "/version" [orig:
	//  Game_ParseCommandLineAndInit @0x4a7360 - @0x4a7727, @0x4a773f], and the host file's
	//  MaxPlayers stores its atol alone [orig: ServerConfig_ApplyHostSetting @0x4a6060 -
	//  @0x4a6289..0x4a6296 (dfx2.exe)].
	t.max_players_switch = false;
	// DFX2-29: a fresh profile's kit pages (above); the single-player page is the rules' spawn kit
	// too (DFX2-33, dfx2_rules.cpp).
	t.profile_single_player_kit = kSinglePlayerKit;
	t.profile_class_kits = kProfileClassKits;
	// DFX2-41: the DEATH map pane's ceiling 6.0 (flt_7CC0C8) and its frame, four lines round the
	// pane's rect in 0xFFE1DF4B after the map [orig: sub_550690 @0x550922 / @0x550974; sub_550F40
	// @0x551178 (the right drag), @0x550fe6..0x551052 (the map, then the frame); sub_550C60
	// @0x550ca6 (the wheel) (dfx2.exe)].
	t.death_map_zoom_max = 6.0f;
	t.death_map_frame_color = 0xFFE1DF4Bu;
	// DFX2-41: the reveal 360 ticks past the death, MAIN shown before the shroud; the activation
	// hides MAIN with it [orig: sub_552770 @0x552792 (360), CWnd_SetShown(MAIN, 1) @0x5527b1;
	// sub_550690 - CWnd_SetShown(MAIN, 0) @0x5507b8 (dfx2.exe)].
	t.death_shroud_reveal_ticks = 360;
	t.death_shroud_main = true;
	// DFX2-41: the team lists [orig: sub_5503A0 @0x5503a0, called by sub_552770 @0x5527e6
	// (dfx2.exe)].
	t.death_screen_team_lists = true;
	// DFX2-60: the campaign and instant-action tabs [orig: sub_560510 @0x560510 (sub_560190 called
	//  @0x560525, the IA_LIST test @0x560596, IA_BRIEFING @0x5606a6); sub_560ED0 @0x560ed0 (the tab
	//  radios, CA_LIST and CA_MISSION_LIST registered), run by sub_560F90 @0x560f90 (dfx2.exe)], the
	// twins of SinglePlayer_PopulateMissionList @0x561840 and UI_InitSinglePlayerScreen @0x562080.
	t.sp_campaign_tabs = true;
	// DFX2-61: the Mods list's base row reads "NONE" [orig: Options_PopulateModList @0x5582a0 (the
	//  literal @0x7CA4EC, read @0x5582d2) (dfx2.exe)].
	t.mod_list_base = "NONE";
	// DFX2-62: the render and audio populate hides OPTIONS' ADVANCED_OUTLINE and ADVANCED_TAB
	// [orig: sub_55AB20 @0x55ab20 (@0x55b829..0x55b862: CWnd_SetShown(0) on each) (dfx2.exe)], the
	// twin of UI_PopulateRenderAndAudioSettings @0x55c830, which ends at the Bink slots
	// (@0x55d530).
	t.options_hides_advanced = true;
	// DFX2-41: the no-zone instruction's penalty line [orig: sub_551E10 @0x55266f..0x55274f
	// (dfx2.exe)].
	t.death_respawn_penalty_line = true;
	// The HUD's own draws (DFX2-81..DFX2-90).
	// DFX2-90: the map key steps off <-> fullscreen [orig: sub_51DB80 @0x51DB80, called from the
	//  dispatcher's case 28 @0x4DD702 (dfx2.exe)].
	t.hud_map_cycle_skips_window = true;
	// DFX2-83: the Tab board's rows carry no class suffix [orig: sub_423890 @0x423890 - the team
	//  arm's format @0x423BB1 (dfx2.exe)].
	t.hud_kill_list_no_class = true;
	// DFX2-86: no magnification readout [orig: sub_59CEE0 @0x59CEE0 - the zero readout @0x59D428
	//  ends it (dfx2.exe)].
	t.hud_scope_no_magnification = true;
	// DFX2-84: the crosshair holds still, and no seat swaps it [orig: sub_590A80 @0x590A80 -
	//  @0x590F7F, the arms @0x590FB0..0x591032 (dfx2.exe)].
	t.hud_crosshair_fixed = true;
	// DFX2-85: the stance icon tinted by the health band [orig: sub_5A1A50 @0x5A1A50 -
	//  @0x5A1AB0..0x5A1AD4 (dfx2.exe)].
	t.hud_stance_health_tint = true;
	// DFX2-88: the message lines parse their tags [orig: sub_5B8CC0 @0x5B8DED, sub_5B8FF0
	//  @0x5B9036, sub_5B8E80 @0x5B8FAD (dfx2.exe)].
	t.hud_message_tags = true;
	t.hud_map_legend = &kHudMapLegend; // DFX2-89
	// DFX2-87: the tags' modes reach by range [orig: sub_5A27F0 @0x5A27F0 - @0x5A288C..0x5A28C1,
	//  @0x5A2E1F..0x5A309A (dfx2.exe)].
	t.hud_tag_ranges = true;
	// DFX2-81: the map's second name walk draws nothing [orig: sub_5A3850 @0x5A3850 -
	//  @0x5A3D53..0x5A3DD4 (dfx2.exe)].
	t.hud_map_no_bank_names = true;
	// DFX2-41: STATIC_GAMETYPE [orig: sub_550690 @0x550b41..0x550b77 (dfx2.exe)].
	t.death_screen_game_type_line = true;
	// DFX2-43: the cube's eye at least 5 units over the terrain [orig: EnvCube_Update @0x60F5F0 -
	//  `add eax, 50000h` @0x60F67B, the raise @0x60F689..0x60F68B (dfx2.exe)].
	t.env_cube_terrain_clearance = 0x50000;
	// DFX2-44: the NVG gain scale at the bottom centre [orig: the twin of NVG_DrawMaskAndGain
	//  @0x5CEEE0 - 0x1E0 @0x5CEF75, 0x2AA @0x5CEF7D, 0x220 @0x5CEF85, 0x2BE @0x5CEF8D (dfx2.exe)].
	t.nvg_gain_rect = TitleRect{ 480, 682, 544, 702 };
	return t;
}

} // namespace

const Title &dfx2_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
