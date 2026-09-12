#include "host_settings.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace opennova::inmatch {
namespace {

int32_t edit_integer(const std::string& value) {
    // Windows strtol is signed 32-bit even when the port's long is wider.
    // [orig: CEditWnd_GetIntValue @ 0x6575d0]
    const long long parsed = std::strtoll(value.c_str(), nullptr, 10);
    return static_cast<int32_t>(std::clamp(parsed,
            static_cast<long long>(std::numeric_limits<int32_t>::min()),
            static_cast<long long>(std::numeric_limits<int32_t>::max())));
}

struct FlagControl {
    std::string_view control;
    uint32_t bit;
    bool inverted;
};
constexpr FlagControl kFlagControls[] = {
    {"TEAM_FF", 0x200u, true}, {"FRIENDLY_TAG", 0x400u, true},
    {"FF_WARNING", 0x8u, true}, {"TEAM_CHOOSE", 0x4u, false},
    {"CLAYMORE_PREF", 0x8000u, false}, {"TRACERS", 0x1u, true},
};

} // namespace

const std::vector<std::string_view>& host_dialog_controls() {
    // [orig: HostDialog_ReadSettings @ 0x555940]
    static const std::vector<std::string_view> controls = {
        "GAME_NAME", "SERVER_PASSWORD", "SERVER_MESSAGE", "SERVERTYPE",
        "GAME_LOCATION", "SERVER_PUNKBUSTER", "SERVER_LANONLY", "CONNECTIONSPEED",
        "REPLAY", "DELAY", "RESPAWN", "TIME", "KILL_LIMIT", "MAX_SCORE", "MAX_KOTH",
        "MAX_PLAYERS", "MAX_FF_KILLS", "TAKEOVER_TIME", "LFP_TAKEOVER", "TEAM_FF",
        "FRIENDLY_TAG", "FF_WARNING", "TEAM_CHOOSE", "CLAYMORE_PREF", "TRACERS",
        "ALLOW_SPECTATORS", "ALLOW_AI", "BLUE_PW", "RED_PW", "SPECTATOR_PW",
        "TOD_CONTINUITY",
    };
    return controls;
}

bool apply_host_dialog_control(GameConfig& config, int32_t& player_limit,
                               bool& serve_and_play, std::string_view control,
                               const std::string& value) {
    // The edit capacities are literal byte counts (no invented -1).
    // [orig: HostDialog_ReadSettings @ 0x555940;
    // CStaticWnd_GetLabelText @ 0x657570]
    if (control == "GAME_NAME") config.server_name = value.substr(0, 32);
    else if (control == "SERVER_PASSWORD") config.server_password = value.substr(0, 17);
    else if (control == "SERVER_MESSAGE") config.custom_text = value.substr(0, 128);
    else if (control == "BLUE_PW") config.side_a_password = value.substr(0, 17);
    else if (control == "RED_PW") config.side_b_password = value.substr(0, 17);
    else if (control == "SPECTATOR_PW") config.spectator_password = value.substr(0, 17);
    else if (control == "GAME_LOCATION") config.country = value.substr(0, 3);
    else {
        const int32_t number = edit_integer(value);
        for (const FlagControl& flag : kFlagControls) {
            if (control != flag.control) continue;
            if ((number != 0) != flag.inverted) config.mp_attributes |= flag.bit;
            else config.mp_attributes &= ~flag.bit;
            return true;
        }
        if (control == "SERVERTYPE") serve_and_play = number == 0;
        else if (control == "SERVER_PUNKBUSTER") config.server_punkbuster = number;
        else if (control == "SERVER_LANONLY") config.server_lan_only = number;
        else if (control == "CONNECTIONSPEED") config.connection_speed = number;
        else if (control == "REPLAY") config.replay_enabled = static_cast<uint32_t>(number);
        else if (control == "DELAY") config.start_delay = static_cast<uint32_t>(number);
        else if (control == "RESPAWN") config.respawn_timeout = static_cast<uint32_t>(number);
        else if (control == "TIME") config.respawn_time = static_cast<uint32_t>(number);
        // Config's 500-point limits and nonpositive KOTH limit have distinct
        // runtime sentinels; TIME has no such substitution.
        // [orig: apply_session_settings_to_globals @ 0x551500]
        else if (control == "KILL_LIMIT") config.score_limit = number == 500 ? 65000u : static_cast<uint32_t>(number);
        else if (control == "MAX_SCORE") config.max_score = number == 500 ? 65000u : static_cast<uint32_t>(number);
        else if (control == "MAX_KOTH") config.time_limit_minutes = number <= 0 ? 0x2222222u : static_cast<uint32_t>(number);
        else if (control == "MAX_PLAYERS") player_limit = std::min(number, 64);
        else if (control == "MAX_FF_KILLS") config.max_friendly_kills = number;
        else if (control == "TAKEOVER_TIME") config.capture_duration_seconds = number;
        else if (control == "LFP_TAKEOVER") config.capture_speed_setting = number;
        else if (control == "ALLOW_SPECTATORS") {
            if (number == 0) config.spectator_slots = 0;
            else if (config.spectator_slots == 0) config.spectator_slots = -1;
        } else if (control == "ALLOW_AI") config.allow_ai = number != 0;
        else if (control == "TOD_CONTINUITY") config.time_of_day_continuity = number;
        else return false;
    }
    return true;
}

uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play) {
    // Dedicated hosting reserves the extra host slot before publishing the
    // network limit. The same live count gates BMS placements.
    // [orig: apply_session_settings_to_globals @ 0x551500;
    // Server_InitNewRoundState @ 0x51c8e0]
    const int64_t total = static_cast<int64_t>(player_limit) + (serve_and_play ? 0 : 1);
    return static_cast<uint32_t>(std::clamp<int64_t>(total, 1, kMaxPlayersCap));
}

} // namespace opennova::inmatch
