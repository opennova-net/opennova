#include <runtime/world/match.h>
#include <base/io/tick_rate.h>
#include <base/io/bam.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <base/gameprofile/game_type.h>
#include <runtime/world/world.h>

namespace opennova::world {
namespace {

namespace gt = opennova::game_type;

constexpr int32_t kTicksPerMinute = 60 * io::kTicksPerSecondInt;
// The objective item ids retail compares the item def's +0x50 type id against.
// Flags: GameEvent_ProcessScoring @0x52F550 routes a capture by the flag's id
// (4091 -> the blue team block @0x52f7e1, 4093 -> red @0x52f7ef, 4095 -> the
// scorer's own team @0x52f7fd) and tests a killed carrier's held flag against
// the same three @0x530262..0x530275; Server_UpdateCaptureZoneProximity
// @0x5086A0 admits them @0x508834..0x508847. Team-owned objectives 4096/4097
// join that admit set @0x508849/@0x508850. Bays: the deploy pick
// SpawnPoint_FindNearestEnemyCapturePoint @0x4DD180 pairs team 1 -> 4098
// @0x4dd1fa, team 2 -> 4100 @0x4dd1ec, team 3 -> 4103 @0x4dd208, team 4 ->
// 4102 @0x4dd216, and Entity_ProcessWaypointInteraction @0x4AD820's id switch
// (jumptable @0x4ad89a) carries the bay cases (4100 @0x4ad9a4, 4103 @0x4ad9d4,
// 4102 @0x4ad9e1). The hill: the proximity pass's pool-3 scan @0x5089e8,
// Entity_SpawnFromBMSRecord @0x40E9F0 @0x40f157, find_max_proximity_coverage
// @0x5BF4D0 @0x5bf511.
constexpr int32_t kBlueFlag = 4091;       // [orig: @0x52f7e1 / @0x50883b]
constexpr int32_t kRedFlag = 4093;        // [orig: @0x52f7ef / @0x508842]
constexpr int32_t kNeutralFlag = 4095;    // [orig: @0x52f7fd / @0x508834]
constexpr int32_t kTeam4Objective = 4096; // [orig: @0x508850 (0x1000)]
constexpr int32_t kTeam3Objective = 4097; // [orig: @0x508849 (0x1001)]
constexpr int32_t kBlueBay = 4098;        // [orig: @0x4dd1fa (0x1002, team 1)]
constexpr int32_t kRedBay = 4100;         // [orig: @0x4dd1ec (0x1004, team 2)]
constexpr int32_t kTeam4Bay = 4102;       // [orig: @0x4dd216 (0x1006, team 4)]
constexpr int32_t kTeam3Bay = 4103;       // [orig: @0x4dd208 (0x1007, team 3)]
constexpr int32_t kHill = 6006;           // [orig: @0x5089e8 (0x1776)]

bool is_flag(int32_t item_id) {
    return item_id == kBlueFlag || item_id == kRedFlag || item_id == kNeutralFlag;
}

bool is_flag_bay(int32_t item_id) {
    return item_id == kBlueBay || item_id == kRedBay ||
           item_id == kTeam4Bay || item_id == kTeam3Bay;
}

// [orig: Server_UpdateCaptureZoneProximity @0x5086A0 — the per-id bit OR into
//  slot+0x15F0C @0x50893d..0x50899b: 4095 -> 1 @0x508944, 4091 -> 2 @0x508957,
//  4093 -> 4 @0x50896a, 4097 -> 8 @0x50897d, 4096 -> 0x10 @0x508994]
uint8_t objective_proximity_bit(int32_t item_id) {
    switch (item_id) {
    case kNeutralFlag:
        return 0x01;
    case kBlueFlag:
        return 0x02;
    case kRedFlag:
        return 0x04;
    case kTeam3Objective:
        return 0x08;
    case kTeam4Objective:
        return 0x10;
    default:
        return 0;
    }
}

bool is_live_player(const Entity *entity) {
    return entity != nullptr && entity->alive &&
           (entity->flags & kEntityFlagDead) == 0;
}

bool within_2d(const Vec3 &a, const Vec3 &b, float radius) {
    if (radius < 0.0f)
        return false;
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    return dx * dx + dy * dy <= static_cast<double>(radius) * radius;
}

int32_t wrap_add(int32_t value, int32_t delta) {
    return static_cast<int32_t>(static_cast<uint32_t>(value) +
                                static_cast<uint32_t>(delta));
}

int32_t unique_best_team(const std::array<int32_t, 5> &scores) {
    int32_t best = std::numeric_limits<int32_t>::min();
    int32_t winner = 0;
    bool tied = false;
    for (uint8_t team = 1; team <= 4; ++team) {
        if (scores[team] > best) {
            best = scores[team];
            winner = team;
            tied = false;
        } else if (scores[team] == best) {
            tied = true;
        }
    }
    return tied ? 0 : winner;
}

bool is_waypoint_family(uint32_t game_type) {
    return gt::is_waypoint_family(game_type);
}

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
    if (is_waypoint_family(game_type)) {
        return {
            {19, 1}, {3, 1}, {4, 1}, {30, 1}, {10, 1}, {11, 1}, {21, 1},
        };
    }
    return {};
}

// The trailing matrix row count of the frozen board: 0 for a non-team type,
// 3 for a team type, 5 for four-team TDM and for Team KOTH / FlagBall at any
// configured team count (rows 3 and 4 of a two-team TKOTH/FlagBall are the
// zero team records). The live 0x16 board's active_team_count is a separate
// rule. [orig: Server_BuildEndOfRoundScoreboard @0x509259..0x50929C (ecx
// 0/3/5), the store @0x5092B8, the trailing count byte @0x509581]
uint8_t scoreboard_team_row_count(const MatchRules &rules) {
    if (!gt::is_team(rules.game_type))
        return 0;
    return (rules.team_count == 4 && rules.game_type == gt::kTeamDeathmatch) ||
                   rules.game_type == gt::kTeamKingOfTheHill ||
                   rules.game_type == gt::kFlagBall
               ? uint8_t{5}
               : uint8_t{3};
}

// Player_ComputeScore's pair-list value: raw points for a team type, the
// game-type primary (ScoreRules_GetPrimaryScoreField) otherwise.
// [orig: Player_ComputeScore @0x500AD0..0x500ADF]
int32_t scoreboard_sort_key(const MatchResultPlayer &row, uint32_t game_type) {
    return gt::is_team(game_type) ? row.stats[MatchStats::kPoints] : row.primary_score;
}

void sort_scoreboard_players(std::vector<MatchResultPlayer> &players, uint32_t game_type) {
    // Server_BuildEndOfRoundScoreboard first scans player slots in numeric
    // order, pairs each with Player_ComputeScore's value, then sorts the
    // pairs with this exact descending Knuth-gap shell sort. Row order is
    // wire-visible through the recipient index in S2C 0x1D and the top three
    // rows of its non-team form.
    // [orig: Server_BuildEndOfRoundScoreboard @0x508F30 (the slot scan: the
    // Player_ComputeScore call @0x509043, the CPairList_AddEntry call
    // @0x50905F, the CPairList_ShellSortByValue call @0x50907F);
    // CPairList_ShellSortByValue @0x526CF0]
    std::sort(players.begin(), players.end(),
              [](const MatchResultPlayer &a, const MatchResultPlayer &b) {
                  return a.identity.slot < b.identity.slot;
              });
    const size_t count = players.size();
    size_t gap = 1;
    while (gap <= count / 9)
        gap = 3 * gap + 1;
    for (; gap > 0; gap /= 3) {
        for (size_t i = gap; i < count; ++i) {
            MatchResultPlayer insert = std::move(players[i]);
            const int32_t key = scoreboard_sort_key(insert, game_type);
            size_t j = i;
            while (j >= gap && scoreboard_sort_key(players[j - gap], game_type) < key) {
                players[j] = std::move(players[j - gap]);
                j -= gap;
            }
            players[j] = std::move(insert);
        }
    }
}

// ItemDef+0x5C == 3, the scorer's person test.
// [orig: GameEvent_ProcessScoring `cmp dword ptr [eax+5Ch], 3` @0x52FFD1]
bool is_person(const Entity &entity) {
    return entity.item_type == 3;
}

} // namespace

std::array<int32_t, 39> default_match_score_values(uint32_t game_type) {
    std::array<int32_t, 39> values{};
    // [orig: GameType_CreateDefaultSettings @0x52DD00]
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
    } else if (game_type == gt::kTeamDeathmatch || is_waypoint_family(game_type)) {
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

std::vector<MatchScoreField> default_match_score_fields(uint32_t game_type) {
    return make_default_score_fields(game_type);
}

int32_t match_score_field_value(const MatchStats &stats, uint8_t field, uint32_t game_type) {
    // This switch is the retail accessor rather than a reimplementation-owned
    // column model. In particular, field 21 is an integer shots-per-kill value;
    // the fixed-point ratio in the seven leading player words is a different
    // producer. [orig: CPlayerStats_GetFieldByIndex @0x52D630]
    switch (field) {
    case 1:
        return stats[6];
    case 2:
        return stats[4];
    case 3:
        return stats[5];
    case 4:
        return stats[7];
    case 5:
        return game_type == gt::kKingOfTheHill ||
                       game_type == gt::kTeamKingOfTheHill
                   ? stats[31]
                   : stats[32] + stats[33];
    case 6:
        return stats[12];
    case 7:
        return stats[11];
    case 8:
        return stats[14];
    case 9:
        return stats[2];
    case 10:
        return stats[9];
    case 11:
        return stats[10];
    case 12:
        return stats[15];
    case 13:
        return stats[16];
    case 14:
        return stats[21];
    case 15:
        return stats[17];
    case 16:
        return stats[18];
    case 17:
        return stats[19];
    case 18:
    case 28:
        return stats[22];
    case 19:
        return stats[29];
    case 20:
        return stats[30];
    case 21:
        return stats[5] == 0 ? -1 : stats[2] / stats[5];
    case 22:
    case 29:
        return stats[23];
    case 23:
        return stats[24];
    case 24:
        return stats[25];
    case 25:
        return stats[26];
    case 26:
        return stats[27];
    case 27:
        return stats[20];
    case 30:
        return stats[28];
    case 32:
        return stats[39];
    default:
        return 0;
    }
}

void Match::configure(const MatchRules &rules) {
    rules_ = rules;
    if (rules_.score_fields.empty())
        rules_.score_fields = default_match_score_fields(rules_.game_type);
    if (!rules_.score_values.has_value())
        rules_.score_values = default_match_score_values(rules_.game_type);
    players_.clear();
    player_punts_.clear();
    teams_ = {};
    team_hold_ticks_ = {};
    periodic_second_timer_ = 0;
    periodic_second_fired_ = false;
    outcome_ = {};
    result_ = {};
    objective_census_ready_ = false;
    flag_capture_targets_ = {};
    demolition_targets_ = {};
    carry_objectives_.clear();
    gameplay_events_.clear();
    // Game_StartMission starts at -1 and seeds GameTime only for a network
    // session outside the Co-op waypoint family, and only when nonzero.
    // [orig: g_round_time_remaining=-1 @0x524A89; seed
    // 3720*g_respawn_time @0x525242..0x525251; decrement @0x5266D6]
    if (rules.game_time_minutes != 0 && !is_waypoint_family(rules.game_type)) {
        const uint64_t ticks = uint64_t(rules.game_time_minutes) * kTicksPerMinute;
        remaining_ticks_ = ticks > uint64_t(std::numeric_limits<int32_t>::max())
                               ? std::numeric_limits<int32_t>::max()
                               : static_cast<int32_t>(ticks);
    } else {
        remaining_ticks_ = -1;
    }
}

void Match::upsert_player(const MatchPlayerIdentity &identity) {
    auto by_entity = std::find_if(players_.begin(), players_.end(), [&](const MatchPlayer &p) {
        return p.identity.entity == identity.entity;
    });
    if (by_entity != players_.end()) {
        by_entity->identity = identity;
        return;
    }
    // A retail roster slot has one live owner. Rebinding it starts a fresh stats
    // record rather than leaving a disconnected entity's counters addressable.
    auto by_slot = std::find_if(players_.begin(), players_.end(), [&](const MatchPlayer &p) {
        return p.identity.slot == identity.slot;
    });
    if (by_slot != players_.end()) {
        *by_slot = MatchPlayer{identity, {}, 0};
        return;
    }
    players_.push_back(MatchPlayer{identity, {}, 0});
}

void Match::remove_player(World &world, EntityHandle entity) {
    drop_carried_object(world, entity);
    players_.erase(
        std::remove_if(players_.begin(), players_.end(),
                       [&](const MatchPlayer &p) { return p.identity.entity == entity; }),
        players_.end());
}

void Match::set_player_spectator(EntityHandle entity, bool spectator) {
    if (MatchPlayer *row = player(entity))
        row->spectator = spectator;
}

const MatchPlayer *Match::player(EntityHandle entity) const {
    const auto it = std::find_if(players_.begin(), players_.end(),
                                 [&](const MatchPlayer &p) { return p.identity.entity == entity; });
    return it == players_.end() ? nullptr : &*it;
}

MatchPlayer *Match::player(EntityHandle entity) {
    const auto it = std::find_if(players_.begin(), players_.end(),
                                 [&](const MatchPlayer &p) { return p.identity.entity == entity; });
    return it == players_.end() ? nullptr : &*it;
}

const MatchStats &Match::team_stats(uint8_t team) const {
    static const MatchStats empty;
    return team < teams_.size() ? teams_[team] : empty;
}

// This raw stats event deliberately bypasses score.ini and team scoring.
// Positive awards recurse through the first AND second occupant links. The
// second link can consequently receive both the nested half and direct quarter.
// Zero recursive amounts still allow the caller's event-27 counter increment.
// [orig: CPlayerStats_RecordEvent @0x52CAF8..0x52CBB3, event 28]
void Match::share_experience(const World &world, MatchPlayer &recipient, int32_t amount) {
    if (amount == 0) return;
    recipient.stats[MatchStats::kPoints] =
            wrap_add(recipient.stats[MatchStats::kPoints], amount);
    const Entity *entity = world.registry.get(recipient.identity.entity);
    if (amount <= 0 || entity == nullptr) return;
    const Entity *first = world.registry.get(entity->primary_occupant);
    if (first == nullptr) return;
    const auto award = [&](EntityHandle handle, int32_t share) {
        if (MatchPlayer *linked = player(handle)) {
            share_experience(world, *linked, share);
            linked->stats[MatchStats::kSharedPointAwards] =
                    wrap_add(linked->stats[MatchStats::kSharedPointAwards], 1);
        }
    };
    award(first->handle, amount >> 1);
    if (const Entity *second = world.registry.get(first->primary_occupant))
        award(second->handle, amount >> 2);
}

// [orig: WacCmd_AddExp @0x4F2690]
bool Match::add_experience(const World &world, EntityHandle handle, int32_t amount) {
    const Entity *entity = world.registry.get(handle);
    MatchPlayer *recipient = player(handle);
    if (entity == nullptr || !entity->has_item_def || amount == 0 || recipient == nullptr)
        return false;
    share_experience(world, *recipient, amount);
    return true;
}

// Handler success means a registered player, including a dead player or the
// local loopback. The connection owner applies its own disconnect gates.
// [orig: WacCmd_PlayerPunt @0x4F0DA0; WacCmd_PlayerKillPunt @0x4F0D30;
// CNapiNPConnection_TrySendChatMessage @0x5006C0]
bool Match::request_player_punt(const World &world, EntityHandle handle, bool kill_punt) {
    const Entity *entity = world.registry.get(handle);
    if (entity == nullptr || player(handle) == nullptr) return false;
    for (const MatchPlayerPunt &pending : player_punts_)
        if (pending.entity == handle && pending.spawn_id == entity->registry_spawn_id)
            return true; // the first pending disconnect event wins
    player_punts_.push_back({handle, entity->registry_spawn_id,
                             uint8_t(kill_punt ? 49 : 33)});
    return true;
}

std::vector<MatchPlayerPunt> Match::drain_player_punts() {
    std::vector<MatchPlayerPunt> result;
    result.swap(player_punts_);
    return result;
}

int32_t Match::flag_capture_target(const World &world, uint8_t scoring_team) {
    ensure_objective_census(world);
    return scoring_team < flag_capture_targets_.size()
               ? flag_capture_targets_[scoring_team]
               : 0;
}

int32_t Match::demolition_target(const World &world, uint8_t scoring_team) {
    ensure_objective_census(world);
    return scoring_team < demolition_targets_.size()
               ? demolition_targets_[scoring_team]
               : 0;
}

int32_t Match::score_value(size_t status_index) const {
    return rules_.score_values.has_value() && status_index < rules_.score_values->size()
               ? (*rules_.score_values)[status_index]
               : 0;
}

// Every scorer points award is RecordEvent 28 with param2 = 0, so a positive
// amount also shares with the Player's +0x170 links (half, then a quarter).
// Only event 25 passes param2 = 1 and keeps its award unshared.
// [orig: GameEvent_ProcessScoring — case 25's `push 1` @0x530989 is the one
//  nonzero param2; CPlayerStats_RecordEvent case 28 @0x52CAF8..0x52CBB3]
void Match::add_event(const World &world, MatchPlayer &player, size_t counter,
                      int32_t points, int32_t raw_delta, bool share) {
    if (!gt::has_score_table(rules_.game_type))
        return;
    player.stats[counter] = wrap_add(player.stats[counter], raw_delta);
    if (share)
        share_experience(world, player, points);
    else
        player.stats[MatchStats::kPoints] =
            wrap_add(player.stats[MatchStats::kPoints], points);
}

void Match::add_points(const World &world, MatchPlayer &player, int32_t points) {
    if (!gt::has_score_table(rules_.game_type))
        return;
    share_experience(world, player, points);
}

void Match::add_team_points(uint8_t team, int32_t points) {
    if ((rules_.game_type & 0x10000u) == 0 || !gt::has_score_table(rules_.game_type) ||
        team >= teams_.size())
        return;
    teams_[team][MatchStats::kPoints] =
        wrap_add(teams_[team][MatchStats::kPoints], points);
}

// The scorer resolves the actor's TeamRecords row only when g_GameType carries
// the team bit; every team leg tests that pointer. A non-team mode therefore
// never touches a team row, although its Players all sit on team 1.
// [orig: GameEvent_ProcessScoring @0x52F657 `test ecx, 10000h; jz`, the
// other-entity twin @0x52F6A3; Server_AssignPlayerTeam @0x4FE3EC]
void Match::add_team_event(uint8_t team, size_t counter, int32_t points,
                           int32_t raw_delta) {
    if ((rules_.game_type & 0x10000u) == 0)
        return;
    add_team_record_event(team, counter, points, raw_delta);
}

void Match::add_team_record_event(uint8_t team, size_t counter, int32_t points,
                                  int32_t raw_delta) {
    if (!gt::has_score_table(rules_.game_type) || team >= teams_.size())
        return;
    teams_[team][counter] = wrap_add(teams_[team][counter], raw_delta);
    teams_[team][MatchStats::kPoints] =
        wrap_add(teams_[team][MatchStats::kPoints], points);
}

void Match::ensure_objective_census(const World &world) {
    if (objective_census_ready_)
        return;
    flag_capture_targets_ = {};
    demolition_targets_ = {};
    world.registry.for_each([&](const Entity &entity) {
        // CTF's two target globals count the authored opposing flags once at
        // round start. Capturing one removes its entity but not the target.
        // [orig: reset_round_counters @0x516C50; CTF arm @0x51B0F0]
        if (entity.item_id == kRedFlag)
            ++flag_capture_targets_[1];
        else if (entity.item_id == kBlueFlag)
            ++flag_capture_targets_[2];

        // S&D/AD count item-attrib 0x8000 targets by defending team; the
        // opposite team must destroy that complete authored census.
        // [orig: reset_round_counters @0x516C50; win arm @0x51B18B]
        if ((entity.item_attrib & kItemAttribObjectiveTarget) != 0) {
            if (entity.team == 1)
                ++demolition_targets_[2];
            else if (entity.team == 2)
                ++demolition_targets_[1];
        }
    });
    objective_census_ready_ = true;
}

Match::CarryObjectiveState *Match::carry_state(World &world, EntityHandle objective) {
    Entity *entity = world.registry.get(objective);
    if (entity == nullptr)
        return nullptr;
    auto found = std::find_if(carry_objectives_.begin(), carry_objectives_.end(),
                              [&](const CarryObjectiveState &state) {
                                  return state.objective == objective &&
                                         state.spawn_id == entity->registry_spawn_id;
                              });
    if (found != carry_objectives_.end())
        return &*found;
    carry_objectives_.erase(
        std::remove_if(carry_objectives_.begin(), carry_objectives_.end(),
                       [&](const CarryObjectiveState &state) {
                           return state.objective == objective;
                       }),
        carry_objectives_.end());
    carry_objectives_.push_back(
        {objective, entity->registry_spawn_id, entity->spawn_position, 0});
    return &carry_objectives_.back();
}

void Match::record_flag_pickup(World &world, EntityHandle player_handle,
                               EntityHandle flag_handle) {
    if (outcome_.ended)
        return;
    MatchPlayer *scorer = player(player_handle);
    Entity *carrier = world.registry.get(player_handle);
    Entity *flag = world.registry.get(flag_handle);
    CarryObjectiveState *state = carry_state(world, flag_handle);
    if (scorer == nullptr || carrier == nullptr || flag == nullptr || state == nullptr ||
        carrier->mounted_child.valid() || flag->primary_occupant.valid())
        return;

    carrier->mounted_child = flag_handle;
    flag->primary_occupant = player_handle;
    flag->flags |= kEntityFlagCarried;
    state->return_ticks = static_cast<int32_t>(rules_.flag_return_ticks);
    add_event(world, *scorer, MatchStats::kFlagPickups, score_value(11));
    add_team_event(carrier->team, MatchStats::kFlagPickups, score_value(11));
    gameplay_events_.push_back({MatchGameplayEventKind::FlagPickup,
                                player_handle,
                                flag_handle,
                                carrier->position,
                                flag->position,
                                static_cast<uint8_t>(flag->flags),
                                player_handle,
                                flag->ground_target,
                                false,
                                static_cast<uint16_t>(flag->item_id)});
}

void Match::return_flag_home(World &world, EntityHandle flag_handle,
                             MatchGameplayEventKind kind, EntityHandle actor) {
    CarryObjectiveState *state = carry_state(world, flag_handle);
    Entity *flag = world.registry.get(flag_handle);
    if (state == nullptr || flag == nullptr)
        return;
    const Vec3 event_position = flag->position;
    if (Entity *carrier = world.registry.get(flag->primary_occupant);
        carrier != nullptr && carrier->mounted_child == flag_handle)
        carrier->mounted_child = EntityHandle{};
    flag->primary_occupant = EntityHandle{};
    flag->ground_target = EntityHandle{};
    flag->flags &= ~kEntityFlagCarried;
    flag->position = state->home;
    flag->alive = true;
    state->return_ticks = 0;
    gameplay_events_.push_back({kind,
                                actor,
                                flag_handle,
                                event_position,
                                flag->position,
                                static_cast<uint8_t>(flag->flags),
                                EntityHandle{},
                                flag->ground_target,
                                false,
                                static_cast<uint16_t>(flag->item_id)});
}

void Match::record_flag_save(World &world, EntityHandle player_handle,
                             EntityHandle flag_handle) {
    MatchPlayer *scorer = player(player_handle);
    const Entity *entity = world.registry.get(player_handle);
    if (outcome_.ended || scorer == nullptr || entity == nullptr)
        return;
    return_flag_home(world, flag_handle, MatchGameplayEventKind::FlagSave,
                     player_handle);
    // [orig: GameEvent_ProcessScoring @0x52F550 case 8 @0x52f8d1..0x52f992 —
    //  ++stats[11] (slot dword 29) @0x52f8d7, points += scoringTable[83]
    //  (score.ini FLAGSAVE, VAR slot 9) @0x52f8ec, the team mirror
    //  ++team[28] (= team field 11) @0x52f988 with the same award @0x52f98c;
    //  dispatched by Server_BroadcastEntityDeathEvent @0x517A90 (push 8
    //  @0x517b03) from Entity_ProcessWaypointInteraction @0x4AD820]
    add_event(world, *scorer, MatchStats::kFlagSaves, score_value(9));
    add_team_event(entity->team, MatchStats::kFlagSaves, score_value(9));
}

void Match::record_flag_capture(World &world, EntityHandle player_handle,
                                EntityHandle flag_handle) {
    if (outcome_.ended)
        return;
    ensure_objective_census(world);
    MatchPlayer *scorer = player(player_handle);
    Entity *carrier = world.registry.get(player_handle);
    if (scorer == nullptr || carrier == nullptr)
        return;
    Entity *flag = world.registry.get(flag_handle);
    add_event(world, *scorer, MatchStats::kFlagCaptures, score_value(10));
    if (flag == nullptr)
        return;
    // The team leg is routed by the FLAG TYPE in every game type: a captured
    // blue flag names TeamRecords[2] and a red one TeamRecords[1] directly,
    // with no team-bit test; only the neutral flag uses the capturer's own
    // (team-mode) row, and any other objective adds no team award. These rows
    // are also CTF's two capture counters.
    // [orig: GameEvent_ProcessScoring case 9 @0x52F7CF..0x52F810 — 4091 ->
    //  TeamRecords[2] @0x52F7E8, 4093 -> TeamRecords[1] @0x52F7F6, 4095 ->
    //  the esi row @0x52F808; Server_CheckWinConditions @0x51B0F0]
    if (flag->item_id == kBlueFlag)
        add_team_record_event(2, MatchStats::kFlagCaptures, score_value(10));
    else if (flag->item_id == kRedFlag)
        add_team_record_event(1, MatchStats::kFlagCaptures, score_value(10));
    else if (flag->item_id == kNeutralFlag)
        add_team_event(carrier->team, MatchStats::kFlagCaptures, score_value(10));
    const Vec3 capture_position = flag->position;
    const uint8_t flags_before = static_cast<uint8_t>(flag->flags);
    carrier->mounted_child = EntityHandle{};
    flag->primary_occupant = EntityHandle{};
    const bool remove = rules_.game_type == gt::kCaptureTheFlag;
    MatchGameplayEvent event{MatchGameplayEventKind::FlagCapture,
                             player_handle,
                             flag_handle,
                             capture_position,
                             capture_position,
                             flags_before,
                             EntityHandle{},
                             flag->ground_target,
                             remove,
                             static_cast<uint16_t>(flag->item_id)};
    if (remove) {
        world.registry.despawn(flag_handle);
    } else {
        CarryObjectiveState *state = carry_state(world, flag_handle);
        if (state != nullptr) {
            flag = world.registry.get(flag_handle);
            flag->flags &= ~kEntityFlagCarried;
            flag->position = state->home;
            flag->ground_target = EntityHandle{};
            state->return_ticks = 0;
            event.objective_position = flag->position;
            event.objective_flags = static_cast<uint8_t>(flag->flags);
            event.ground = flag->ground_target;
        }
    }
    gameplay_events_.push_back(event);
}

void Match::record_target_destroyed(const World &world, EntityHandle target_handle,
                                    EntityHandle attacker_handle) {
    if (outcome_.ended)
        return;
    const Entity *target = world.registry.get(target_handle);
    const Entity *attacker_entity = world.registry.get(attacker_handle);
    MatchPlayer *attacker = player(attacker_handle);
    if (target == nullptr || attacker_entity == nullptr ||
        (target->item_attrib & kItemAttribObjectiveTarget) == 0)
        return;
    // [orig: GameEvent_ProcessScoring @0x52F550, event 11 @0x52fa61..0x52fb0e:
    //  ++stats[14] @0x52fa67, points += table[87] @0x52fa7f, then
    //  RecordEvent(28, bonus>>1) @0x52faba and RecordEvent(28, bonus>>2)
    //  @0x52faff on the first and second occupant links, each with an
    //  event-27 count @0x52facb/@0x52fb0e. Event 28 recurses through both
    //  links itself (CPlayerStats_RecordEvent @0x52caf8..0x52cbb3), which is
    //  exactly share_experience: the second link takes the nested
    //  (bonus>>1)>>1 AND the direct bonus>>2. The scoring head's table gate
    //  @0x52f627 covers the whole event, the occupant awards included.]
    const int32_t bonus = score_value(13);
    if (attacker && gt::has_score_table(rules_.game_type)) {
        add_event(world, *attacker, MatchStats::kTargetsDestroyed, 0);
        share_experience(world, *attacker, bonus);
    }
    if ((rules_.game_type & 0x10000u) != 0)
        add_team_event(attacker_entity->team, MatchStats::kTargetsDestroyed, bonus);
}

void Match::drop_carried_object(World &world, EntityHandle player_handle) {
    Entity *carrier = world.registry.get(player_handle);
    if (carrier == nullptr || !carrier->mounted_child.valid())
        return;
    const EntityHandle flag_handle = carrier->mounted_child;
    Entity *flag = world.registry.get(flag_handle);
    CarryObjectiveState *state = carry_state(world, flag_handle);
    carrier->mounted_child = EntityHandle{};
    if (flag == nullptr || state == nullptr)
        return;
    flag->primary_occupant = EntityHandle{};
    flag->flags &= ~kEntityFlagCarried;
    flag->position = carrier->position;
    flag->alive = true;
    // The dropped-flag service re-arms the return window whenever the flag is
    // not idle: 210 when the configured time is below 5, else the configured
    // seconds. [orig: Entity_UpdateIdleCheck @0x408531..0x40853B]
    state->return_ticks = rules_.flag_return_ticks < 5
                              ? 210
                              : static_cast<int32_t>(rules_.flag_return_ticks);
    gameplay_events_.push_back({MatchGameplayEventKind::FlagDrop,
                                player_handle,
                                flag_handle,
                                flag->position,
                                flag->position,
                                static_cast<uint8_t>(flag->flags),
                                EntityHandle{},
                                flag->ground_target,
                                false,
                                static_cast<uint16_t>(flag->item_id)});
}

bool Match::sync_flag_to_authored_pose(World &world, EntityHandle flag_handle) {
    // [orig: Entity_SyncPositionFromDefinition @0x43A9B0]. The authored pose is
    // the carry state's home (aiRuntime f0_7[4..6]); the yaw term of retail's
    // equality test (f0_7[7]) never differs here because nothing in this sim
    // rotates a flag, so position alone decides it.
    Entity *flag = world.registry.get(flag_handle);
    CarryObjectiveState *state = carry_state(world, flag_handle);
    if (flag == nullptr || state == nullptr)
        return false;
    const int32_t x = to_fixed(flag->position.x);
    const int32_t y = to_fixed(flag->position.y);
    const int32_t z = to_fixed(flag->position.z);
    const int32_t home_x = to_fixed(state->home.x);
    const int32_t home_y = to_fixed(state->home.y);
    const int32_t home_z = to_fixed(state->home.z);
    if (x == home_x && y == home_y && z == home_z)
        return false; // [orig: @0x43a9f8 — already at the definition pose]
    const int64_t dx = int64_t{x} - home_x;
    const int64_t dy = int64_t{y} - home_y;
    const int64_t dz = int64_t{z} - home_z;
    const bool near_home =
        static_cast<int64_t>(std::sqrt(static_cast<long double>(dx * dx + dy * dy))) < 0x20000 &&
        (dz < 0 ? -dz : dz) < 0x20000; // [orig: @0x43aa3b / @0x43aa4d]
    if (!near_home) {
        // The snap: position back to the definition pose; the ground link is
        // re-resolved by a downward raycast in retail (@0x43ab22) — this port
        // clears it like the timeout return does, no raycast seam here.
        flag->position = state->home;                 // [orig: @0x43aa7d..0x43aa96]
        flag->ground_target = EntityHandle{};
    }
    // Both paths publish the 19-B 0x2F state and nothing else (no 0x1E, no
    // scoring): the FlagDrop-shaped record is the feed-less lane.
    // [orig: Entity_SyncPositionFromDefinition @0x43A9B0 (the
    // Server_SendDestructibleDeathPacket calls @0x43AA6B / @0x43AB40)]
    gameplay_events_.push_back({MatchGameplayEventKind::FlagDrop,
                                EntityHandle{},
                                flag_handle,
                                flag->position,
                                flag->position,
                                static_cast<uint8_t>(flag->flags),
                                flag->primary_occupant,
                                flag->ground_target,
                                false,
                                static_cast<uint16_t>(flag->item_id)});
    return !near_home;
}

void Match::record_death(World &world, EntityHandle victim_handle,
                         EntityHandle killer_handle, uint32_t cause_flags) {
    // A Player's death first unlinks every pool-0 AI body whose +0x170
    // point-share link names it. [orig: GameEvent_PlayerDeath
    // @0x516E07..0x516E45]
    if (const Entity *dead = world.registry.get(victim_handle);
            dead != nullptr &&
            ((dead->flags | dead->engine_flags) & kEntityFlagPlayer) != 0) {
        world.registry.for_each_in_pool(0, [&](const Entity &row) {
            if (row.handle == victim_handle || row.primary_occupant != victim_handle ||
                    world.ai.for_handle(row.handle) == nullptr)
                return;
            world.registry.get(row.handle)->primary_occupant = EntityHandle{};
        });
    }
    if (outcome_.ended)
        return;
    // The kill scorer reads the victim's carried-object link (+0x268) as the
    // death finds it, before the drop clears it.
    // [orig: GameEvent_ProcessScoring @0x530246..0x530277]
    bool victim_carried_flag = false;
    if (const Entity *dead = world.registry.get(victim_handle))
        if (const Entity *carried = world.registry.get(dead->mounted_child))
            victim_carried_flag = carried->has_item_def && is_flag(carried->item_id);
    drop_carried_object(world, victim_handle);
    const Entity *victim_entity = world.registry.get(victim_handle);
    if (victim_entity == nullptr)
        return;
    if ((victim_entity->item_attrib & kItemAttribObjectiveTarget) != 0 &&
            !victim_entity->objective_death_scored)
        record_target_destroyed(world, victim_handle, killer_handle);

    // A Player's death runs the victim-only scorer call, then the
    // killer-victim call; a non-Player person's death edge runs the
    // killer-victim call alone. Nothing else reaches scorer event 3.
    // [orig: GameEvent_PlayerDeath @0x516F06 / @0x516FB0;
    // Entity_CheckAndProcessDeath `test dword ptr [esi+24h], 100h` @0x51B555,
    // the non-Player call @0x51B5B3]
    if (player(victim_handle) != nullptr) {
        score_death(world, victim_handle);
        score_kill(world, killer_handle, victim_handle, cause_flags,
                   victim_carried_flag);
    } else if (is_person(*victim_entity)) {
        score_kill(world, killer_handle, victim_handle, cause_flags, false);
    }
}

// Scorer event 3 without an other-entity: the Player's own death, RecordEvent
// 6 (Deaths) and DEATH points on its slot and, in team modes, its team row.
// The scorer head refuses a spectator-latched slot.
// [orig: GameEvent_ProcessScoring @0x52F550 — spectator test @0x52F6E5,
//  case 3 @0x52FFBB `test edx, edx; jz` -> @0x53074E..0x5307A7]
void Match::score_death(World &world, EntityHandle victim_handle) {
    MatchPlayer *victim = player(victim_handle);
    const Entity *victim_entity = world.registry.get(victim_handle);
    if (victim == nullptr || victim_entity == nullptr || victim->spectator)
        return;
    add_event(world, *victim, MatchStats::kDeaths, score_value(5));
    add_team_event(victim_entity->team, MatchStats::kDeaths, score_value(5));
}

// Scorer event 3 with an other-entity (the victim).
// [orig: GameEvent_ProcessScoring @0x52F550 case 3 @0x52FF43]
void Match::score_kill(World &world, EntityHandle killer_handle,
                       EntityHandle victim_handle, uint32_t cause_flags,
                       bool victim_carried_flag) {
    // The category gate precedes everything, then the head refuses a
    // spectator-latched slot on either side.
    // [orig: @0x52F617..0x52F640; @0x52F6E1..0x52F701]
    if (!gt::has_score_table(rules_.game_type))
        return;
    MatchPlayer *killer = player(killer_handle);
    MatchPlayer *victim = player(victim_handle);
    if ((killer != nullptr && killer->spectator) ||
            (victim != nullptr && victim->spectator))
        return;
    const Entity *killer_entity = world.registry.get(killer_handle);
    if (killer == nullptr) {
        // A non-Player killer (an NPC, a vehicle, an item) credits the Player
        // on its +0x170 link with half the ENEMYKILL value, a LOGICAL shift,
        // as a shared-points award.
        // [orig: @0x52FF43 `test edi, edi; jnz` -> @0x52FF47..0x52FF79
        //  (`shr edx, 1` @0x52FF76) -> RecordEvent 28 / 27 @0x52F8AA..0x52F8C0]
        if (killer_entity == nullptr)
            return;
        MatchPlayer *link = player(killer_entity->primary_occupant);
        if (link == nullptr)
            return;
        share_experience(world, *link,
                         static_cast<int32_t>(static_cast<uint32_t>(score_value(3)) >> 1));
        link->stats[MatchStats::kSharedPointAwards] =
            wrap_add(link->stats[MatchStats::kSharedPointAwards], 1);
        return;
    }
    const Entity *victim_entity = world.registry.get(victim_handle);
    if (killer_entity == nullptr || victim_entity == nullptr)
        return;
    const uint8_t killer_team = killer_entity->team;
    const uint8_t victim_team = victim_entity->team;
    const bool team_mode = (rules_.game_type & 0x10000u) != 0;

    // The victim's entity+0x2C kill-cause counters, each with its own points
    // and team mirror, on both the person and the Player arm.
    // [orig: 0x100 -> RecordEvent 16 + MULTIPLEKILL, 0x800 -> 17 +
    //  HEADSHOTKILL, 0x400 -> 18 + KNIFEKILL: @0x530076..0x530178 (person),
    //  @0x530357..0x53046F (Player)]
    auto cause_bonuses = [&]() {
        if ((cause_flags & 0x100u) != 0) {
            add_event(world, *killer, MatchStats::kMultipleKills, score_value(16));
            add_team_event(killer_team, MatchStats::kMultipleKills, score_value(16));
        }
        if ((cause_flags & 0x800u) != 0) {
            add_event(world, *killer, MatchStats::kHeadshotKills, score_value(17));
            add_team_event(killer_team, MatchStats::kHeadshotKills, score_value(17));
        }
        if ((cause_flags & 0x400u) != 0) {
            add_event(world, *killer, MatchStats::kKnifeKills, score_value(18));
            add_team_event(killer_team, MatchStats::kKnifeKills, score_value(18));
        }
    };

    if (victim == nullptr) {
        // A non-Player person: only an enemy of a different nonzero team
        // counts, as an ENEMYKILL plus the cause bonuses.
        // [orig: @0x52FFC3 `cmp [esp+var_C], 0; jnz` -> team tests
        //  @0x52FFDB..0x52FFF9, RecordEvent 4 + 28 @0x530027 / @0x530039, the
        //  team mirror @0x53005E / @0x530071, cause legs @0x530076..0x530178]
        if (killer_team == 0 || victim_team == 0 || killer_team == victim_team)
            return;
        add_event(world, *killer, MatchStats::kEnemyKills, score_value(3));
        add_team_event(killer_team, MatchStats::kEnemyKills, score_value(3));
        cause_bonuses();
        return;
    }

    if (killer_handle == victim_handle) {
        // Suicide: RecordEvent 5 + SUICIDE. [orig: `cmp ebx, edx` @0x530182,
        // @0x530186..0x5301D5]
        add_event(world, *killer, MatchStats::kSuicides, score_value(4));
        add_team_event(killer_team, MatchStats::kSuicides, score_value(4));
        return;
    }
    if (team_mode && killer_team == victim_team) {
        // A team kill is the two actors resolving the SAME TeamRecords row,
        // which exists only in team modes (team 0 included). There is no
        // see-all exemption in the scorer: that test belongs to the kill feed.
        // [orig: @0x5301E4..0x5301EE `test esi, esi` / `test eax, eax` /
        //  `cmp esi, eax`; RecordEvent 3 + FRIENDLYKILL @0x5301FD..0x530237;
        //  feed-only see-all @0x5170A6..0x517113]
        add_event(world, *killer, MatchStats::kTeamKills, score_value(2));
        add_team_event(killer_team, MatchStats::kTeamKills, score_value(2));
        return;
    }

    // Enemy Player kill: RecordEvent 4 + ENEMYKILL.
    // [orig: @0x530246..0x5302F9]
    add_event(world, *killer, MatchStats::kEnemyKills, score_value(3));
    add_team_event(killer_team, MatchStats::kEnemyKills, score_value(3));

    // A victim carrying a flag adds FLAGCARRIERKILL, then the cause bits, then
    // a sniper-class victim's ENEMYSNIPERKILL points (no counter), each with
    // its team mirror, all before the zone bonuses.
    // [orig: the carried-flag test @0x530246..0x530277, RecordEvent 20 +
    //  FLAGCARRIERKILL @0x5302FE..0x530352; the victim class-6 points
    //  @0x530474..0x5304AB]
    if (victim_carried_flag) {
        add_event(world, *killer, MatchStats::kFlagCarrierKills, score_value(20));
        add_team_event(killer_team, MatchStats::kFlagCarrierKills, score_value(20));
    }
    cause_bonuses();
    if (victim_entity->player_class == 6) {
        add_points(world, *killer, score_value(28));
        add_team_points(killer_team, score_value(28));
    }

    // The six objective-proximity bonuses are independent tests and may
    // all fire for one kill. Bit 0 works in solo and team modes; the four
    // team-relative tests require the team stats rows retail materializes
    // only when g_GameType carries 0x10000.
    // [orig: @0x5304B0..0x530744]
    const uint8_t attacker_mask = killer->objective_proximity_mask;
    const uint8_t victim_mask = victim->objective_proximity_mask;
    auto bonus = [&](size_t counter, size_t score_index) {
        add_event(world, *killer, counter, score_value(score_index));
        add_team_event(killer_team, counter, score_value(score_index));
    };
    if ((victim_mask & 0x01u) != 0)
        bonus(MatchStats::kVictimNearNeutralObjectiveKills, 21);
    if ((attacker_mask & 0x01u) != 0)
        bonus(MatchStats::kAttackerNearNeutralObjectiveKills, 22);
    if (team_mode && attacker_mask > 1u && killer_team < 8u &&
        (attacker_mask & static_cast<uint8_t>(1u << killer_team)) != 0)
        bonus(MatchStats::kAttackerNearOwnObjectiveKills, 24);
    if (team_mode && victim_mask > 1u && victim_team < 8u &&
        (victim_mask & static_cast<uint8_t>(1u << victim_team)) != 0)
        bonus(MatchStats::kVictimNearOwnObjectiveKills, 25);
    if (team_mode && attacker_mask > 1u && victim_team < 8u &&
        (attacker_mask & static_cast<uint8_t>(1u << victim_team)) != 0)
        bonus(MatchStats::kAttackerNearVictimObjectiveKills, 26);
    if (team_mode && victim_mask > 1u && killer_team < 8u &&
        (victim_mask & static_cast<uint8_t>(1u << killer_team)) != 0)
        bonus(MatchStats::kVictimNearAttackerObjectiveKills, 23);
}

// Scorer event 12: the victim's signed `score` word goes to field 30 through
// RecordEvent 29 (no points) on the killer's slot, and on the killer's team
// row whenever the team bit resolves one, Player or not.
// [orig: Score_ProcessKillEvent @0x4FD400 (the event-12 call @0x4FD438);
//  GameEvent_ProcessScoring case 12 @0x52FEC2..0x52FF0F]
void Match::record_kill_event(const World &world, EntityHandle killer_handle,
                              EntityHandle victim_handle) {
    if (!gt::has_score_table(rules_.game_type))
        return;
    const Entity *killer_entity = world.registry.get(killer_handle);
    const Entity *victim_entity = world.registry.get(victim_handle);
    MatchPlayer *killer = player(killer_handle);
    MatchPlayer *victim = player(victim_handle);
    // [orig: the scorer head's spectator refusals @0x52F6E1..0x52F701]
    if ((killer != nullptr && killer->spectator) ||
        (victim != nullptr && victim->spectator))
        return;
    // [orig: `test edx, edx` / the def test @0x52FEC2..0x52FECF]
    if (killer_entity == nullptr || victim_entity == nullptr || !victim_entity->has_item_def)
        return;
    const int32_t score = victim_entity->item_score;
    if (killer != nullptr)
        add_event(world, *killer, MatchStats::kUnitScore, 0, score);
    add_team_event(killer_entity->team, MatchStats::kUnitScore, 0, score);
}

// Scorer event 1: one shot (RecordEvent 1) plus the FIRE value on the
// shooter's slot, with the team-mode mirror.
// [orig: Server_ClientFiredRound @0x50BAA0 (the event-1 call @0x50C727);
//  GameEvent_ProcessScoring case 1 @0x52FB2A..0x52FC13]
void Match::record_shot(const World &world, EntityHandle shooter_handle) {
    const Entity *shooter_entity = world.registry.get(shooter_handle);
    MatchPlayer *shooter = player(shooter_handle);
    if (shooter_entity == nullptr || (shooter != nullptr && shooter->spectator))
        return;
    if (shooter != nullptr)
        add_event(world, *shooter, MatchStats::kShotsFired, score_value(0));
    add_team_event(shooter_entity->team, MatchStats::kShotsFired, score_value(0));
}

// Scorer event 6: the medic's MEDICSAVE (RecordEvent 8 + value 7), with the
// team-mode mirror.
// [orig: GameEvent_RevivePlayer @0x517CD0 (the event-6 call @0x517DC5);
//  GameEvent_ProcessScoring case 6 @0x52FCD8..0x52FD2B]
void Match::record_revive(const World &world, EntityHandle medic_handle) {
    const Entity *medic_entity = world.registry.get(medic_handle);
    MatchPlayer *medic = player(medic_handle);
    if (medic_entity == nullptr || (medic != nullptr && medic->spectator))
        return;
    if (medic != nullptr)
        add_event(world, *medic, MatchStats::kMedicSaves, score_value(7));
    add_team_event(medic_entity->team, MatchStats::kMedicSaves, score_value(7));
}

void Match::record_zone_capture(const World &world,
                                const std::vector<EntityHandle> &scorers) {
    if (outcome_.ended)
        return;
    for (const EntityHandle handle : scorers) {
        MatchPlayer *scorer = player(handle);
        if (scorer == nullptr)
            continue;
        const Entity *scorer_entity = world.registry.get(handle);
        if (scorer_entity == nullptr)
            continue;
        // CaptureZone_CheckProximityScoring calls scorer event 24 for every
        // alive same-team slot in radius. Event 24 increments raw stats[39]
        // and applies table[108] = status value 34 to raw stats[29].
        // [orig: call @0x500D84; case 24 @0x5307C2]
        add_event(world, *scorer, MatchStats::kZoneTakeovers, score_value(34));
        add_team_event(scorer_entity->team, MatchStats::kZoneTakeovers, score_value(34));
    }
}

void Match::update_objective_proximity(const World &world) {
    std::vector<const Entity *> proximity_objectives;
    std::vector<const Entity *> hills;
    std::vector<const Entity *> capturable_entities;
    world.registry.for_each([&](const Entity &entity) {
        const int pool = entity.handle.pool();
        if (pool == 1 && entity.has_item_def &&
            objective_proximity_bit(entity.item_id) != 0)
            proximity_objectives.push_back(&entity);
        if (pool == 3 && entity.has_item_def && entity.item_id == kHill)
            hills.push_back(&entity);
        if ((pool == 1 || pool == 2) && entity.has_item_def &&
            (entity.item_attrib & kItemAttribSpawnPoint) != 0 &&
            entity.zone_number != 0)
            capturable_entities.push_back(&entity);
    });

    const bool team_mode = (rules_.game_type & 0x10000u) != 0;
    for (MatchPlayer &match_player : players_) {
        const Entity *player_entity = world.registry.get(match_player.identity.entity);
        match_player.objective_proximity_mask = 0;
        if (player_entity == nullptr)
            continue;
        const bool live = is_live_player(player_entity);

        // This counter is independent of capture contact. Retail increments
        // it for every live, non-respawn-pending player and dispatches event
        // 25 on exact multiples of status value 36 (values < 1 use 0xffff).
        // Event 25 updates only the player: raw stat 40 receives the original
        // status-36 value and points receive status value 35, the one award
        // that never shares with the +0x170 links (param2 = 1).
        // [orig: Server_UpdateCaptureZoneProximity @0x5087C9..0x5087F1;
        // GameEvent_ProcessScoring case 25 @0x530968..0x530990]
        if (live) {
            match_player.periodic_score_ticks =
                wrap_add(match_player.periodic_score_ticks, 1);
            int32_t scoring_interval = score_value(36);
            if (scoring_interval < 1)
                scoring_interval = 0xffff;
            if (match_player.periodic_score_ticks % scoring_interval == 0) {
                add_event(world, match_player, MatchStats::kPeriodicScoreUnits,
                          score_value(35), score_value(36), /*share=*/false);
            }
        }

        // Pool-1 flag/objective types use a fixed 20-unit horizontal radius.
        // A carried objective is tested at its owner's live position.
        // [orig: Server_UpdateCaptureZoneProximity @0x5087F4..0x50899B — the
        //  carrier backref @0x508857..0x5088A6, the 20-unit test
        //  @0x5088BF..0x508925]
        for (const Entity *objective : proximity_objectives) {
            const Entity *position_source = objective;
            if (const Entity *owner = world.registry.get(objective->primary_occupant))
                position_source = owner;
            if (live && within_2d(player_entity->position,
                                  position_source->position, 20.0f))
                match_player.objective_proximity_mask |=
                    objective_proximity_bit(objective->item_id);
        }

        bool in_hill = false;
        for (const Entity *hill : hills) {
            if (!within_2d(player_entity->position, hill->position,
                           hill->bound_radius))
                continue;
            if (live && hill->team < 8u)
                match_player.objective_proximity_mask |=
                    team_mode ? static_cast<uint8_t>(1u << hill->team) : 0x01u;
            if (!team_mode || hill->team == 0)
                in_hill = true;
        }

        bool in_capturable_entity = false;
        for (const Entity *zone : capturable_entities) {
            const float radius = static_cast<float>(zone->zone_radius);
            const float dz = std::fabs(player_entity->position.z - zone->position.z);
            if (!within_2d(player_entity->position, zone->position, radius) ||
                dz > radius * 0.5f)
                continue;
            if (!world.zones.is_capturable(1, *zone) &&
                !world.zones.is_capturable(2, *zone))
                continue;
            in_capturable_entity = true;
            if (live && team_mode && zone->team < 8u)
                match_player.objective_proximity_mask |=
                    static_cast<uint8_t>(1u << zone->team);
        }

        // A numbered capturable entity anywhere in the mission globally wins
        // source precedence over every type-6006 volume. If neither source
        // family exists, retail skips these counters instead of decaying them.
        // [orig: Server_UpdateCaptureZoneProximity @0x508C33..0x508C4F]
        const bool has_capture_source =
            !capturable_entities.empty() || !hills.empty();
        if (!has_capture_source)
            continue;
        const bool inside = !capturable_entities.empty()
                                ? in_capturable_entity
                                : in_hill;
        if (!inside || !live) {
            if (match_player.objective_ticks > 0)
                --match_player.objective_ticks;
            if (match_player.capture_score_ticks > 0)
                --match_player.capture_score_ticks;
            if (match_player.capture_period_ticks > 0)
                --match_player.capture_period_ticks;
            continue;
        }
        if (match_player.objective_ticks < std::numeric_limits<int32_t>::max())
            ++match_player.objective_ticks;
        const int32_t capture_threshold = std::max<int32_t>(1, score_value(12));
        if (++match_player.capture_score_ticks >= capture_threshold) {
            match_player.capture_score_ticks = 0;
            const int32_t raw_delta = score_value(12);
            if (rules_.game_type == gt::kKingOfTheHill ||
                rules_.game_type == gt::kTeamKingOfTheHill) {
                // Events 18: hill time/status 12, points/status 33.
                // [orig: call @0x508CE2..0x508CEA; scorer
                // @0x5307B8..0x530821]
                add_event(world, match_player, MatchStats::kHillTime,
                          score_value(33), raw_delta);
                if (team_mode)
                    add_team_event(player_entity->team, MatchStats::kHillTime,
                                   score_value(33), raw_delta);
            } else {
                const bool friendly_zone = player_entity->team < 8u &&
                    (match_player.objective_proximity_mask &
                     static_cast<uint8_t>(1u << player_entity->team)) != 0;
                if (friendly_zone) {
                    // Event 20 writes friendly time to the player, but retail
                    // writes hostile time to the team row. The mismatch is in
                    // the original scorer and is intentionally retained.
                    // [orig: call @0x508CBE..0x508CEA; player/team split
                    // @0x530890..0x5308F9]
                    add_event(world, match_player, MatchStats::kFriendlyZoneTime,
                              score_value(32), raw_delta);
                    if (team_mode)
                        add_team_event(player_entity->team,
                                       MatchStats::kHostileZoneTime,
                                       score_value(32), raw_delta);
                } else {
                    // Event 19: hostile-zone time/status 12 and
                    // points/status 31. [orig: call @0x508C95..0x508CB1;
                    // scorer @0x530824..0x53088D]
                    add_event(world, match_player, MatchStats::kHostileZoneTime,
                              score_value(31), raw_delta);
                    if (team_mode)
                        add_team_event(player_entity->team,
                                       MatchStats::kHostileZoneTime,
                                       score_value(31), raw_delta);
                }
            }
        }
        if (++match_player.capture_period_ticks >= 10)
            match_player.capture_period_ticks = 0;
    }

    // Game_AccumulateTeamScores consumes the freshly rebuilt bit-0 masks. It
    // runs for every team game, although only TKOTH exposes this hold counter
    // as a win condition. [orig: Game_CountAlivePlayersPerTeam @0x5001C0;
    // Game_AccumulateTeamScores @0x508D70]
    if (team_mode) {
        std::array<int32_t, 5> holders{};
        for (const MatchPlayer &match_player : players_) {
            const Entity *entity = world.registry.get(match_player.identity.entity);
            if (!is_live_player(entity) ||
                (match_player.objective_proximity_mask & 0x01u) == 0 ||
                entity->team >= holders.size())
                continue;
            ++holders[entity->team];
        }
        // Per team row, once a second: at least one alive holder in the
        // volume gains one tick, an empty team loses min(hold, koth_delta).
        // [orig: Game_AccumulateTeamScores @0x508DA0..0x508DC2 — holders test
        // @0x508DA0, ++hold @0x508DAD, the clamped decrement @0x508DB2..0x508DC2]
        for (uint8_t team = 0; team < team_hold_ticks_.size(); ++team) {
            if (holders[team] > 0) {
                if (team_hold_ticks_[team] < std::numeric_limits<int32_t>::max())
                    ++team_hold_ticks_[team];
            } else if (team_hold_ticks_[team] > 0) {
                team_hold_ticks_[team] = std::max<int32_t>(
                    0, team_hold_ticks_[team] - static_cast<int32_t>(rules_.hill_delta));
            }
        }
    }
}

// [orig: Entity_UpdateIdleCheck @0x408430]
void Match::tick_flag_event(World &world, Entity &flag) {
    if (!world.rules.logic_authority) {
        flag.class_think_ticks = 0x1000000;
        return;
    }
    CarryObjectiveState *state = carry_state(world, flag.handle);
    if (state == nullptr) return;
    const int32_t x = int32_t(flag.position.x * 65536);
    const int32_t y = int32_t(flag.position.y * 65536);
    const int32_t dx = io::bam_sub(x, int32_t(state->home.x * 65536));
    const int32_t dy = io::bam_sub(y, int32_t(state->home.y * 65536));
    const int32_t dz = io::bam_sub(int32_t(flag.position.z * 65536), int32_t(state->home.z * 65536));
    const bool near_home = std::sqrt(double(dx) * dx + double(dy) * dy) < 131072.0 &&
            io::bam_abs(dz) < 131072;
    const bool idle = x == state->previous_x_q16 && y == state->previous_y_q16 &&
            !near_home && !flag.primary_occupant.valid();
    const Entity *ground = world.registry.get(flag.ground_target);
    if ((ground != nullptr && !near_home && (!ground->has_item_def || ground->item_type == 1)) ||
            idle) {
        state->return_ticks = io::bam_sub(state->return_ticks, 1);
    } else {
        state->return_ticks = rules_.flag_return_ticks < 5 ? 210 : int32_t(rules_.flag_return_ticks);
    }
    if (state->return_ticks <= 0) {
        const EntityHandle handle = flag.handle;
        world.registry.for_each_in_pool(0, [&](const Entity &body) {
            if (body.mounted_child == handle) drop_carried_object(world, body.handle);
        });
        return_flag_home(world, handle, MatchGameplayEventKind::FlagReturn);
        state = carry_state(world, handle);
    }
    state->previous_x_q16 = int32_t(flag.position.x * 65536);
    state->previous_y_q16 = int32_t(flag.position.y * 65536);
    const Entity *occupant = world.registry.get(flag.primary_occupant);
    const Entity *local = world.registry.get(world.cached.local_player);
    if (occupant != nullptr && local != nullptr && local->team == flag.team) {
        flag.position.x = occupant->position.x;
        flag.position.y = occupant->position.y;
    }
    flag.class_think_ticks = 62;
}

void Match::update_flag_objectives(World &world) {
    // The retail collision dispatcher keys only on the objective item ID. It
    // has no game-type gate, which is observable in C&C's combined flag/zone
    // score schema. [orig: Entity_ProcessWaypointInteraction @0x4AD820;
    // GameType_CreateDefaultSettings @0x52DD00]
    ensure_objective_census(world);

    if (world.collision == nullptr)
        return;
    const std::vector<CollisionWorld::GameplayContact> contacts =
        world.collision->take_movement_callback_contacts();
    for (const CollisionWorld::GameplayContact &contact : contacts) {
        MatchPlayer *match_player = player(contact.source);
        Entity *carrier = world.registry.get(contact.source);
        Entity *target = world.registry.get(contact.target);
        const bool move_callback = target != nullptr && target->has_item_def &&
            (target->item_attrib & kItemAttribMoveCallback) != 0 &&
            (target->item_attrib & kItemAttribPowerup) == 0;
        if (match_player == nullptr || carrier == nullptr || !move_callback ||
            (carrier->flags & (kEntityFlagDead | kEntityFlagPlayer)) !=
                kEntityFlagPlayer ||
            (carrier->net_move_input & Entity::kMoveOrderMoving) == 0)
            continue;

        // Preserve resolver/candidate order. If one movement pass touches a
        // flag and its bay, retail mutates the carried link inline before the
        // later callback. [orig: sole caller @0x4B2FF5; handler @0x4AD820]
        if (carrier->mounted_child.valid()) {
            Entity *flag = world.registry.get(carrier->mounted_child);
            if (flag == nullptr) {
                carrier->mounted_child = EntityHandle{};
                continue;
            }
            if (!is_flag_bay(target->item_id))
                continue;
            // The bay matrix: the blue bay (4098) takes a carried red or
            // neutral flag from team 1, or from ANY team when g_GameType == 8
            // (Flag Me); the red bay (4100) takes blue/neutral from team 2;
            // 4102 and 4103 take only the neutral flag from teams 4 and 3.
            // [orig: Entity_ProcessWaypointInteraction — 4098 @0x4AD95E..0x4AD9A2
            // (the Flag Me OR @0x4AD966..0x4AD976), 4100 @0x4AD9A6..0x4AD9D2,
            // 4102 @0x4AD9E3, 4103 @0x4AD9D6; capture @0x4ADA19]
            const bool neutral = flag->item_id == kNeutralFlag;
            const bool accepted =
                (target->item_id == kBlueBay &&
                 (rules_.game_type == gt::kFlagMe || carrier->team == 1) &&
                 (flag->item_id == kRedFlag || neutral)) ||
                (target->item_id == kRedBay && carrier->team == 2 &&
                 (flag->item_id == kBlueFlag || neutral)) ||
                (target->item_id == kTeam3Bay && carrier->team == 3 && neutral) ||
                (target->item_id == kTeam4Bay && carrier->team == 4 && neutral);
            if (accepted)
                record_flag_capture(world, contact.source,
                                    carrier->mounted_child);
            continue;
        }

        if (!is_flag(target->item_id) || target->primary_occupant.valid())
            continue;
        CarryObjectiveState *state = carry_state(world, contact.target);
        const bool own_flag =
            (carrier->team == 1 && target->item_id == kBlueFlag) ||
            (carrier->team == 2 && target->item_id == kRedFlag);
        if (own_flag) {
            // An own flag is SAVED only when Entity_SyncPositionFromDefinition
            // reports it displaced from its authored pose — a 2D distance or a
            // height delta of at least 0x20000 (2 u); the return timer is not
            // consulted, and an own flag at home does nothing (no pickup arm
            // exists for it in any game type).
            // [orig: Entity_ProcessWaypointInteraction 4091 @0x4AD8E9..0x4AD912
            // / 4093 @0x4AD8A3..0x4AD8CD -> Entity_SyncPositionFromDefinition
            // @0x43A9B0 (the 0x20000 tests @0x43AA3B/@0x43AA4D, return 1
            // @0x43A9BE) -> Server_BroadcastEntityDeathEvent @0x4AD8D3]
            if (state == nullptr)
                continue;
            const int64_t dx = int64_t{to_fixed(target->position.x)} -
                               to_fixed(state->home.x);
            const int64_t dy = int64_t{to_fixed(target->position.y)} -
                               to_fixed(state->home.y);
            const int64_t dz = int64_t{to_fixed(target->position.z)} -
                               to_fixed(state->home.z);
            const bool displaced =
                static_cast<int64_t>(std::sqrt(static_cast<long double>(
                    dx * dx + dy * dy))) >= 0x20000 ||
                (dz < 0 ? -dz : dz) >= 0x20000;
            if (displaced)
                record_flag_save(world, contact.source, contact.target);
            continue;
        }
        // An enemy or neutral flag is picked up when the toucher carries
        // nothing. [orig: Entity_ProcessWaypointInteraction @0x4AD820 — the
        // carried-object test @0x4AD936, the Entity_TryAttachToVehicle call
        // @0x4AD944, the Server_DispatchScoringEvent call @0x4AD94B, +0x124 = 0
        // @0x4AD955]
        record_flag_pickup(world, contact.source, contact.target);
    }
}

void Match::advance_tick(World &world, TickPhase phase) {
    // The shared periodic service starts armed at zero, executes immediately,
    // then reloads 62 and decrements-before-testing on later simulation ticks.
    // KOTH accumulation and host-side 1 Hz services read this frame's verdict.
    // Flags use their own class countdown. The host reads the shared verdict
    // through periodic_second(). The countdown keeps running after the round
    // ends because the host's linger-phase legs still ride it.
    // [orig: g_periodic_second_timer in Server_TickUpdate @0x51D7E0;
    // Server_UpdateCaptureZoneProximity @0x5086A0]
    if (periodic_second_timer_ > 0)
        --periodic_second_timer_;
    periodic_second_fired_ = periodic_second_timer_ == 0;
    if (periodic_second_fired_)
        periodic_second_timer_ = 62;
    if (outcome_.ended)
        return;
    ensure_objective_census(world);
    if (periodic_second_fired_)
        update_objective_proximity(world);
    if (phase != TickPhase::Gameplay)
        return;
    update_flag_objectives(world);
    if (remaining_ticks_ > 0)
        --remaining_ticks_;
}

std::vector<MatchGameplayEvent> Match::drain_gameplay_events() {
    std::vector<MatchGameplayEvent> out;
    out.swap(gameplay_events_);
    return out;
}

int32_t Match::primary_score(const MatchStats &stats, int32_t objective_ticks) const {
    // [orig: ScoreRules_GetPrimaryScoreField (ex sub_52C850) @0x52C850]
    if (is_waypoint_family(rules_.game_type))
        return stats[MatchStats::kPoints];
    if (rules_.game_type == gt::kAdvanceAndSecure ||
        rules_.game_type == gt::kConquerAndControl)
        return stats[MatchStats::kZoneTakeovers];
    if (rules_.game_type == gt::kDeathmatch || rules_.game_type == gt::kTeamDeathmatch)
        return stats[MatchStats::kEnemyKills];
    if (rules_.game_type == gt::kKingOfTheHill ||
        rules_.game_type == gt::kTeamKingOfTheHill)
        return objective_ticks;
    if (rules_.game_type == gt::kSearchAndDestroy ||
        rules_.game_type == gt::kAttackDefend)
        return stats[MatchStats::kTargetsDestroyed];
    if (rules_.game_type == gt::kCaptureTheFlag ||
        rules_.game_type == gt::kFlagBall || rules_.game_type == gt::kFlagMe)
        return stats[MatchStats::kFlagCaptures];
    return 0;
}

int32_t Match::primary_score(const MatchPlayer &match_player) const {
    return primary_score(match_player.stats, match_player.objective_ticks);
}

int32_t Match::team_objective_ticks(const World &world, uint8_t team) const {
    int64_t total = 0;
    for (const MatchPlayer &match_player : players_) {
        const Entity *entity = world.registry.get(match_player.identity.entity);
        if (entity != nullptr && entity->team == team)
            total += match_player.objective_ticks;
    }
    return static_cast<int32_t>(std::clamp<int64_t>(
        total, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

int32_t Match::team_primary_score(uint8_t team) const {
    // Both board builders pass TeamRecord+0x150 (unknown_040[272]), the hill
    // hold timer, as the KOTH family's external score; the per-second fold
    // of the players' slot ticks lands in the neighbouring +0x148 word and is
    // not what either board reads.
    // [orig: Server_BuildAndBroadcastScoreboard @0x50DB92..@0x50DCCE;
    // Server_BuildEndOfRoundScoreboard @0x508FA7/@0x508FB8;
    // Game_AccumulateTeamScores @0x508DAD/@0x508DC2 (the +0x150 timer),
    // @0x508E0A (the +0x148 fold)]
    if (team >= teams_.size())
        return 0;
    return primary_score(teams_[team], team_hold_ticks_[team]);
}

MatchLiveScoreboard Match::live_scoreboard(World &world) {
    MatchLiveScoreboard out;
    out.team_count = gt::active_team_count(rules_.game_type, rules_.team_count);
    out.team_mode = out.team_count != 0;
    // Bit 1 is literal game-type 1 only; team KOTH uses its per-team auxiliary
    // bytes instead. [orig: Server_BuildAndBroadcastScoreboard @0x50D960]
    out.timed_score_mode = rules_.game_type == gt::kKingOfTheHill;
    for (uint8_t team = 1; team <= out.team_count; ++team) {
        out.teams[team].primary_score = team_primary_score(team);
        out.teams[team].points = teams_[team][MatchStats::kPoints];
    }

    if (rules_.game_type == gt::kTeamKingOfTheHill) {
        std::array<uint32_t, 5> alive{};
        for (const MatchPlayer &match_player : players_) {
            const Entity *entity = world.registry.get(match_player.identity.entity);
            if (entity != nullptr && entity->team < alive.size() && entity->alive &&
                (entity->flags & kEntityFlagDead) == 0)
                ++alive[entity->team];
        }
        for (uint8_t team = 1; team <= out.team_count; ++team)
            out.teams[team].alive_players = static_cast<uint8_t>(alive[team]);
    }

    if (rules_.game_type == gt::kCaptureTheFlag) {
        ensure_objective_census(world);
        // The row byte is the side's OWN authored flag count, while the win
        // target is the opposing flag count; invert the scorer-facing census.
        out.teams[1].authored_objectives =
            static_cast<uint8_t>(flag_capture_targets_[2]);
        out.teams[2].authored_objectives =
            static_cast<uint8_t>(flag_capture_targets_[1]);
    } else if (rules_.game_type == gt::kSearchAndDestroy ||
               rules_.game_type == gt::kAttackDefend) {
        ensure_objective_census(world);
        out.teams[1].authored_objectives =
            static_cast<uint8_t>(demolition_targets_[2]);
        out.teams[2].authored_objectives =
            static_cast<uint8_t>(demolition_targets_[1]);
    }
    return out;
}

std::optional<int32_t> Match::winner_if_finished(const World &world) {
    if (outcome_.ended)
        return std::nullopt;
    ensure_objective_census(world);

    // The uniform-zone test is first and game-type independent. Empty chains
    // do not win. [orig: Server_CheckWinConditions @0x51AD8A ->
    // ZoneSlotChain_GetWinningTeamIfAllOwned @0x4A2920]
    bool saw_zone = false;
    bool uniform_zones = true;
    uint8_t uniform_team = 0;
    for (const EntityHandle handle : world.zones.chain.zones) {
        const Entity *zone = world.registry.get(handle);
        if (zone == nullptr)
            continue;
        if (!saw_zone) {
            saw_zone = true;
            uniform_team = zone->team;
        } else if (uniform_team != zone->team) {
            uniform_zones = false;
            break;
        }
    }
    if (saw_zone && uniform_zones) {
        // The helper's success bit is independent of the returned team. A
        // uniformly neutral chain therefore suppresses every later win arm
        // without producing a winner. [orig:
        // ZoneSlotChain_GetWinningTeamIfAllOwned @0x4A2920;
        // Server_CheckWinConditions @0x51AD8A..0x51ADA3]
        if (uniform_team != 0)
            return static_cast<int32_t>(uniform_team);
        return std::nullopt;
    }

    if (rules_.game_type == gt::kDeathmatch) {
        if (rules_.score_limit != 0) {
            for (const MatchPlayer &match_player : players_) {
                if (world.registry.get(match_player.identity.entity) != nullptr &&
                    match_player.stats[MatchStats::kEnemyKills] >=
                        static_cast<int32_t>(rules_.score_limit))
                    return 0;
            }
        }
        return remaining_ticks_ == 0 ? std::optional<int32_t>{0} : std::nullopt;
    }

    if (rules_.game_type == gt::kTeamDeathmatch) {
        // A zero score limit returns from the entire TDM arm before clock
        // expiry. [orig: Server_CheckWinConditions @0x51AE34..0x51AE3B]
        if (rules_.score_limit == 0)
            return std::nullopt;
        for (uint8_t team = 0; team < teams_.size(); ++team) {
            if (teams_[team][MatchStats::kEnemyKills] >=
                static_cast<int32_t>(rules_.score_limit))
                return static_cast<int32_t>(team);
        }
        if (remaining_ticks_ != 0)
            return std::nullopt;
        std::array<int32_t, 5> scores{};
        for (uint8_t team = 1; team <= 4; ++team)
            scores[team] = teams_[team][MatchStats::kEnemyKills];
        return unique_best_team(scores);
    }

    const int64_t hill_threshold = int64_t{60} * rules_.hill_limit_minutes;
    if (rules_.game_type == gt::kKingOfTheHill) {
        for (const MatchPlayer &match_player : players_) {
            if (world.registry.get(match_player.identity.entity) != nullptr &&
                int64_t{match_player.objective_ticks} >= hill_threshold)
                return 0;
        }
        return remaining_ticks_ == 0 ? std::optional<int32_t>{0} : std::nullopt;
    }

    if (rules_.game_type == gt::kTeamKingOfTheHill) {
        for (uint8_t team = 1; team <= 4; ++team) {
            if (int64_t{team_hold_ticks_[team]} >= hill_threshold)
                return static_cast<int32_t>(team);
        }

        std::array<int32_t, 5> objective_scores{};
        for (uint8_t team = 1; team <= 4; ++team)
            objective_scores[team] = team_objective_ticks(world, team);
        // The clinch: each team's floor is score - koth_delta * remaining; the
        // moment one floor exceeds every other team's score + remaining the
        // round clock is zeroed. [orig: Server_CheckWinConditions
        // @0x51B018..0x51B064 — g_koth_delta * remaining @0x51B018..0x51B024,
        // the four floor comparisons @0x51B02C..0x51B062, clock = 0 @0x51B064]
        if (remaining_ticks_ > 0) {
            for (uint8_t team = 1; team <= 4; ++team) {
                const int64_t floor = int64_t{objective_scores[team]} -
                                      int64_t{remaining_ticks_} * rules_.hill_delta;
                bool clinched = true;
                for (uint8_t other = 1; other <= 4; ++other) {
                    if (other != team &&
                        floor <= int64_t{objective_scores[other]} + remaining_ticks_) {
                        clinched = false;
                        break;
                    }
                }
                if (clinched) {
                    remaining_ticks_ = 0;
                    break;
                }
            }
        }
        if (remaining_ticks_ != 0)
            return std::nullopt;
        const int32_t winner = unique_best_team(team_hold_ticks_);
        // At clock zero a strict four-way maximum decides, but retail's team-4
        // branch pushes 1 (it jumps to the team-1 label), so team 4 can win
        // only at the hill limit above. [orig: Server_CheckWinConditions
        // clock-zero pushes @0x51B0A0 (2) / @0x51B0B0 (1) / @0x51B0C0 (3) /
        // @0x51B0D0 (team 4 -> 1) / @0x51B0D4 (0)]
        return winner == 4 ? 1 : winner;
    }

    if (rules_.game_type == gt::kCaptureTheFlag) {
        // Raw field 0xB against the two capture limits (dword_C8FF00 /
        // dword_C8FEFC): t1 >= lim1 (lim1 != 0) -> 1; t2 >= lim2 (lim2 != 0)
        // -> 2; at clock zero lim2 == 0 -> 2, lim1 == 0 -> 1, t2 > t1 -> 2,
        // t1 > t2 -> 1, else 0. [orig: Server_CheckWinConditions
        // @0x51B0DE..0x51B196 — @0x51B108 / @0x51B129 / @0x51B13A / @0x51B147 /
        // @0x51B169 / @0x51B18B / @0x51B18F]
        const int32_t team1 = teams_[1][MatchStats::kFlagCaptures];
        const int32_t team2 = teams_[2][MatchStats::kFlagCaptures];
        const int32_t target1 = flag_capture_targets_[1];
        const int32_t target2 = flag_capture_targets_[2];
        if (target1 != 0 && team1 >= target1)
            return 1;
        if (target2 == 0 || team2 < target2) {
            if (remaining_ticks_ != 0)
                return std::nullopt;
            if (target2 != 0) {
                if (target1 == 0)
                    return 1;
                if (team2 <= team1)
                    return team1 > team2 ? 1 : 0;
            }
        }
        return 2;
    }

    if (rules_.game_type == gt::kSearchAndDestroy ||
        rules_.game_type == gt::kAttackDefend) {
        // Raw field 0xD against the demolition limits (dword_C8FF04 /
        // dword_C8FF08) with the same two award arms, but the clock-zero tail
        // never compares counts: lim2 == 0 -> 2, lim1 == 0 -> 1, else 0 (both
        // limits nonzero is always a draw). [orig: Server_CheckWinConditions
        // @0x51B199..0x51B208 — @0x51B1C6 / @0x51B1E4 / @0x51B1F5 / @0x51B202 /
        // @0x51B206]
        const int32_t team1 = teams_[1][MatchStats::kTargetsDestroyed];
        const int32_t team2 = teams_[2][MatchStats::kTargetsDestroyed];
        const int32_t target1 = demolition_targets_[1];
        const int32_t target2 = demolition_targets_[2];
        if (target1 != 0 && team1 >= target1)
            return 1;
        if (target2 == 0 || team2 < target2) {
            if (remaining_ticks_ != 0)
                return std::nullopt;
            if (target2 != 0)
                return target1 == 0 ? 1 : 0;
        }
        return 2;
    }

    if (rules_.game_type == gt::kFlagBall) {
        // The field-0xB limit scan over teams 1..4 has NO zero-guard on the
        // limit — limit 0 instantly awards team 1 — then the clock-zero tail
        // runs the strict-unique-max chain.
        // [orig: Server_CheckWinConditions @0x51B21A..0x51B413]
        for (uint8_t team = 1; team <= 4; ++team) {
            if (teams_[team][MatchStats::kFlagCaptures] >=
                static_cast<int32_t>(rules_.max_score))
                return static_cast<int32_t>(team);
        }
        if (remaining_ticks_ != 0)
            return std::nullopt;
        std::array<int32_t, 5> scores{};
        for (uint8_t team = 1; team <= 4; ++team)
            scores[team] = teams_[team][MatchStats::kFlagCaptures];
        return unique_best_team(scores);
    }

    if (rules_.game_type == gt::kFlagMe) {
        // `if (!g_kill_limit) return;` guards the whole arm, then the per-slot
        // field-0xB scan ends the round with no clock arm at all.
        // [orig: Server_CheckWinConditions @0x51B422..0x51B47C]
        if (rules_.max_score == 0)
            return std::nullopt;
        for (const MatchPlayer &match_player : players_) {
            if (world.registry.get(match_player.identity.entity) != nullptr &&
                match_player.stats[MatchStats::kFlagCaptures] >=
                    static_cast<int32_t>(rules_.max_score))
                return 0;
        }
        return std::nullopt;
    }

    if ((rules_.game_type == gt::kAdvanceAndSecure ||
         rules_.game_type == gt::kConquerAndControl) &&
        remaining_ticks_ == 0) {
        // At clock zero both A&S and C&C count the held zones per team
        // (CTeamSlotManager_CountByTeam) and award the larger count, draw on a
        // tie. [orig: Server_CheckWinConditions @0x51B49E..0x51B4E6]
        int32_t team1 = 0;
        int32_t team2 = 0;
        for (const EntityHandle handle : world.zones.chain.zones) {
            const Entity *zone = world.registry.get(handle);
            if (zone == nullptr)
                continue;
            if (zone->team == 1)
                ++team1;
            else if (zone->team == 2)
                ++team2;
        }
        if (team1 == team2)
            return 0;
        return team1 > team2 ? 1 : 2;
    }

    // Co-op/waypoint outcomes remain owned by WAC/BMS and converge through
    // World::process_round_end. [orig: Server_CheckWinConditions @0x51AD40]
    return std::nullopt;
}

bool Match::finish(int32_t winner_team, const World &world) {
    if (outcome_.ended)
        return false;
    outcome_.winner_team = winner_team;
    // Winner event 21 writes raw field 35 = 2 before the board build; it is an
    // assignment, so the later eligible-player pass is idempotent.
    // [orig: calls @0x516565/@0x5167FD; scorer case 21 @0x53073B]
    if (winner_team != 0) {
        for (MatchPlayer &p : players_) {
            const Entity *entity = world.registry.get(p.identity.entity);
            if (entity != nullptr && entity->team == winner_team)
                p.stats[MatchStats::kRoundMarker] = 2;
        }
    }
    outcome_.ended = true;

    result_.ready = true;
    result_.game_type = rules_.game_type;
    result_.winner_team = winner_team;
    result_.score_fields = rules_.score_fields;
    result_.team_stats = teams_;
    result_.team_hold_ticks = team_hold_ticks_;
    result_.team_row_count = scoreboard_team_row_count(rules_);
    if (rules_.game_type == gt::kAdvanceAndSecure ||
        rules_.game_type == gt::kConquerAndControl) {
        for (const EntityHandle handle : world.zones.chain.zones) {
            const Entity *zone = world.registry.get(handle);
            if (zone == nullptr)
                continue;
            if (zone->team == 1)
                ++result_.team_scores[0];
            else if (zone->team == 2)
                ++result_.team_scores[1];
        }
    } else {
        result_.team_scores[0] = team_primary_score(1);
        result_.team_scores[1] = team_primary_score(2);
    }
    result_.players.reserve(players_.size());
    for (const MatchPlayer &player : players_) {
        const Entity *entity = world.registry.get(player.identity.entity);
        result_.players.push_back(MatchResultPlayer{
            player.identity,
            entity != nullptr ? entity->team : uint8_t{0},
            entity != nullptr ? entity->player_class : uint8_t{0},
            player.stats,
            player.objective_ticks,
            primary_score(player),
        });
    }
    sort_scoreboard_players(result_.players, rules_.game_type);

    if (gt::is_team(rules_.game_type)) {
        // [orig: Server_BuildEndOfRoundScoreboard @0x5092AD]
        result_.draw = result_.team_scores[0] == result_.team_scores[1];
        return true;
    }

    // Non-team draw: the slot scan keeps the largest sort key seen from zero;
    // the row walk clears the draw flag for any row whose primary is below
    // it; a lone row with a positive score is not a draw either. For a
    // non-team type the sort key IS the primary.
    // [orig: Server_BuildEndOfRoundScoreboard — max @0x509053..0x509055, v47
    // = 1 @0x50909D, the per-row clear @0x50920E..0x509210, the single-row
    // clear @0x50926A..0x50926C, g_endround_draw_flag @0x509270/@0x5092B2]
    int32_t max_key = 0;
    for (const MatchResultPlayer &row : result_.players)
        max_key = std::max(max_key, row.primary_score);
    bool all_tied = true;
    for (const MatchResultPlayer &row : result_.players) {
        if (row.primary_score < max_key)
            all_tied = false;
    }
    if (result_.players.size() == 1 && max_key > 0)
        all_tied = false;
    result_.draw = all_tied;

    // Non-team winner: unless the board is a draw or its first two rows tie
    // (row 1 reads zero from the memset table when only one row exists),
    // every state-6 slot whose sort key equals row 0's primary receives
    // event 21 -> RecordEvent(34, 2) -> raw field 35 = 2. The scorer refuses
    // a spectator-flagged slot and a slot without an entity.
    // [orig: Server_ProcessRoundEnd — isDrawOrNonTeam @0x5165A3..0x5165C3
    // over dword_24C1AD4/dword_24C1BB8 (rows 0/1 entry+0x40), the compare
    // @0x5167E2..0x5167F2, GameEvent_ProcessScoring(gt, entity, 21, 0, 2)
    // @0x5167F6..0x5167FD; the memset @0x508F3F; case 21 @0x52FF20..0x52FF34;
    // the spectator early-out @0x52F6FA; CPlayerStats_RecordEvent case 34
    // @0x52C8E0]
    const int32_t row0_primary =
        result_.players.empty() ? 0 : result_.players[0].primary_score;
    const int32_t row1_primary =
        result_.players.size() > 1 ? result_.players[1].primary_score : 0;
    if (result_.draw || row1_primary == row0_primary)
        return true;
    for (MatchResultPlayer &row : result_.players) {
        if (row.primary_score != row0_primary)
            continue;
        MatchPlayer *match_player = player(row.identity.entity);
        if (match_player == nullptr || match_player->spectator ||
            world.registry.get(row.identity.entity) == nullptr)
            continue;
        match_player->stats[MatchStats::kRoundMarker] = 2;
        row.stats[MatchStats::kRoundMarker] = 2;
    }
    return true;
}

} // namespace opennova::world
