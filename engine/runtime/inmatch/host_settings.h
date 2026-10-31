#pragma once

#include "game_config.h"
#include <formats/gamecfg/game_cfg.h>
#include <formats/gamecfg/server_settings.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/hud/game_text_lookup.h>
#include <string>
#include <string_view>

namespace opennova::menu {
class HostDialog;
class MenuRuntime;
}

namespace opennova::inmatch {

// The MULTI_PLAYER_HOST screen works on the process's cfg block: it opens
// showing the block's words, and its START reads every control back into the
// block, saves it, and creates the session from it (host_config.h
// host_session_settings), so a word the screen does not show keeps the
// file's. [orig: UI_PopulateHostSettingsFromConfig @0x555FE0;
// UI_HandleHostSessionStart @0x556D00 - HostDialog_ReadSettings @0x556E36,
// Game_SaveConfig @0x556E72; CNapiGameSession_BuildAndCreateSession @0x5694D0]
// The NovaWorld host runs the same dialog: its open populates the controls,
// its START reads them back into the block, then its NovaWorld arm opens the
// hosting URL; the game shell's NovaWorld page opens this screen for it.
// [orig: UI_InitHostSettingsDialog @0x558960 - the populate @0x5589C7;
//  UI_PopulateHostSettingsFromConfig - GAME_NAME @0x556010, the pb\pbcl.dll
//  test @0x55613A; UI_HandleHostSessionStart - the NovaWorld arm
//  (networkConnectType_480 == 1) @0x556E47; HostDialog_ReadSettings - GAME_NAME
//  @0x555962, SERVER_PUNKBUSTER @0x555A9B]
//
// Both walks find every control on the title's own host screen (Title::host_screen): Jointops.exe's
// MULTI_PLAYER_HOST, dfx2.exe's NW_MULTI_PLAYER, which dfx2.exe's walks name at every control (it
// names MULTI_PLAYER_HOST only for its LAN screen's show of four location and LAN-only controls,
// unported). [orig: UI_FindScreenControl @0x63AE80 (g_GameMenu, the screen, the control's name)
// at each control of UI_PopulateHostSettingsFromConfig and HostDialog_ReadSettings]
// [orig: UI_PopulateHostSettingsFromConfig @0x554190 (GAME_NAME @0x5541ae); HostDialog_ReadSettings
//  @0x553af0 (GAME_NAME @0x553b00); UI_InitLANMultiplayerScreen @0x554ae0 - MULTI_PLAYER_HOST's
//  STATIC_GAME_LOCATION, GAME_LOCATION, STATIC_SERVER_LANONLY, SERVER_LANONLY @0x554b9b..0x554c01
//  (dfx2.exe)]
// The controls, in the read's order. [orig: HostDialog_ReadSettings @0x555940]
const std::vector<std::string_view>& host_dialog_controls();
// One control's selected spin value or raw edit string into the block, as the
// read stores it: the edits by their byte caps, the numeric edits by strtol,
// MAX_PLAYERS clamped to `title`'s screen cap (title_player_caps), the six attribute spins onto their mpattrib bits, ALLOW_SPECTATORS
// keeping a nonzero spectator count. False for an unknown control.
// [orig: HostDialog_ReadSettings @0x555940 - per control in host_settings.cpp]
bool apply_host_dialog_control(gamecfg::GameCfg& cfg, const Title& title, std::string_view control,
                               const std::string& value);
// The populate's one write: the block's cap clamped to the title's screen cap
// before the screen shows it. [orig: UI_PopulateHostSettingsFromConfig
// @0x556328..0x55632A]
void populate_host_dialog(gamecfg::GameCfg& cfg, const Title& title);
// What the host screen shows for a control from the block: edit text, or the
// spin/checkbox item value as a decimal string; GAME_LOCATION yields the
// country the shell matches against the item names (strnicmp over 3 chars).
// The limits show the block's words as they stand (no session sentinel).
// Empty for an unknown control.
// [orig: UI_PopulateHostSettingsFromConfig @0x555FE0 - per control in
//  host_settings.cpp]
std::string host_dialog_value(const gamecfg::GameCfg& cfg, std::string_view control);
// The screen's open over the menu runtime holding it: the populate's clamp
// (populate_host_dialog), then each control the screen holds shows the
// block's word (host_dialog_value): a spin list the row whose value= is the
// word, row 0 when no row holds it; GAME_LOCATION the first row whose name
// matches the country over three characters, the row left when none does;
// SERVER_PUNKBUSTER, with no pb\pbcl.dll (none ships with OpenNova), the row
// of value 0, disabled; an edit by its text set (the class's {hot} cut); a
// checkbox (the shipped screen has none) checked on a nonzero word. A control
// the screen lacks is skipped.
// [orig: UI_PopulateHostSettingsFromConfig @0x555FE0 - per control in
//  host_settings.cpp] [orig: UI_PopulateHostSettingsFromConfig @0x554190 (dfx2.exe)]
void populate_host_screen(menu::MenuRuntime& screen, gamecfg::GameCfg& cfg, const Title& title);
// The player count the host screen recommends for a CONNECTIONSPEED row (the block's
// nwisptype): rows 0..17 map to 1, 2, 2, 2, 3, 3, 4, 5, 6, 8, 9, 11, 14, 16, 21, 23, 26, 32
// under the title's screen cap, row 18 the cap itself, any other row 0. The populate shows
// it in STATIC_MAXPLAYERS_RECOMMENDATION through menutxt.bin's MAX_PLAYERS format
// (populate_host_screen); nothing reads it back.
// [orig: UI_MapSliderIndexToMaxPlayers @0x555F00 (the cap g_HostScreenMaxPlayers @0x555F1F /
//  @0x555F86)] [orig: UI_MapSliderIndexToMaxPlayers @0x5540B0, g_HostMaxPlayersCap (dfx2.exe)]
int32_t map_slider_index_to_max_players(int32_t slider_index, int32_t host_screen_cap);
// START's read of the screen into the block, each control it holds in the
// read's order through apply_host_dialog_control: a spin list its selected
// row's value= (GAME_LOCATION the row's name), an edit its text, a checkbox
// 1 or 0.
// [orig: HostDialog_ReadSettings @0x555940] [orig: HostDialog_ReadSettings @0x553af0 (dfx2.exe)]
void read_host_screen(const menu::MenuRuntime& screen, gamecfg::GameCfg& cfg, const Title& title);
// What the settings schema takes of `title` (gamecfg::ServerSettingsTitle): its cfg table, its
// host screen's MAX_PLAYERS cap and the availability words of the protocol it speaks. A title code
// the registry lacks reads as the default title's.
gamecfg::ServerSettingsTitle server_settings_title(const Title& title);
gamecfg::ServerSettingsTitle server_settings_title(std::string_view title_code);
// The screen's restriction lists from the block, as the dialog's init fills them after its
// populate, and the LAN screen's activation fills WEAPON_LIST again. Each list, and each radio
// below, is the one on the title's own host screen (Title::host_screen: Jointops.exe's
// MULTI_PLAYER_HOST, dfx2.exe's NW_MULTI_PLAYER); a title that names none has no lists.
// WEAPON_LIST: cleared, then a row per loadout_selectable weapon in the table's order, its text
// the weapon's WepDes name
// (`gametext`, its loadout_menu_textid; "" with none), its value its table row, its column 1 the
// word's Game.bin "Menu" token (`menu_ui`): -1 and the mission word MISSION, 0 NEVER, the
// armory-only word ARMORY, any other ALWAYS. CLASS_LIST: cleared, a row per class 5..9 (Game.bin's
// CHARCLASS_* name, its value the class word's index), column 1 NEVER for 0, MISSION for 2,
// ALWAYS for any other. A token the table lacks reads "??KEY??".
// [orig: UI_InitHostSettingsDialog @0x558960 - @0x5589CC, @0x5589D1; UI_InitLANMultiplayerScreen
//  @0x556930 - @0x5569CC; HostDialog_PopulateWeaponRestrictionList @0x556600;
//  HostDialog_PopulateClassListCombo @0x556700; TextResource_GetStringWithFallback @0x562EE0]
// [orig: HostDialog_PopulateWeaponRestrictionList @0x5547b0; HostDialog_PopulateClassListCombo
//  @0x5548b0 (dfx2.exe)]
void populate_host_weapon_list(menu::MenuRuntime& screen, const gamecfg::GameCfg& cfg, const Title& title,
                               const gamecfg::WeaponRoster& weapons, const hud::GameTextLookup& gametext,
                               const hud::GameTextLookup& menu_ui);
void populate_host_class_list(menu::MenuRuntime& screen, const gamecfg::GameCfg& cfg, const Title& title,
                              const hud::GameTextLookup& menu_ui);
// A press on a restriction list's row in its column 1 (the 0x8000001 cell event, its press or
// double-click form): WEAPON_LIST steps the row's weapon word up to the title's mission word and
// back to 0 (Jointops.exe 0 -> 1 -> 2 -> 3 -> 0, dfx2.exe 0 -> 1 -> 2 -> 0, its ARMORY 3 never
// reached), CLASS_LIST the row's class word 0 -> 1 -> 2 -> 0 and then the START gate (`dialog`,
// set_class_words); the cell shows the new token. False for another widget or column, or a row
// past the list.
// [orig: HostDialog_WeaponRestrictionCycleHandler @0x5576c0; HostDialog_OnClassAvailabilityListClick
//  @0x5577d0]
// [orig: @0x5558a0; HostDialog_OnWeaponRestrictionListClick @0x5559b0 (dfx2.exe)]
bool host_restriction_list_pressed(menu::MenuRuntime& screen, menu::HostDialog& dialog,
                                   gamecfg::GameCfg& cfg, const Title& title, std::string_view widget, int row,
                                   int column, const hud::GameTextLookup& menu_ui);
// A press on a radio that sets every word: WPN_ALWAYS 1, WPN_NEVER 0, WPN_ARMORY the title's
// armory-only word and WPN_MISSION its mission word (Jointops.exe's 2 and 3, dfx2.exe's 3 and 2)
// over each loadout_selectable weapon, every WEAPON_LIST row's cell relabelled; CLASS_ALWAYS 1,
// CLASS_NEVER 0 and CLASS_MISSION 2 over the ten class words, every CLASS_LIST row's cell
// relabelled, then the START gate. False for another control.
// [orig: UI_RegisterHostSettingsCallbacks @0x5580f0 (the controls and their values);
//  HostDialog_SetAllWeaponAvailability @0x557910; HostDialog_SetAllClassAvailability @0x557a10]
// [orig: @0x5562c0 (the controls and their values); @0x555af0; HostDialog_SetAllWeaponRestrictions
//  @0x555bf0 (dfx2.exe)]
bool host_restriction_radio_pressed(menu::MenuRuntime& screen, menu::HostDialog& dialog,
                                    gamecfg::GameCfg& cfg, const Title& title,
                                    const gamecfg::WeaponRoster& weapons, std::string_view control,
                                    const hud::GameTextLookup& menu_ui);
// The live player-slot limit the session apply makes of a cap: the cap, plus
// the host's own slot when dedicated (GameConfig::player_slot_limit), under the
// title's ceiling (TitlePlayerCaps::session) when the cfg block's network connect
// type is 1 (GameConfig::network_connect_type).
uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play,
                                int32_t network_connect_type, const Title& title);
// The session create's cap: in session the cap goes into 1..65 (dfx2.exe:
// 1..251) before the apply (host_player_slot_limit), so a blank cap is 1 (a
// slot limit of 2 dedicated) and a dedicated 65 makes the ceiling's 66. It is
// GameConfig::max_players. Both hosts run it: inmatch::host_session_settings
// over the cfg block and the game's host request.
// [orig: CNapiGameSession_BuildAndCreateSession @0x569554 (is_in_session),
//  @0x56955D..0x56955F (below 1 -> 1), @0x56956A..0x56956C (above 65 -> 65)]
// [orig: @0x5676fb..0x56770e (dfx2.exe)]
int32_t session_player_cap(int32_t player_limit, const Title& title);
// The session apply's two limit substitutions, one rule for the host screen's
// read and for the game.cfg apply (host_config.h): a cfg point limit of 500
// is the 65000 no-limit sentinel, and a nonpositive KOTH limit is 0x2222222
// minutes. [orig: Game_ApplySessionSettingsToGlobals @0x551B67..0x551B8C
// (g_ScoreLimit, g_KillLimit), @0x551CD2..0x551CDF (g_KothLimitMinutes)]
inline constexpr uint32_t kSessionNoPointLimit = 65000u;
inline constexpr uint32_t kSessionNoKothLimitMinutes = 0x2222222u;
uint32_t session_point_limit(int32_t cfg_limit);
uint32_t session_koth_limit_minutes(int32_t cfg_limit);

// The session title (g_SessionTitle), as every mission start's settings apply leaves it: after
// copying the cfg's server name in, it zeroes the word and fills it with the [TITLE] entry the
// mission-title table holds under the map file's name cut at its last '.' (the entry's text up to
// its first CR, at most 31 bytes), else the map file's first 31 bytes. The title table is never
// loaded (no writer), so the lookup answers from the mount's text-override table alone (null:
// none), and a map name with no '.' looks nothing up. The status report prints it (SERVER:), the
// Co-op S2C 0x7B and the stock Co-op MISSIONNAME advertise it; SetServerName and a joiner's final
// S2C 0x64 overwrite it.
// [orig: Game_ApplySessionSettingsToGlobals - the cfg copy @0x551FDA, the '.' cut @0x552112..
//  0x552127, TextResource_FindEntryBySectionAndKey(g_MissionTitleTextResource, "TITLE", ..)
//  (the call @0x552135, its result @0x55213D), the zero @0x552145..0x552168, the CR cut and
//  the 31 cap @0x552174..0x552190, the map file @0x55219E; g_MissionTitleTextResource @0x2550194 (read @0x5520C7 / @0x55212A, no
//  writer); TextResource_FindEntryBySectionAndKey @0x75D250 (the override table first @0x75D27B)]
std::string mission_session_title(const rtxt::File *text_override, const std::string &map_file);

} // namespace opennova::inmatch
