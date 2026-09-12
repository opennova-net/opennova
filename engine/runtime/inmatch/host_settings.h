#pragma once

#include "game_config.h"
#include <string_view>

namespace opennova::inmatch {

// Selected spin values and raw edit strings from the authored host dialog.
// Absent controls leave the request untouched.
// [orig: HostDialog_ReadSettings @ 0x555940;
// apply_session_settings_to_globals @ 0x551500]
const std::vector<std::string_view>& host_dialog_controls();
bool apply_host_dialog_control(GameConfig& config, int32_t& player_limit,
                               bool& serve_and_play, std::string_view control,
                               const std::string& value);
uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play);

} // namespace opennova::inmatch
