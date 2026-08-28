#pragma once

#include <cstdint>

// Runtime-owned retail game-type code words. They are opaque beyond the two
// witnessed bits below. Gameplay can therefore share the exact wire values
// without depending on the network or mission layers.
// [orig: g_GameType @0x24D2128; Server_CheckWinConditions @0x51AD40;
// ScoreRules_GetPrimaryScoreField (ex sub_52C850) @0x52C850]
namespace opennova::game_type {

inline constexpr uint32_t kDeathmatch = 0x00000u;
inline constexpr uint32_t kKingOfTheHill = 0x00001u;
inline constexpr uint32_t kFlagMe = 0x00008u;
inline constexpr uint32_t kTeamDeathmatch = 0x10000u;
inline constexpr uint32_t kTeamKingOfTheHill = 0x10001u;
inline constexpr uint32_t kAttackDefend = 0x10002u;
inline constexpr uint32_t kCaptureTheFlag = 0x10004u;
inline constexpr uint32_t kFlagBall = 0x10008u;
inline constexpr uint32_t kAdvanceAndSecure = 0x10010u;
inline constexpr uint32_t kCoop = 0x10020u;
inline constexpr uint32_t kObjectiveCoop = 0x30020u;
inline constexpr uint32_t kConquerAndControl = 0x50010u;
inline constexpr uint32_t kSearchAndDestroy = 0x90002u;

inline constexpr uint32_t kTeamBit = 0x10000u;
inline constexpr uint32_t kObjectiveBit = 0x20000u;
inline constexpr uint32_t kWaypointFamilyMask = 0xFFFDFFFFu;
inline constexpr uint32_t kWaypointFamilyValue = 0x10020u;

constexpr bool is_team(uint32_t game_type) {
    return (game_type & kTeamBit) != 0;
}

constexpr bool is_objective(uint32_t game_type) {
    return (game_type & kObjectiveBit) != 0;
}

constexpr bool is_waypoint_family(uint32_t game_type) {
    return (game_type & kWaypointFamilyMask) == kWaypointFamilyValue;
}

constexpr bool is_stock_coop(uint32_t game_type) {
    return is_waypoint_family(game_type) && !is_objective(game_type);
}

// Every g_GameType code word Game_StartMission can produce; anything else is
// a typo, not a mode (what the dedicated host's --game-type override is
// validated against). [orig: Game_StartMission @0x524360 type switch]
constexpr bool is_retail_code_word(uint32_t game_type) {
    switch (game_type) {
    case kDeathmatch:
    case kKingOfTheHill:
    case kFlagMe:
    case kTeamDeathmatch:
    case kTeamKingOfTheHill:
    case kAttackDefend:
    case kCaptureTheFlag:
    case kFlagBall:
    case kAdvanceAndSecure:
    case kCoop:
    case kObjectiveCoop:
    case kConquerAndControl:
    case kSearchAndDestroy:
        return true;
    default:
        return false;
    }
}

// The raw retail score-table row selector. Row zero is a real selector result
// that the consumers normalize to Co-op row 2; it is distinct from Deathmatch
// row 11. Flag Me deliberately maps to 12 even though the table has only rows
// 0..11, so its event scorer, FIELD loader, and status-value copy all fail
// closed. [orig: load_scoring_table_for_game_type @0x52D300;
// GameEvent_ProcessScoring @0x52F550; Server_BuildStatusReport @0x530A60]
constexpr uint8_t score_table_index(uint32_t game_type) {
    if (game_type == kDeathmatch)
        return 11;
    if (game_type == kTeamDeathmatch)
        return 1;
    if (is_waypoint_family(game_type) && is_objective(game_type))
        return 2;
    switch (game_type) {
    case kTeamKingOfTheHill:
        return 3;
    case kKingOfTheHill:
        return 4;
    case kSearchAndDestroy:
        return 5;
    case kAttackDefend:
        return 6;
    case kCaptureTheFlag:
        return 7;
    case kFlagBall:
        return 8;
    case kAdvanceAndSecure:
        return 9;
    case kConquerAndControl:
        return 10;
    case kFlagMe:
        return 12;
    default:
        return 0;
    }
}

constexpr bool has_score_table(uint32_t game_type) {
    return score_table_index(game_type) <= 11;
}

// Retail permits four active sides only for the three symmetric team modes.
// Every other team code serializes exactly two sides, regardless of the host's
// mp_numteams setting; solo modes serialize zero. This count is shared by both
// live and end-round scoreboards.
// [orig: Server_BuildAndBroadcastScoreboard @0x50D960;
// Server_BuildEndOfRoundScoreboard @0x508F30]
constexpr uint8_t active_team_count(uint32_t game_type, uint8_t configured) {
    if (!is_team(game_type))
        return 0;
    const bool supports_four =
        game_type == kTeamDeathmatch || game_type == kTeamKingOfTheHill ||
        game_type == kFlagBall;
    return configured == 4 && supports_four ? uint8_t{4} : uint8_t{2};
}

static_assert(kObjectiveCoop == (kCoop | kObjectiveBit));
static_assert(is_waypoint_family(kCoop) && is_waypoint_family(kObjectiveCoop));
static_assert(!is_waypoint_family(kTeamDeathmatch));
static_assert(active_team_count(kTeamDeathmatch, 4) == 4);
static_assert(active_team_count(kAdvanceAndSecure, 4) == 2);
static_assert(active_team_count(kFlagMe, 4) == 0);
static_assert(score_table_index(kDeathmatch) == 11);
static_assert(score_table_index(kCoop) == 0);
static_assert(score_table_index(kObjectiveCoop) == 2);
static_assert(!has_score_table(kFlagMe));
static_assert(is_retail_code_word(kDeathmatch) && is_retail_code_word(kObjectiveCoop));
static_assert(!is_retail_code_word(2) && !is_retail_code_word(0x10003u));

} // namespace opennova::game_type
