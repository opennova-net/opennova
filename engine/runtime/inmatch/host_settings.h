#pragma once

#include "game_config.h"
#include <formats/gamecfg/game_cfg.h>
#include <formats/rtxt/rtxt.h>
#include <string>
#include <string_view>

namespace opennova::inmatch {

// The MULTI_PLAYER_HOST screen works on the process's cfg block: it opens
// showing the block's words, and its START reads every control back into the
// block, saves it, and creates the session from it (host_config.h
// host_session_settings), so a word the screen does not show keeps the
// file's. [orig: UI_PopulateHostSettingsFromConfig @0x555FE0;
// UI_HandleHostSessionStart @0x556D00 - HostDialog_ReadSettings @0x556E36,
// Game_SaveConfig @0x556E72; CNapiGameSession_BuildAndCreateSession @0x5694D0]
//
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
// (g_ScoreLimit, g_KillLimit), @0x551CD2..0x551CDF (g_TimeLimitMinutes)]
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
