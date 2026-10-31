#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opennova::gamecfg {
enum class ConfigTable : uint8_t; // formats/gamecfg/game_cfg.h
} // namespace opennova::gamecfg

namespace opennova::controls {
struct Catalog; // runtime/controls/controls.h
} // namespace opennova::controls

namespace opennova {

// The container format a NEW archive of any title is written in, as a PffFormat value (0 =
// PFF_FORMAT_PFF3): PFF3, ADR 0008's default for a newly created archive, which every JO-family
// title opens [orig: PFF_Open @ 0x7682e0]. One value for every title: a title that ships another
// flavour makes it a Title field.
inline constexpr int kNewArchivePffFormat = 0;

// The HUD's label fonts by screen width, as a title's executable loads them at each resolution
// change (runtime/hud/hud_math.h hud_label_font_choice reads them): the Arial pair of the first
// tier whose bound holds the width, each slot at (width << 16) / the tier's divisor, and the two
// large slots, one file each, at (width << 16) / the large divisor.
// [orig: HUD_InitAllFonts @0x51EE20] [orig: the twin @0x51BD70 (dfx2.exe)]
struct TitleHudFontTier {
	int32_t max_width = 0; // the widest screen the tier holds; 0 ends the table (any width)
	const char *normal_fnt = nullptr; // g_HUDLabelFont
	const char *bold_fnt = nullptr;   // g_HUDLabelFontBold
	int32_t divisor = 0;
};
struct TitleHudFonts {
	const TitleHudFontTier *tiers = nullptr; // its last tier's max_width is 0
	const char *large_fnt = nullptr;         // g_HUDLabelFontLarge
	const char *impact38_fnt = nullptr;      // g_HUDLabelFontImpact38
	int32_t large_divisor = 0;
};

// The mission loading screen's text as a title's executable composites it into the background
// (runtime/hud/loading_screen.h reads it): the two fonts, the narrow pair under `wide_width` pixels
// of background and the wide pair from it (0: the narrow pair at every width), the small one the
// server message's and the large one the band's; the message label's colour; the band's right
// edge over a custom background and over the stock one, from the image's left, or from its right
// edge when `band_from_right`; and the progress lines' pitch under and from `wide_width` (0: the
// executable draws none).
// [orig: Render_LoadingScreen @0x521D10] [orig: the twin @0x51EC60 (dfx2.exe)]
struct TitleLoadingScreen {
	const char *small_fnt = nullptr;
	const char *large_fnt = nullptr;
	int32_t wide_width = 0;
	const char *small_fnt_wide = nullptr;
	const char *large_fnt_wide = nullptr;
	uint32_t message_label_rgb = 0;
	int32_t band_right_custom = 0;
	int32_t band_right_stock = 0;
	bool band_from_right = false;
	int32_t progress_pitch = 0;
	int32_t progress_pitch_wide = 0;
};

// The order a title's mission catalog takes (runtime/mission/mission_catalog.h sort_rows), the
// comparator its boot scan's qsort is handed: by file name, Jointops.exe's; by the [Info] TITLE
// (the row's display title), dfx2.exe's. Each case-insensitive.
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 - qsort(.., 0x11E8,
//  Mission_CompareMapNames) @ 0x5635e4..0x5635f0; Mission_CompareMapNames @ 0x5628e0, stricmp on
//  entry+0] [orig: MissionList_ScanAndBuildFromFiles @0x5620d0 - qsort(.., 0x11E8,
//  CompareFunction) @0x562544..0x562550; CompareFunction @0x561840, _stricmp on entry+1044
//  (dfx2.exe)]
enum class MissionListOrder : uint8_t { FileName, Title };

// The F12 map legend's table, the title's executable's (runtime/hud's element_map_legend draws
// it): one entry per icon, its gametext Hud key, TSDicon strip cell, size factor and ARGB, laid in
// two columns from design y 144 a design `row_step` apart.
// [orig: HUD_DrawHelpScreenIcons @0x497480 - the 44-byte records from aHudIconPerson, name +0x00,
//  cell +0x20, size +0x24, colour +0x28, the walk ending on an empty name] [orig: sub_497490
//  @0x497490 (dfx2.exe)]
struct TitleHudMapLegendIcon {
	const char *name = nullptr;
	uint8_t cell = 0;
	float size = 0.0f;
	uint32_t color = 0;
};
struct TitleHudMapLegend {
	const TitleHudMapLegendIcon *icons = nullptr;
	int count = 0;
	int row_step = 0;
};

// A rect in the HUD's 1024 x 768 design space by its corners, as the executable's draws push
// them: (x0, y0) the top left, (x1, y1) the bottom right.
struct TitleRect {
	int32_t x0 = 0;
	int32_t y0 = 0;
	int32_t x1 = 0;
	int32_t y1 = 0;
};

// A title (CONTEXT.md "Title"): the retail game a game runs as, by its short code (the `/game
// <code>` launch flag, a project's target game). Its rows live in games/<code>/<code>_title.cpp,
// which games::install() hands to install_titles; engine/ holds the type and the lookups alone.
// A field no binary of the title has been read for is null, never another title's value.
struct Title {
	// The short launch-flag token, lowercase ("jo", "dfx2", "acidgambit").
	const char *code = nullptr;
	// The name a player and an artist read ("Joint Operations").
	const char *display_name = nullptr;
	// The title text the executable copies into g_TitleText: the _connectlog.txt header's TITLE
	// line and the VERSION command's title word.
	const char *title_text = nullptr;
	// The program the title's install ships beside its archives, which a player starts (and the
	// editor's Play in the game install).
	const char *executable = nullptr;
	// The command file a game folder's stock UPDATE.WIZ names for the title (its CMD), which
	// the launch reads for the folder's product (ADR 0067 d2).
	const char *command_file = nullptr;
	// The updater login of a title whose content OpenNova's service serves with no wizard in the
	// folder yet (its own wizard's PRODUCT_LOGIN, the product the updater installs: OP: Acid
	// Gambit's). Null for a retail title, whose folder's own UPDATE.WIZ names its login.
	const char *updater_login = nullptr;
	// The game.cfg table the title's executable walks (gamecfg::config_var_rows).
	gamecfg::ConfigTable config_table{};
	// DFX2-8: the player caps the title's executable compiles in. `max_players` is the most a
	// session takes: the config default's clamp and the NovaWorld host leg's (64, dfx2.exe's 250),
	// one more with a dedicated host's own slot (the session create's, the published and the slot
	// table's ceiling: 65, 251); `max_players_host_screen` the host screen's MAX_PLAYERS cap (64,
	// dfx2.exe's 100). On the Title, not the title's rules (runtime/world/title_rules.h) nor its
	// Protocol: they are rules, not wire, and the host leg (net/novaworld/connect_or_host.h) and the
	// service's hosted limits read them beside the session's caps (inmatch title_player_caps). The
	// game.cfg rows and ceilings keyed by config_table stay formats/gamecfg's. 0 unwitnessed.
	int32_t max_players = 0;
	int32_t max_players_host_screen = 0;
	// The mp.mnu screen the host dialog's restriction lists and their radios live on, the one the
	// title's executable names at each of them (runtime/inmatch/host_settings.h): Jointops.exe's
	// MULTI_PLAYER_HOST, dfx2.exe's NW_MULTI_PLAYER, the same code over other data
	// (docs/titles/dfx2-vs-jo-re.md §10). Null unwitnessed: the lists are found on no screen.
	const char *host_screen = nullptr;
	// The roles of the boot's required resources (base/gameprofile/required_resources.h) the
	// title's stock data never ships and its program still reads unchecked: each is optional for
	// the title. Null-terminated; null for none.
	const char *const *absent_resource_roles = nullptr;
	// Whether the title's executable mounts an expansion (`/exp` / `/mod`: an expansion folder's
	// archives over the base game's), which a project may build as (the editor's
	// check_project_expansion). False where no binary of the title was read for it.
	bool expansions = false;
	// Every .def name the title's executable opens, each at its open site: a definitions text under
	// any other name the game never reads (the editor's game_never_reads). Null-terminated, as
	// absent_resource_roles is; null: no claim, the title's binary unread for them.
	const char *const *read_defs = nullptr;
	// DFX2-36: the executable's HUD draws the EXPPOINTS element (runtime/hud/hud_exp_points.h:
	// the label, the S2C 0x81 score and the fade its fanfare starts, the S2C 0x80 / 0x82 tracker's
	// lines). Jointops.exe parses the EXPPOINTS line and reads none of it, and its 0x81 starts no
	// fade. On the Title, not the title's rules nor its Protocol: it is the executable's own HUD,
	// single player's too, and changes no wire byte and no world.
	// [orig: HUD_ParseHudposToken @0x59F370 (@0x5A0463..0x5A04AF, no reader)] [orig: sub_5A6D20
	//  @0x5A6EDE, sub_59B7E0 @0x59B7E0, sub_59B730 called @0x4223C5 (dfx2.exe)]
	bool hud_exp_points = false;
	// DFX2-91: the executable's HUD draws the lone-gunner panel, the HUD side of DFX2-37: while the
	// local player is a lone gunner (world::local_rides_solo_turret), the overlay panels' pass
	// boxes "The tank requires another player / in order to use the cannon" in the large slot
	// (HudFrameCompiler::element_lone_gunner_panel). Jointops.exe carries the drawer and its string
	// with no caller. On the Title: the executable's own HUD, no wire byte and no world.
	// [orig: @0x5B9100 (no function, no xref)] [orig: sub_5BEEA0 @0x5BEFBA..0x5BEFEC, sub_5B8050
	//  @0x5B8050 (dfx2.exe)]
	bool hud_lone_gunner_panel = false;
	// DFX2-92: the executable's waypoint step tints the waypoint pointer and its altitude nub with
	// g_HUDColors.active halved, alpha and all, at the level band (2), whatever the waypoint's
	// height. False (Jointops.exe): three colours and bands by the waypoint's height against the
	// local player's, +-2.0 apart. On the Title: the executable's own HUD.
	// [orig: HUD_UpdateWaypointAltitudeColor @0x590970] [orig: sub_58EDF0 @0x58EDF0 (dfx2.exe)]
	bool hud_waypoint_active_tint = false;
	// DFX2-93: the executable's objectives panel marks a done row with a green square, a device
	// clear inside its box; false (Jointops.exe): a red X, both diagonals tripled. On the Title:
	// the executable's own HUD.
	// [orig: HUD_DrawWinConditions @0x5BA940 (@0x5BABA4..0x5BAC5B)] [orig: sub_5B9890 @0x5B9890
	//  (@0x5B9AFA..0x5B9B12) (dfx2.exe)]
	bool hud_objective_done_fill = false;
	// DFX2-94: the executable's gameplay overlays carry no pre-round armory line
	// (STROVER_ARMORY_INFO / _WAIT): a running pre-round timer draws no service line, as the
	// vehicle bay's waits on its end in both. False (Jointops.exe): the armory line while it
	// runs and no menu screen is open. On the Title: the executable's own HUD.
	// [orig: HUD_DrawGameplayOverlays @0x5BDE60 (@0x5BDEE9..0x5BDF45)] [orig: sub_5BCD20
	//  @0x5BCD20 (@0x5BCDAA..0x5BCE30) (dfx2.exe)]
	bool hud_drops_armory_prompt = false;
	// DFX2-95: the executable's in-session end-game (I) screen puts its footer in the text box's
	// bottom corners: "Page n of 4" right-aligned at its right edge and the paging hint
	// left-aligned at its left, both at the box's bottom (each point scaled again). False
	// (Jointops.exe): both lines centred on the screen, at y 700 and 720. On the Title: the
	// executable's own HUD (HudFrameCompiler::element_end_game_screen).
	// [orig: HUD_DrawEndGameScreen @0x5BD070 (@0x5BDD67..0x5BDE31)] [orig: sub_5BBF20 @0x5BBF20
	//  (@0x5BCC3D..0x5BCCF0) (dfx2.exe)]
	bool hud_end_game_footer_corners = false;
	// DFX2-41: the deploy screen picks the loadout. death.mnu's DEATH screen carries PRIMARY /
	// SECONDARY / ACCESSORY lists its activation fills for the player's class, and the screen's
	// close on its triggers' clear submits them (the loadout apply, a C2S 0x2F in session); its
	// spawn points are a SPAWNPOINTS_TABLE, not a list. With it the loadout serializer writes every
	// grenade position whose weapon resolves, shown or not, as the death screen authors no grenade
	// control (DFX2-38). False (Jointops.exe): the DEATH screen's SPAWNPOINTS_LIST, a close that
	// applies nothing, and only the shown grenade positions serialized.
	bool death_screen_loadout = false;
	// The HUD's label fonts by screen width (TitleHudFonts); null where no binary of the title was
	// read for them, which loads none (the HUD's slots fall back to its hudpos font).
	const TitleHudFonts *hud_fonts = nullptr;
	// DFX2-22: the order its mission catalog takes (MissionListOrder above): every list the
	// catalog feeds, the single-player screen's among them, shows it.
	MissionListOrder mission_list_order = MissionListOrder::FileName;
	// DFX2-4: the magic word player.sav and weapon.sav open with (runtime/profile/player_profiles.h):
	// the one each file's load gates on and its save writes, ahead of the "0211" version word both
	// titles share. Jointops.exe's "FPBC", dfx2.exe's "2XFD": neither game reads the other's
	// profile files. 0 unwitnessed.
	uint32_t profile_magic = 0;
	// The name the profile load gives record 0 before player.sav's records are read over it
	// (runtime/profile/player_profiles.h): dfx2.exe's literal "Bravo 1"; null, the process's
	// account name, as Jointops.exe reads it through GetUserNameA.
	const char *profile_first_name = nullptr;
	// The +1396 dword a fresh profile record carries (runtime/profile/player_profiles.h
	// init_defaults, playersav::ProfileRecord::word_1396): dfx2.exe's defaults store 1, which no
	// code of its reads; Jointops.exe's leave the memset's 0. A file-parity value.
	int32_t profile_word_1396 = 0;
	// Whether the executable's command line takes `/maxplayers N`, the cap a host file's
	// MaxPlayers line yields to (inmatch::read_host_file's max_player_arg): Jointops.exe's does;
	// dfx2.exe's walk knows no such switch, and its host file's MaxPlayers line stores its own
	// value alone. False unwitnessed.
	bool max_players_switch = false;
	// DFX2-29: the kit pages PlayerProfile_InitDefaults copies into a fresh profile's weapon
	// record (runtime/profile/player_profiles.h init_defaults), each a null-terminated list of
	// weapon names (every entry "<name> -1 -1 -1"; an empty list an empty page): the
	// single-player page, and the ten class pages, side 0's classes 5..9 then side 1's. Each title
	// a binary was read for names its own (Jointops.exe's are playersav::kJointOpsSinglePlayerKit
	// and kJointOpsClassKits); null unwitnessed, a title no launch runs (refused by name), whose
	// fresh record init_defaults gives no page.
	const char *const *profile_single_player_kit = nullptr;
	const char *const *const *profile_class_kits = nullptr;
	// DFX2-58: the map views' zoom ceiling, the CMAP window's (its right drag and its ZOOMIN /
	// ZOOMOUT step clamp to it) and the DEATH screen's map pane's (the show's fit, the right drag
	// and the wheel clamp to it, and a fit landing on it resets the pan, the target and the zoom
	// ease): Jointops.exe's 10.0 (flt_7C44B4) at every site, dfx2.exe's 6.0 (flt_7CC0C8)
	// [orig: CMapWindow_HandleEvent @0x5497f0 - @0x548276, @0x549d5c; CMap_OverlayInputHandler
	//  @0x554310 - @0x5544df; DeathScreen_UpdateUI @0x553150 - @0x5533a0 / @0x5533e5;
	//  DeathScreen_ZoomStep @0x5535d0 - @0x553616].
	float map_view_zoom_max = 10.0f;
	// DFX2-41: the colour of the frame the DEATH screen's map pane draws round the map after it
	// (0: none, Jointops.exe's).
	uint32_t death_map_frame_color = 0;
	// DFX2-41: the DEATH screen's reveal: the ticks past the local death after which its
	// DEATH_SHROUD shows (at once under the deploy overlay), and whether its MAIN window hides
	// with the shroud at the activation and shows with it. Jointops.exe's: 240, MAIN untouched
	// [orig: DeathScreen_UpdateShroudReveal @0x554730 - the compare @0x554752; DeathScreen_UpdateUI
	//  @0x553150 - the shroud's hide @0x55325f].
	int32_t death_shroud_reveal_ticks = 240;
	bool death_shroud_main = false;
	// DFX2-41: the DEATH screen's content refresh also writes its team lists, TEAMBLUE and
	// TEAMRED, and its STATIC_TEAM line (world::deploy_team_lists). False (Jointops.exe): the call
	// there is an empty stub [orig: nullsub_177 @0x553140].
	bool death_screen_team_lists = false;
	// DFX2-60: the SINGLE_PLAYER screen's two tabs. Its populate first reads campaign.def into
	// CA_LIST (each campaign whose depends are not all complete locked), the shown campaign's
	// missions into CA_MISSION_LIST and its briefing into BRIEFING, then lists in IA_LIST the plain
	// Co-op missions no campaign holds or the profile has completed, an IA_LIST pick writing
	// IA_BRIEFING; the tab radios refresh ACCEPT from their list's selection; the round end's
	// completion byte then has campaign missions to write (runtime/menu/menu_flow.h,
	// runtime/mission/campaign.h). False (Jointops.exe): IA_LIST alone, every waypoint-family
	// mission, its pick writing BRIEFING.
	bool sp_campaign_tabs = false;
	// DFX2-61: the key of the Mods list's base-game row, looked up through the list's string table
	// (its text where the table holds none): the literal the executable carries. Null unwitnessed:
	// the row is blank.
	const char *mod_list_base = nullptr;
	// DFX2-62: the OPTIONS screen's render and audio populate ends by hiding its ADVANCED_OUTLINE
	// and ADVANCED_TAB controls (menu::OptionsScreen::apply_title). False (Jointops.exe): they show
	// as authored.
	bool options_hides_advanced = false;
	// DFX2-41: the DEATH screen's second instruction with no spawn zone for a team's player:
	// outside an assault game, or in a 0x20000 one while dead, a dead player under a penalty
	// reads "<PENALTYTIMER> <cFF4040><n><co> <SECONDS>.", else RESPAWN4 under 0x20000 and RESPAWN3
	// otherwise; an assault game's other case RESPAWN4 (world::build_deploy_instructions). False
	// (Jointops.exe): RESPAWN3 outside assault, RESPAWN4 or, dead out of 0x20000, RESPAWN5
	// [orig: UI_UpdateDeathScreenContent @0x5536a0 - @0x554271..0x5542ef].
	bool death_respawn_penalty_line = false;
	// The executable's own HUD where dfx2.exe's parts from Jointops.exe's (DFX2-81..DFX2-90), each
	// false for Jointops.exe's draw. On the Title, not the title's rules nor its Protocol: each is
	// the executable's own HUD, single player's too, and changes no wire byte and no world.
	// DFX2-90: the map key steps off -> fullscreen -> off, never the north-up window (mode 2);
	// Jointops.exe's steps off -> window -> fullscreen -> off (hud::HudMapControl::cycle).
	// [orig: HUD_CycleMapMode @0x520BC0] [orig: sub_51DB80 @0x51DB80 (dfx2.exe)]
	bool hud_map_cycle_skips_window = false;
	// DFX2-83: the Tab board's rows carry no " (class)" suffix; Jointops.exe's stashes the class
	// name of a row whose entity shares the local team and appends it to the next drawn row
	// (hud::ScoreboardRowComposer).
	// [orig: HUD_DrawKillList @0x423A30 (the stash @0x423D64..0x423DC3, " (%s)" @0x424104)]
	// [orig: sub_423890 @0x423890 (dfx2.exe) - "%s<ch>%s<co> [%02ld]" @0x423BB1, no stash]
	bool hud_kill_list_no_class = false;
	// DFX2-86: a scoped weapon's overlay draws no magnification readout (Hud/hud_scope_mag at
	// hudpos SCOPEMAG); its range and zero readouts are Jointops.exe's.
	// [orig: HUD_DrawScopeOverlayDetails @0x59E420 (the readout @0x59E97C..0x59E9F6)]
	// [orig: sub_59CEE0 @0x59CEE0 (dfx2.exe) - the zero readout @0x59D428 ends it]
	bool hud_scope_no_magnification = false;
	// DFX2-84: the crosshair's arms hold still and a Controller / Driver seat keeps the ordinary
	// crosshair: Jointops.exe spreads the arms by the weapon's ERROR row for the stance (prone,
	// crouched, standing; 2 in the air, the water or on a ladder, 1 mounted) plus the recoil
	// words, and in camera mode 1 swaps a Controller / Driver seat to the driver crosshair, its
	// arms closed, in half-bright palette 1.
	// [orig: HUD_DrawCrosshair @0x592640 (the spread @0x592b25..0x592bf5, the seat swap
	//  @0x592bf8..0x592c2d)] [orig: sub_590A80 @0x590A80 (dfx2.exe) - the projection times
	//  flt_7C2244 (0.0) @0x590F7F, no seat swap]
	bool hud_crosshair_fixed = false;
	// DFX2-85: the stance icon takes the stance colour triple (hudpos stancecolor_good / _middle /
	// _bad) by the HUD info's health band, where Jointops.exe's takes stanceicon_color.
	// [orig: HUD_DrawStanceIndicator @0x599F10 (dword_2723AE8 @0x599F76)] [orig: sub_5A1A50
	//  @0x5A1A50 (dfx2.exe) - HUD_ClassifyHealthBand @0x5A1AB6, the triple @0x5A1AC2..0x5A1AD4]
	bool hud_stance_health_tint = false;
	// DFX2-88: the chat lines of the J window's CHAT column and of a dedicated host's console
	// lines parse their inline tags, as every other line does; Jointops.exe draws those two with
	// the tags off (flag 0x100), a '<' a glyph. Its third reader, the console window, is
	// HUD_DrawConsoleMessagesOverlay, which OpenNova does not port.
	// [orig: HUD_DrawMessageLog @0x5B9D70 (push 100h @0x5B9E9D); HUD_DrawServerConsoleLines
	//  @0x5BA0A0 (@0x5BA0E6); HUD_DrawConsoleMessagesOverlay @0x5B9F30 (@0x5BA05D)]
	// [orig: sub_5B8CC0 @0x5B8DED, sub_5B8FF0 @0x5B9036, sub_5B8E80 @0x5B8FAD, each push 0
	//  (dfx2.exe)]
	bool hud_message_tags = false;
	// DFX2-89: the F12 map legend (TitleHudMapLegend): Jointops.exe's 26 entries 40 design units a
	// row, dfx2.exe's 20 (no medic, personmedicable, lfp, farp or armoury entry, a pwrweap one) 52
	// a row. Null where no binary of the title was read for it: the legend draws its box alone.
	const TitleHudMapLegend *hud_map_legend = nullptr;
	// DFX2-87: the friendly tags' modes reach by range: FAR/BRIEF (1) draws every tag's name, FULL
	// (2) the name within the tag's reach (50 u for an AI organic, 1000 u for a player whose slot
	// +0x3C word is the local player's slot's, else 300 u) or for the HUD target cursor's entity
	// and the brief tick beyond, BRIEF (3) the tick. Jointops.exe's FULL draws every name, its mode
	// 1 the name within 300 u and nothing beyond.
	// [orig: HUD_DrawEntityLabel @0x5A39B0 (the mode tests @0x5A3FC3..0x5A40D4)]
	// [orig: sub_5A27F0 @0x5A27F0 (dfx2.exe) - the reach @0x5A288C..0x5A28C1, the mode switch
	//  @0x5A2E1F..0x5A309A]
	bool hud_tag_ranges = false;
	// DFX2-81: the map's second name walk draws nothing: dfx2.exe still finds each same-team
	// transient-bank person's position and display name, and draws no label; Jointops.exe's names
	// them in the regular label face (hud::HudMinimapInput::bank_names).
	// [orig: HUD_DrawEntityLabelsAndMarkers @0x5A49E0 (@0x5A4EDE..0x5A4FA3, the draw @0x5A4F95)]
	// [orig: sub_5A3850 @0x5A3850 (dfx2.exe) - @0x5A3D53..0x5A3DD4]
	bool hud_map_no_bank_names = false;
	// DFX2-41: the DEATH screen's activation writes STATIC_GAMETYPE, "Gametype: <cFFFF40>%s" over
	// the game type's Overlays label (world::apply_deploy_activation). False (Jointops.exe): its
	// show writes no such line [orig: DeathScreen_UpdateUI @0x553150].
	bool death_screen_game_type_line = false;
	// DFX2-43: how far over the terrain under the local player the environment cube's capture eye
	// stands at least (Q16 world units; runtime/renderer/environment_cube.h
	// environment_cube_eye_height): Jointops.exe's 10 units, dfx2.exe's 5. 0 unwitnessed.
	int32_t env_cube_terrain_clearance = 0;
	// DFX2-44: the NVG gain scale indicator's rect over the NVG mask (runtime/hud/view_effects.h
	// nvg_scale_rect): Jointops.exe's (960, 32)..(1008, 64) at the top right, dfx2.exe's
	// (480, 682)..(544, 702) at the bottom centre. Zero unwitnessed.
	TitleRect nvg_gain_rect;
	// DFX2-45: the weapon Inset's reticle is a textured quad over the aperture's rect, not
	// Jointops.exe's four crosshair arms (runtime/hud HudFrameCompiler::element_inset_cues). dfx2.exe
	// draws its quad through a helper whose texture global no load writes, so nothing draws there;
	// the ring and the friendly brackets stay.
	// [orig: Render_WeaponInsetScene @0x5C9740 - the arms @0x5C9E96..0x5C9F47] [orig: the twin
	//  @0x5C86B0 - Viewport_ScreenToVirtual @0x5C8E0F / @0x5C8E23, sub_591440 @0x5C8E40; sub_591440
	//  @0x591440, its texture 0x2724E2C read @0x59145A and written nowhere (dfx2.exe)]
	bool inset_reticle_quad = false;
	// DFX2-47: the side of FrameFX's two square work targets ([fx+4] / [fx+8]) and of every pass
	// that draws into the work rect (runtime/renderer/frame_fx_effects.h frame_fx_work_side):
	// Jointops.exe's 256, dfx2.exe's 512. 0 unwitnessed.
	int32_t frame_fx_work_side = 0;
	// DFX2-48: the scene frame dispatches FrameFX's low-health ring (type 5) below the local
	// player's health ceiling (runtime/renderer/frame_fx_effects.h FrameFxFrameInputs::
	// low_health_ring). Jointops.exe's dispatcher carries the type with no caller.
	// [orig: FrameFX_ApplyScreenEffect @0x584440 (callers @0x5CA92E, @0x5CAA3F, @0x5CAA71,
	//  @0x5CAA97: types 0, 4, 1, 2)] [orig: the twin of Render_ProcessMainSceneFrame @0x5C9000 -
	//  @0x5C98DE..0x5C995D (dfx2.exe)]
	bool low_health_screen_effect = false;
	// DFX2-49: the map pass (HUD_DrawMapOverlay: the corner minimap, the M-cycle big map, the
	// command map) walks pool 3 once more ahead of its marker layers when its mask carries
	// 0x810, drawing each 6027 / 6028 capture-zone ring there too: under the marker banks, and
	// again in the usual walk over them (runtime/hud HudMinimapOverlays::
	// capture_rings_under_layers).
	// [orig: HUD_DrawMapOverlay @0x5A5F40 - the one pool-3 walk @0x5A750A, past
	//  MapOverlay_RenderAllLayers @0x5A6D2B] [orig: the twin @0x5A4FC0 - the walk @0x5A5DA3
	//  (6027 @0x5A5DDE, 6028's sub_5958E0 @0x5A5E68) ahead of MapOverlay_RenderAllLayers
	//  @0x5A5E8D, the usual walk @0x5A667B (dfx2.exe)]
	bool map_capture_rings_under_layers = false;
	// DFX2-50: the DEATH screen's map pass (MapOverlay_DrawView) walks pool 3 twice more: the
	// 6027 / 6028 capture-zone rings ahead of its marker layers, and past its labels and laser
	// leg the 6006 KOTH ring and the 2044 location labels, with no mask test (runtime/hud
	// HudMinimapOverlays::death_map_zone_markers). Jointops.exe's pass walks pool 3 never.
	// [orig: MapOverlay_DrawView @0x5A58E0 - MapOverlay_RenderAllLayers @0x5A5A11,
	//  HUD_DrawEntityLabelsAndMarkers @0x5A5A29, Render_LaserSightEffect @0x5A5A40, then the zone
	//  walk] [orig: the twin @0x5A48A0 - the capture walk @0x5A49C2..0x5A4A0D (sub_5958E0
	//  @0x5A4A0D) ahead of MapOverlay_RenderAllLayers @0x5A4A26; the KOTH / location walk
	//  @0x5A4A62..0x5A4ABA (Minimap_DrawKothZoneRing @0x5A4AA2, sub_595710 @0x5A4ABA) past the
	//  laser leg @0x5A4A55 and its flush @0x5A4A5D (dfx2.exe)]
	bool death_map_zone_markers = false;
	// DFX2-63: the mission loading screen's text (TitleLoadingScreen); null where no binary of the
	// title was read for it, which composites none.
	const TitleLoadingScreen *loading_screen = nullptr;
	// DFX2-110: the static binding catalog the executable compiles in (runtime/controls/
	// controls.h Catalog): every row's action code, config token, class, flags and default
	// bindings, which the default binding table player.sav's entries merge into, the live
	// binding records, the key-config and help screens and every by-code reader take. Null:
	// Jointops.exe's (controls::default_catalog).
	const controls::Catalog *controls = nullptr;
	// DFX2-96: out of a session the Briefing window (the I key) draws the single-player how-to
	// (Overlays STROVER_SPMISSIONHOWTO) and the briefing panel is the Objectives row's window
	// (runtime/hud/hud_toggles.h hud_briefing_window). False (Jointops.exe): the Briefing window
	// draws the briefing panel; the how-to has no caller. On the Title: the executable's own HUD.
	bool hud_sp_how_to = false;
	// DFX2-97: the end-round STAT screen's DONE (inmatch/stat_screen_feed.h): its show hides it,
	// the menu-screen pass shows it while a joiner's S2C 0x25 round-reset hold is up, and its press
	// ends the joiner's end-round linger (mission exit 4). False (Jointops.exe): no DONE is
	// registered and the pass's call there is an empty stub [orig: nullsub_130 @0x562130, from
	// UI_ProcessMenuScreenFrame @0x54e681; HUD_CacheStatPanelValues @0x5627a0].
	bool stat_screen_done = false;
};

// Fills the registry with `titles` (each outliving the program) and names its default: the title
// a launch, a project or a session that names none runs as. A second call replaces the first.
void install_titles(const Title *const *titles, size_t count, const Title *default_title);

// Every lookup below aborts on an EMPTY registry, naming the invariant: a program that reaches
// them calls games::install() at its start (games/games.h).

// The title whose code is `code`, compared case-insensitively; null for an unknown one.
const Title *title_by_code(std::string_view code);
// The title a launch that names none runs as.
const Title &default_title();
// The installed titles, in install order; title_at is null past the end.
size_t title_count();
const Title *title_at(size_t index);
// The title whose command_file is `command_file` (compared case-insensitively, as the updater
// opens the file); null for none.
const Title *title_by_command_file(std::string_view command_file);

} // namespace opennova
