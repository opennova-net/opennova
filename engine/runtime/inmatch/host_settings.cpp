#include "host_settings.h"

#include <base/io/crt_ftol.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace opennova::inmatch {
namespace {

int32_t edit_integer(const std::string& value) {
    // The CRT strtol at radix 10 on a signed 32-bit long even when the port's long is
    // wider, its white space the locale's (0xA0 included; io::retail_strtol, D-NET-384).
    // [orig: CEditWnd_GetIntValue @ 0x6575d0 — strtol(text, &end, 10) @0x6575e5]
    return io::retail_strtol(value.c_str(), 10);
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

bool apply_host_dialog_text(GameConfig& config, std::string_view control,
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
    else return false;
    return true;
}

bool apply_host_dialog_number(GameConfig& config, int32_t& player_limit,
                              bool& serve_and_play, std::string_view control,
                              int32_t number) {
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
    // [orig: Game_ApplySessionSettingsToGlobals @ 0x551500]
    else if (control == "KILL_LIMIT") config.score_limit = session_point_limit(number);
    else if (control == "MAX_SCORE") config.max_score = session_point_limit(number);
    else if (control == "MAX_KOTH") config.time_limit_minutes = session_koth_limit_minutes(number);
    // The dialog read clamps its edit to 64 [orig: HostDialog_ReadSettings
    // @ 0x555c25..0x555c2d].
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
    return true;
}

bool apply_host_dialog_control(GameConfig& config, int32_t& player_limit,
                               bool& serve_and_play, std::string_view control,
                               const std::string& value) {
    if (apply_host_dialog_text(config, control, value)) return true;
    return apply_host_dialog_number(config, player_limit, serve_and_play, control,
            edit_integer(value));
}

std::string host_dialog_value(const GameConfig& config, int32_t player_limit,
                              bool serve_and_play, std::string_view control) {
    // The populate step that seeds the MULTI_PLAYER_HOST screen from the
    // config before it shows: text edits by SetText, numeric edits by
    // SetIntValue, spins by SelectItemByValue. MAX_PLAYERS is clamped to 64
    // first; the four inverted flag spins show the bit CLEAR as 1; the 500-point
    // and nonpositive-KOTH sentinels map back to their dialog numbers (the read
    // side folds the apply's substitution into the config, see above).
    // [orig: UI_PopulateHostSettingsFromConfig @0x555fe0 -- GAME_NAME @0x556015,
    // SERVERTYPE @0x55608b, GAME_LOCATION strnicmp(country, item, 3) @0x5560fc,
    // CONNECTIONSPEED @0x556187, MAX_PLAYERS clamp @0x556328 + SetIntValue
    // @0x556358, TEAM_FF (flags & 0x200) == 0 @0x5563e1, ALLOW_SPECTATORS
    // maxSpectators != 0 @0x55651a, TOD_CONTINUITY @0x5565e3]
    if (control == "GAME_NAME") return config.server_name;
    if (control == "SERVER_PASSWORD") return config.server_password;
    if (control == "SERVER_MESSAGE") return config.custom_text;
    if (control == "BLUE_PW") return config.side_a_password;
    if (control == "RED_PW") return config.side_b_password;
    if (control == "SPECTATOR_PW") return config.spectator_password;
    if (control == "GAME_LOCATION") return config.country;
    const auto number = [](int64_t value) { return std::to_string(value); };
    for (const FlagControl& flag : kFlagControls) {
        if (control != flag.control) continue;
        return number(((config.mp_attributes & flag.bit) != 0) != flag.inverted ? 1 : 0);
    }
    if (control == "SERVERTYPE") return number(serve_and_play ? 0 : 1);
    if (control == "SERVER_PUNKBUSTER") return number(config.server_punkbuster);
    if (control == "SERVER_LANONLY") return number(config.server_lan_only);
    if (control == "CONNECTIONSPEED") return number(config.connection_speed);
    if (control == "REPLAY") return number(config.replay_enabled);
    if (control == "DELAY") return number(config.start_delay);
    if (control == "RESPAWN") return number(config.respawn_timeout);
    if (control == "TIME") return number(config.respawn_time);
    if (control == "KILL_LIMIT") return number(config.score_limit == kSessionNoPointLimit ? 500 : config.score_limit);
    if (control == "MAX_SCORE") return number(config.max_score == kSessionNoPointLimit ? 500 : config.max_score);
    if (control == "MAX_KOTH") return number(config.time_limit_minutes == kSessionNoKothLimitMinutes ? 0 : config.time_limit_minutes);
    if (control == "MAX_PLAYERS") return number(std::min(player_limit, 64));
    if (control == "MAX_FF_KILLS") return number(config.max_friendly_kills);
    if (control == "TAKEOVER_TIME") return number(config.capture_duration_seconds);
    if (control == "LFP_TAKEOVER") return number(config.capture_speed_setting);
    if (control == "ALLOW_SPECTATORS") return number(config.spectator_slots != 0 ? 1 : 0);
    if (control == "ALLOW_AI") return number(config.allow_ai ? 1 : 0);
    if (control == "TOD_CONTINUITY") return number(config.time_of_day_continuity);
    return std::string();
}

uint32_t session_point_limit(int32_t cfg_limit) {
    // [orig: Game_ApplySessionSettingsToGlobals @0x551B67..0x551B75 (g_ScoreLimit
    // from maxKills_408), @0x551B84..0x551B8C (g_KillLimit from maxScore_40C)]
    return cfg_limit == 500 ? kSessionNoPointLimit : static_cast<uint32_t>(cfg_limit);
}

uint32_t session_koth_limit_minutes(int32_t cfg_limit) {
    // [orig: Game_ApplySessionSettingsToGlobals @0x551CD2..0x551CDF
    // (g_TimeLimitMinutes from kothLimit_474)]
    return cfg_limit <= 0 ? kSessionNoKothLimitMinutes : static_cast<uint32_t>(cfg_limit);
}

uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play) {
    // Dedicated hosting reserves the extra host slot before publishing the
    // network limit; the 65 ceiling tests the PRE-increment cap (a dedicated
    // 65 publishes 66) and applies only for networkConnectType 1 (game.cfg's
    // `networkconnecttype`, default 1). The apply has no lower clamp: a blank
    // cap publishes 0 (1 dedicated). In session, though, the session's
    // creation clamps the cap into 1..65 before the apply runs, so the ceiling
    // never cuts and a blank cap publishes 1 (2 dedicated); host_config.h
    // host_session_settings ports that clamp, and the game's host, which calls
    // this with the screen's cap, still lacks it (D-NET-335's open half). The
    // same live count gates BMS placements.
    // [orig: Game_ApplySessionSettingsToGlobals @0x551b26..0x551b48;
    // CNapiGameSession_BuildAndCreateSession @0x56955D..0x56956C;
    // Config_SetDefaults @0x54d1d4 (networkConnectType_480 = 1);
    // Server_InitNewRoundState @ 0x51c8e0]
    int64_t total = static_cast<int64_t>(player_limit) + (serve_and_play ? 0 : 1);
    if (player_limit > static_cast<int32_t>(kMaxPlayersCap)) total = kMaxPlayersCap;
    // GameConfig::max_players is unsigned: retail's signed `count >= max`
    // @0x4c623f rejects every join for a negative cap, exactly as 0 does.
    return static_cast<uint32_t>(std::max<int64_t>(total, 0));
}

} // namespace opennova::inmatch
