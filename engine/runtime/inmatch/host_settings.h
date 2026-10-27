#pragma once

#include "game_config.h"
#include <formats/gamecfg/game_cfg.h>
#include <formats/rtxt/rtxt.h>
#include <string>
#include <string_view>

namespace opennova::menu {
class MenuRuntime;
}
namespace opennova::mnu {
struct Document;
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
// hosting URL. So a dialog left untouched (the game shell's NovaWorld page,
// whose map picker stands in for it) gives the block back as the screen
// shows it: the populate's cap clamp, with no pb\pbcl.dll the PunkBuster
// spin's 0, and every other control's read of what its populate showed
// (read_untouched_host_dialog; D-NET-1108).
// [orig: UI_InitHostSettingsDialog @0x558960 - the populate @0x5589C7;
//  UI_PopulateHostSettingsFromConfig - GAME_NAME @0x556010, the pb\pbcl.dll
//  test @0x55613A; UI_HandleHostSessionStart - the NovaWorld arm
//  (networkConnectType_480 == 1) @0x556E47; HostDialog_ReadSettings - GAME_NAME
//  @0x555962, SERVER_PUNKBUSTER @0x555A9B]
//
// The screen both walks find every control on. [orig: UI_FindScreenControl
// @0x63AE80 (g_GameMenu, "MULTI_PLAYER_HOST", the control's name) at each
// control of UI_PopulateHostSettingsFromConfig and HostDialog_ReadSettings]
inline constexpr std::string_view kHostDialogScreen = "MULTI_PLAYER_HOST";
// The controls, in the read's order. [orig: HostDialog_ReadSettings @0x555940]
const std::vector<std::string_view>& host_dialog_controls();
// One control's selected spin value or raw edit string into the block, as the
// read stores it: the edits by their byte caps, the numeric edits by strtol,
// MAX_PLAYERS clamped to the title's screen cap (the block's table names the
// title), the six attribute spins onto their mpattrib bits, ALLOW_SPECTATORS
// keeping a nonzero spectator count. False for an unknown control.
// [orig: HostDialog_ReadSettings @0x555940 - per control in host_settings.cpp]
bool apply_host_dialog_control(gamecfg::GameCfg& cfg, std::string_view control,
                               const std::string& value);
// The populate's one write: the block's cap clamped to the title's screen cap
// before the screen shows it. [orig: UI_PopulateHostSettingsFromConfig
// @0x556328..0x55632A]
void populate_host_dialog(gamecfg::GameCfg& cfg);
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
//  host_settings.cpp]
void populate_host_screen(menu::MenuRuntime& screen, gamecfg::GameCfg& cfg);
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
// [orig: HostDialog_ReadSettings @0x555940]
void read_host_screen(const menu::MenuRuntime& screen, gamecfg::GameCfg& cfg);
// The dialog left untouched: the mounted menu's MULTI_PLAYER_HOST opened
// fresh (null `doc`, or a menu without the screen: no control), populated and
// read back, so the block takes each word as the screen gives it back.
// [orig: UI_InitHostSettingsDialog @0x558960 - UI_PopulateHostSettingsFromConfig
//  @0x5589C7; UI_HandleHostSessionStart @0x556D00 - HostDialog_ReadSettings
//  @0x556E36, the NovaWorld arm @0x556E47]
void read_untouched_host_dialog(gamecfg::GameCfg& cfg, const mnu::Document* doc);
// The live player-slot limit the session apply makes of a cap: the cap, plus
// the host's own slot when dedicated (GameConfig::player_slot_limit), under the
// title's ceiling (TitlePlayerCaps::session; `title` a gameprofile::GameId).
uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play, int title);
// The session create's cap: in session the cap goes into 1..65 (dfx2.exe:
// 1..251) before the apply (host_player_slot_limit), so a blank cap is 1 (a
// slot limit of 2 dedicated) and a dedicated 65 makes the ceiling's 66. It is
// GameConfig::max_players. Both hosts run it: inmatch::host_session_settings
// over the cfg block and the game's host request.
// [orig: CNapiGameSession_BuildAndCreateSession @0x569554 (is_in_session),
//  @0x56955D..0x56955F (below 1 -> 1), @0x56956A..0x56956C (above 65 -> 65)]
// [orig: @0x5676fb..0x56770e (dfx2.exe)]
int32_t session_player_cap(int32_t player_limit, int title);
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
