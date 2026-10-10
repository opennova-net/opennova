#pragma once

#include "game_config.h"
#include <string_view>

namespace opennova::inmatch {

// Selected spin values and raw edit strings from the authored host dialog.
// Absent controls leave the request untouched.
// [orig: HostDialog_ReadSettings @ 0x555940;
// Game_ApplySessionSettingsToGlobals @ 0x551500]
const std::vector<std::string_view>& host_dialog_controls();
bool apply_host_dialog_control(GameConfig& config, int32_t& player_limit,
                               bool& serve_and_play, std::string_view control,
                               const std::string& value);
// The two arms of that read, for a writer that parses its own values (the
// host file reads with atol): the seven text edits and their byte caps, and
// every numeric control. False for a control the arm does not own.
bool apply_host_dialog_text(GameConfig& config, std::string_view control,
                            const std::string& value);
bool apply_host_dialog_number(GameConfig& config, int32_t& player_limit,
                              bool& serve_and_play, std::string_view control,
                              int32_t number);
// The inverse: the value the host screen shows for a control before the user
// touches it -- edit text, or the spin/checkbox item value as a decimal
// string; GAME_LOCATION yields the country the shell matches against the
// item names. Empty for an unknown control.
// [orig: UI_PopulateHostSettingsFromConfig @ 0x555fe0]
std::string host_dialog_value(const GameConfig& config, int32_t player_limit,
                              bool serve_and_play, std::string_view control);
// The live player-slot limit the session apply makes of a cap: the cap, plus
// the host's own slot when dedicated (GameConfig::player_slot_limit).
uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play);
// The session create's cap: in session the cap goes into 1..65 before the apply
// (host_player_slot_limit), so a blank cap is 1 (a slot limit of 2 dedicated)
// and a dedicated 65 makes the ceiling's 66. It is GameConfig::max_players.
// Both hosts run it: inmatch::host_session_settings over the cfg block and the
// game's host request.
// [orig: CNapiGameSession_BuildAndCreateSession @0x569554 (is_in_session),
//  @0x56955D..0x56955F (below 1 -> 1), @0x56956A..0x56956C (above 65 -> 65)]
int32_t session_player_cap(int32_t player_limit);
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
