// The game types' built-in score table: each row's FIELD list and VAR values, which the host
// scores with and S2C 0x58 advertises when no score.ini replaces them. The FIELD lists are the one
// set; each title compiles in its own VAR values (TitleRules::score_values,
// docs/titles/dfx2-vs-jo-re.md DFX2-15), Jointops.exe's here, each matching the title's shipped
// score.ini.
// [orig: GameType_CreateDefaultSettings @0x52DD00]
#include <runtime/world/match.h>

#include <base/gameprofile/game_type.h>
#include <runtime/world/title_rules.h>

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

} // namespace

// Jointops.exe's rows (title_rules.h).
// [orig: GameType_CreateDefaultSettings @0x52DD00]
std::array<int32_t, 39> builtin_score_values(uint32_t game_type) {
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

std::array<int32_t, 39> default_match_score_values(uint32_t game_type, const TitleRules &rules) {
    return rules.score_values(game_type);
}

// The FIELD lists are the one set in both titles.
std::vector<MatchScoreField> default_match_score_fields(uint32_t game_type) {
    return make_default_score_fields(game_type);
}

} // namespace opennova::world
