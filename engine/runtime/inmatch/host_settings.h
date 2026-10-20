#pragma once

#include "game_config.h"
#include <formats/gamecfg/game_cfg.h>
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

} // namespace opennova::inmatch
