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

namespace {

// The title the block's table runs: dfx2.exe's carries the one row
// Jointops.exe's lacks (gamecfg::config_var_rows).
int block_title(const gamecfg::GameCfg& cfg) {
    return cfg.table == gamecfg::ConfigTable::Dfx2 ? gameprofile::GAME_DFX2 : gameprofile::GAME_JO;
}

} // namespace

bool apply_host_dialog_control(gamecfg::GameCfg& cfg, std::string_view control,
                               const std::string& value) {
    // The text edits by their capacities, literal byte counts (no invented -1)
    // [orig: HostDialog_ReadSettings @0x555940 - GAME_NAME @0x555967 (32),
    //  SERVER_PASSWORD @0x555990 (17), SERVER_MESSAGE @0x5559BC (128), BLUE_PW
    //  @0x555E7F, RED_PW @0x555EA8, SPECTATOR_PW @0x555ED1 (17 each);
    //  CStaticWnd_GetLabelText @0x657570]; the location's first three characters
    // [orig: strncpy(country_3C5, item, min(len, 3)) @0x555A72].
    if (control == "GAME_NAME") { cfg.game_name = value.substr(0, 32); return true; }
    if (control == "SERVER_PASSWORD") { cfg.mp_host_game_password = value.substr(0, 17); return true; }
    if (control == "SERVER_MESSAGE") { cfg.servermsg = value.substr(0, 128); return true; }
    if (control == "BLUE_PW") { cfg.mp_host_side_password_a = value.substr(0, 17); return true; }
    if (control == "RED_PW") { cfg.mp_host_side_password_b = value.substr(0, 17); return true; }
    if (control == "SPECTATOR_PW") { cfg.mp_host_spectator_password = value.substr(0, 17); return true; }
    if (control == "GAME_LOCATION") { cfg.country = value.substr(0, 3); return true; }
    const int32_t number = edit_integer(value);
    // The six attribute spins onto their mpattrib bits, four of them inverted
    // [orig: @0x555CBC..0x555DE8].
    for (const FlagControl& flag : kFlagControls) {
        if (control != flag.control) continue;
        if ((number != 0) != flag.inverted) cfg.mpattrib = static_cast<int32_t>(static_cast<uint32_t>(cfg.mpattrib) | flag.bit);
        else cfg.mpattrib = static_cast<int32_t>(static_cast<uint32_t>(cfg.mpattrib) & ~flag.bit);
        return true;
    }
    // [orig: SERVERTYPE dedicatedServer_400 @0x5559DE, SERVER_PUNKBUSTER @0x555A9B,
    //  SERVER_LANONLY hostLanOnly_340 @0x555AC0, CONNECTIONSPEED nwiSpType_540
    //  @0x555AE5, REPLAY @0x555B0A, DELAY startDelay_410 @0x555B31, RESPAWN
    //  respawnTimeout_47C @0x555B58, TIME timeLimit_470 @0x555B7F, KILL_LIMIT
    //  maxKills_408 @0x555BA6, MAX_SCORE maxScore_40C @0x555BCD, MAX_KOTH
    //  kothLimit_474 @0x555BF4, MAX_PLAYERS maxPlayers_3F4 @0x555C1B with the
    //  screen cap @0x555C25..0x555C2D (dfx2.exe's 100 @0x553dd0), MAX_FF_KILLS
    //  @0x555C54, TAKEOVER_TIME teamChangeTime_4C0 @0x555C7B, LFP_TAKEOVER
    //  @0x555CA0, ALLOW_SPECTATORS @0x555E0A..0x555E23, ALLOW_AI enableAi_A14
    //  @0x555E53, TOD_CONTINUITY @0x555EF3]
    if (control == "SERVERTYPE") cfg.dedicated = number;
    else if (control == "SERVER_PUNKBUSTER") cfg.sv_punkbuster = number;
    else if (control == "SERVER_LANONLY") cfg.mp_novaworld_host_lan_only = number;
    else if (control == "CONNECTIONSPEED") cfg.nwisptype = number;
    else if (control == "REPLAY") cfg.replay = number;
    else if (control == "DELAY") cfg.startdelay = number;
    else if (control == "RESPAWN") cfg.timeout = number;
    else if (control == "TIME") cfg.time_limit = number;
    else if (control == "KILL_LIMIT") cfg.max_kills = number;
    else if (control == "MAX_SCORE") cfg.max_score = number;
    else if (control == "MAX_KOTH") cfg.koth_limit = number;
    else if (control == "MAX_PLAYERS")
        cfg.mp_max_players = std::min(number, title_player_caps(block_title(cfg)).host_screen);
    else if (control == "MAX_FF_KILLS") cfg.numallowablefriendlykills = number;
    else if (control == "TAKEOVER_TIME") cfg.teamchange_time = number;
    else if (control == "LFP_TAKEOVER") cfg.mp_lfp_takeoverspeed = number;
    else if (control == "ALLOW_SPECTATORS") {
        if (number == 0) cfg.mp_num_spectators_max = 0;
        else if (cfg.mp_num_spectators_max == 0) cfg.mp_num_spectators_max = -1;
    } else if (control == "ALLOW_AI") cfg.enable_ai = number != 0 ? 1 : 0;
    else if (control == "TOD_CONTINUITY") cfg.mp_tod_continuity = number;
    else return false;
    return true;
}

void populate_host_dialog(gamecfg::GameCfg& cfg) {
    // [orig: UI_PopulateHostSettingsFromConfig @0x556328..0x55632A, dword_83C79C]
    // [orig: @0x5544cd (dfx2.exe)]
    cfg.mp_max_players = std::min(cfg.mp_max_players, title_player_caps(block_title(cfg)).host_screen);
}

std::string host_dialog_value(const gamecfg::GameCfg& cfg, std::string_view control) {
    // Text edits by SetText, numeric edits by SetIntValue, spins by
    // SelectItemByValue; the four inverted flag spins show the bit CLEAR as 1.
    // [orig: UI_PopulateHostSettingsFromConfig @0x555FE0 - GAME_NAME @0x556015,
    //  SERVERTYPE @0x55608B, SERVER_LANONLY @0x5560B2, GAME_LOCATION strnicmp(country,
    //  item, 3) @0x5560FC, SERVER_PUNKBUSTER @0x55614E, CONNECTIONSPEED @0x556187,
    //  REPLAY @0x556222, DELAY @0x55624E, TIME @0x5562A0, MAX_SCORE @0x5562F2,
    //  MAX_PLAYERS @0x556358, LFP_TAKEOVER @0x5563B2, TEAM_FF (flags & 0x200) == 0
    //  @0x5563E1, FRIENDLY_TAG @0x556439, FF_WARNING @0x556468, TRACERS @0x5564EE,
    //  ALLOW_SPECTATORS maxSpectators != 0 @0x55651A, ALLOW_AI @0x556541,
    //  TOD_CONTINUITY @0x5565E3]
    if (control == "GAME_NAME") return cfg.game_name;
    if (control == "SERVER_PASSWORD") return cfg.mp_host_game_password;
    if (control == "SERVER_MESSAGE") return cfg.servermsg;
    if (control == "BLUE_PW") return cfg.mp_host_side_password_a;
    if (control == "RED_PW") return cfg.mp_host_side_password_b;
    if (control == "SPECTATOR_PW") return cfg.mp_host_spectator_password;
    if (control == "GAME_LOCATION") return cfg.country;
    const auto number = [](int64_t value) { return std::to_string(value); };
    for (const FlagControl& flag : kFlagControls) {
        if (control != flag.control) continue;
        return number(((static_cast<uint32_t>(cfg.mpattrib) & flag.bit) != 0) != flag.inverted ? 1 : 0);
    }
    if (control == "SERVERTYPE") return number(cfg.dedicated);
    if (control == "SERVER_PUNKBUSTER") return number(cfg.sv_punkbuster);
    if (control == "SERVER_LANONLY") return number(cfg.mp_novaworld_host_lan_only);
    if (control == "CONNECTIONSPEED") return number(cfg.nwisptype);
    if (control == "REPLAY") return number(cfg.replay);
    if (control == "DELAY") return number(cfg.startdelay);
    if (control == "RESPAWN") return number(cfg.timeout);
    if (control == "TIME") return number(cfg.time_limit);
    if (control == "KILL_LIMIT") return number(cfg.max_kills);
    if (control == "MAX_SCORE") return number(cfg.max_score);
    if (control == "MAX_KOTH") return number(cfg.koth_limit);
    if (control == "MAX_PLAYERS") return number(cfg.mp_max_players);
    if (control == "MAX_FF_KILLS") return number(cfg.numallowablefriendlykills);
    if (control == "TAKEOVER_TIME") return number(cfg.teamchange_time);
    if (control == "LFP_TAKEOVER") return number(cfg.mp_lfp_takeoverspeed);
    if (control == "ALLOW_SPECTATORS") return number(cfg.mp_num_spectators_max != 0 ? 1 : 0);
    if (control == "ALLOW_AI") return number(cfg.enable_ai);
    if (control == "TOD_CONTINUITY") return number(cfg.mp_tod_continuity);
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

TitlePlayerCaps title_player_caps(int title) {
    TitlePlayerCaps caps;
    if (title == gameprofile::GAME_DFX2) {
        caps.session = kMaxPlayersCapDfx2;
        // [orig: dfx2.exe g_HostMaxPlayersCap @0x835f0c = 100], against Jointops.exe's 64
        // [orig: dword_83C79C @0x83C79C]
        caps.host_screen = 100;
    }
    return caps;
}

uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play, int title) {
    // Dedicated hosting reserves the extra host slot in the live slot limit
    // (dword_24D211C); the session's advertised and admitted cap
    // (GameConfig::max_players) never carries it. The 65 ceiling tests the
    // PRE-increment cap (a dedicated
    // 65 publishes 66) and applies only for networkConnectType 1 (game.cfg's
    // `networkconnecttype`, default 1). The apply has no lower clamp: a blank
    // cap publishes 0 (1 dedicated). In session, though, the session's
    // creation clamps the cap into 1..65 before the apply runs, so the ceiling
    // never cuts and a blank cap publishes 1 (2 dedicated): session_player_cap,
    // which both hosts run first. The same live count gates BMS placements.
    // The ceiling is the title's: 65, or dfx2.exe's 251 (TitlePlayerCaps).
    // [orig: Game_ApplySessionSettingsToGlobals @0x551b26..0x551b48;
    // CNapiGameSession_BuildAndCreateSession @0x56955D..0x56956C;
    // Config_SetDefaults @0x54d1d4 (networkConnectType_480 = 1);
    // Server_InitNewRoundState @ 0x51c8e0] [orig: @0x54ed93..0x54ed9a (dfx2.exe)]
    const int32_t ceiling = static_cast<int32_t>(title_player_caps(title).session);
    int64_t total = static_cast<int64_t>(player_limit) + (serve_and_play ? 0 : 1);
    if (player_limit > ceiling) total = ceiling;
    // The limit is unsigned: a negative total (no session create clamped the
    // cap) reads as 0.
    return static_cast<uint32_t>(std::max<int64_t>(total, 0));
}

uint32_t GameConfig::player_slot_limit() const {
    return host_player_slot_limit(static_cast<int32_t>(max_players), !dedicated_server, title);
}

int32_t session_player_cap(int32_t player_limit, int title) {
    // [orig: CNapiGameSession_BuildAndCreateSession @0x569567 (0x41)] [orig:
    //  @0x567707 (0xFB) (dfx2.exe)]
    const int32_t session_cap = static_cast<int32_t>(title_player_caps(title).session);
    if (player_limit < 1) return 1;
    return player_limit > session_cap ? session_cap : player_limit;
}

} // namespace opennova::inmatch
