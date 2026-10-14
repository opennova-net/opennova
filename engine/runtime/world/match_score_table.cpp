// The game types' built-in score table: each row's FIELD list and VAR values, which the host
// scores with and S2C 0x58 advertises when no score.ini replaces them, and the EXP_FANFARE pair.
// Each title compiles in its own (docs/titles/dfx2-vs-jo-re.md DFX2-15); each matches the title's
// shipped score.ini.
// [orig: GameType_CreateDefaultSettings @0x52DD00] [orig: GameType_CreateDefaultSettings @0x52ae80
//  (dfx2.exe)]
#include <runtime/world/match.h>

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>

namespace opennova::world {
namespace {

namespace gt = opennova::game_type;

std::vector<MatchScoreField> make_default_score_fields(uint32_t game_type) {
    // Every row installed by GameType_CreateDefaultSettings, including
    // disabled columns and original order. score.ini replaces the whole row.
    // [orig: GameType_CreateDefaultSettings @0x52DD00]
    if (game_type == gt::kDeathmatch) {
        return {{19, 1}, {3, 1}, {4, 1}, {1, 0}, {15, 0},
                {16, 0}, {17, 0}, {27, 0}, {21, 1}};
    }
    if (game_type == gt::kTeamDeathmatch) {
        return {
            {19, 1}, {3, 1},  {2, 0},  {4, 1},  {1, 0},  {30, 1}, {10, 1},
            {11, 0}, {12, 0}, {13, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1},
        };
    }
    if (game_type == gt::kKingOfTheHill) {
        return {{19, 1}, {5, 1}, {22, 1}, {18, 1}, {3, 1}, {4, 1},
                {1, 0}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}};
    }
    if (game_type == gt::kTeamKingOfTheHill) {
        return {{19, 1}, {5, 1}, {22, 1}, {18, 1}, {3, 1}, {2, 0},
                {4, 1}, {1, 0}, {30, 1}, {10, 1}, {11, 0}, {12, 0},
                {13, 1}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}};
    }
    if (game_type == gt::kSearchAndDestroy || game_type == gt::kAttackDefend) {
        return {{19, 1}, {8, 1}, {3, 1}, {2, 0}, {4, 1}, {1, 0}, {30, 1},
                {10, 1}, {11, 0}, {12, 0}, {13, 1}, {15, 0}, {16, 0},
                {17, 0}, {27, 0}, {21, 1}};
    }
    if (game_type == gt::kCaptureTheFlag) {
        return {{19, 1}, {6, 1}, {7, 1}, {14, 1}, {3, 1}, {2, 0}, {4, 1},
                {1, 0}, {30, 1}, {10, 1}, {11, 0}, {12, 0}, {13, 0},
                {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}};
    }
    if (game_type == gt::kFlagBall) {
        return {{19, 1}, {6, 1}, {14, 1}, {29, 1}, {28, 1}, {3, 1}, {2, 0},
                {4, 1}, {1, 0}, {30, 1}, {10, 1}, {11, 0}, {12, 0},
                {13, 0}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}};
    }
    if (game_type == gt::kAdvanceAndSecure) {
        return {
            {19, 1}, {3, 1},  {2, 0},  {4, 1},  {1, 0},  {30, 1}, {10, 1}, {11, 0},
            {32, 1}, {12, 0}, {13, 1}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1},
        };
    }
    if (game_type == gt::kConquerAndControl) {
        return {
            {19, 1}, {6, 1},  {14, 1}, {29, 1}, {28, 1}, {3, 1},  {2, 0},
            {4, 1},  {1, 0},  {30, 1}, {10, 1}, {11, 0}, {32, 1}, {12, 0},
            {13, 1}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1},
        };
    }
    if (gt::is_waypoint_family(game_type)) {
        return {
            {19, 1}, {3, 1}, {4, 1}, {30, 1}, {10, 1}, {11, 1}, {21, 1},
        };
    }
    return {};
}

// Jointops.exe's rows.
// [orig: GameType_CreateDefaultSettings @0x52DD00]
std::array<int32_t, 39> jo_score_values(uint32_t game_type) {
    std::array<int32_t, 39> values{};
    auto common_combat = [&] {
        values[3] = 5;
        values[6] = 1;
        values[7] = 2;
        values[15] = 12;
        values[16] = 10;
        values[17] = 5;
        values[18] = 1;
        values[37] = 5;
    };

    if (game_type == gt::kDeathmatch) {
        values[3] = 5;
        values[16] = 10;
        values[17] = 5;
        values[18] = 1;
        values[37] = 5;
    } else if (game_type == gt::kTeamDeathmatch || gt::is_waypoint_family(game_type)) {
        common_combat();
    } else if (game_type == gt::kKingOfTheHill) {
        values[3] = 2;
        values[12] = 5;
        values[16] = 10;
        values[17] = 5;
        values[18] = 1;
        values[21] = 5;
        values[22] = 5;
        values[33] = 1;
        values[37] = 5;
    } else if (game_type == gt::kTeamKingOfTheHill) {
        values[3] = 2;
        values[6] = 1;
        values[7] = 2;
        values[12] = 10;
        values[15] = 12;
        values[16] = 10;
        values[17] = 5;
        values[18] = 1;
        values[21] = 5;
        values[22] = 5;
        values[33] = 1;
        values[37] = 5;
    } else if (game_type == gt::kSearchAndDestroy || game_type == gt::kAttackDefend) {
        common_combat();
        values[13] = 50;
        values[24] = 2;
        if (game_type == gt::kSearchAndDestroy)
            values[26] = 2;
        else
            values[23] = 1;
    } else if (game_type == gt::kCaptureTheFlag) {
        common_combat();
        values[9] = 10;
        values[10] = 20;
        values[11] = 2;
        values[20] = 5;
    } else if (game_type == gt::kFlagBall) {
        common_combat();
        values[10] = 40;
        values[11] = 2;
        values[20] = 5;
    } else if (game_type == gt::kAdvanceAndSecure ||
               game_type == gt::kConquerAndControl) {
        common_combat();
        values[12] = 5;
        values[24] = 2;
        values[26] = 2;
        values[31] = 1;
        values[34] = 15;
    }
    return values;
}

// dfx2.exe's rows: the same FIELD lists, other VAR values -- ENEMYKILL (3) 2 or 4, SUICIDE (4) -5,
// ALIVE (35) 2, VATTACHKILL (37) 6, and in every row with a capture or an objective
// ALIVEQUANTUM (36) 60 -- and its own Advance and Secure / Conquer and Control split, each row as
// the function stores it.
// [orig: GameType_CreateDefaultSettings @0x52ae80 (dfx2.exe)]
std::array<int32_t, 39> dfx2_score_values(uint32_t game_type) {
    std::array<int32_t, 39> values{};
    // What the capture and objective rows hold beside their own words.
    auto objective_combat = [&] {
        values[3] = 4;
        values[4] = -5;
        values[6] = 1;
        values[7] = 2;
        values[15] = 12;
        values[16] = 10;
        values[17] = 2;
        values[18] = 2;
        values[35] = 2;
        values[36] = 60;
        values[37] = 6;
    };

    if (game_type == gt::kDeathmatch) {
        values[3] = 2;
        values[4] = -5;
        values[16] = 10;
        values[17] = 2;
        values[18] = 2;
        values[35] = 2;
        values[37] = 6;
    } else if (game_type == gt::kTeamDeathmatch || gt::is_waypoint_family(game_type)) {
        values[3] = 2;
        values[4] = -5;
        values[6] = 1;
        values[15] = 12;
        values[16] = 10;
        values[17] = 2;
        values[18] = 2;
        values[35] = 2;
        values[37] = 6;
    } else if (game_type == gt::kKingOfTheHill) {
        values[3] = 4;
        values[4] = -5;
        values[12] = 10;
        values[16] = 10;
        values[17] = 2;
        values[18] = 2;
        values[21] = 2;
        values[22] = 3;
        values[33] = 2;
        values[35] = 2;
        values[36] = 60;
        values[37] = 6;
    } else if (game_type == gt::kTeamKingOfTheHill) {
        objective_combat();
        values[12] = 10;
        values[21] = 2;
        values[22] = 3;
        values[33] = 2;
    } else if (game_type == gt::kSearchAndDestroy || game_type == gt::kAttackDefend) {
        objective_combat();
        values[13] = 50;
        if (game_type == gt::kSearchAndDestroy) {
            values[24] = 2;
            values[26] = 3;
        } else {
            values[23] = 2;
            values[24] = 3;
        }
    } else if (game_type == gt::kCaptureTheFlag) {
        objective_combat();
        values[9] = 10;
        values[10] = 20;
        values[11] = 2;
        values[20] = 5;
    } else if (game_type == gt::kFlagBall) {
        objective_combat();
        values[10] = 40;
        values[11] = 2;
        values[20] = 5;
    } else if (game_type == gt::kAdvanceAndSecure) {
        objective_combat();
        values[12] = 5;
        values[24] = 2;
        values[26] = 2;
        values[31] = 1;
        values[34] = 15;
    } else if (game_type == gt::kConquerAndControl) {
        objective_combat();
        values[12] = 10;
        values[24] = 2;
        values[26] = 3;
        values[31] = 2;
        values[34] = 15;
    }
    return values;
}

} // namespace

std::array<int32_t, 39> default_match_score_values(uint32_t game_type, int title) {
    return title == gameprofile::GAME_DFX2 ? dfx2_score_values(game_type)
                                           : jo_score_values(game_type);
}

// The FIELD lists are the one set in both titles.
std::vector<MatchScoreField> default_match_score_fields(uint32_t game_type) {
    return make_default_score_fields(game_type);
}

uint16_t default_score_exp_fanfare(int title) {
    // [orig: GameType_CreateDefaultSettings @0x52DD00 - 0 @0x52dd30, 0 @0x52dd45 (the
    //  word_24C1170 mirror @0x52dd3e / @0x52dd49)] [orig: @0x52aeb0 (4), @0x52aec5 (10)
    //  (dfx2.exe)]
    if (title == gameprofile::GAME_DFX2) return static_cast<uint16_t>(4u | (10u << 8));
    return 0;
}

} // namespace opennova::world
