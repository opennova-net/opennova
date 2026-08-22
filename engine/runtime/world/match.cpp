#include "world/match.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "world/game_type.h"
#include "world/world.h"

namespace opennova::world {
namespace {

namespace gt = opennova::game_type;

constexpr int32_t kTicksPerMinute = 60 * 62;
constexpr int32_t kBlueFlag = 4091;
constexpr int32_t kRedFlag = 4093;
constexpr int32_t kNeutralFlag = 4095;
constexpr int32_t kBlueBay = 4098;
constexpr int32_t kRedBay = 4100;
constexpr int32_t kTeam4Bay = 4102;
constexpr int32_t kTeam3Bay = 4103;
constexpr int32_t kHill = 6006;

bool is_flag(int32_t item_id) {
    return item_id == kBlueFlag || item_id == kRedFlag || item_id == kNeutralFlag;
}

bool is_flag_bay(int32_t item_id) {
    return item_id == kBlueBay || item_id == kRedBay ||
           item_id == kTeam4Bay || item_id == kTeam3Bay;
}

bool overlaps_objective(const Entity &player, const Entity &objective) {
    const float dx = player.position.x - objective.position.x;
    const float dy = player.position.y - objective.position.y;
    const float dz = player.position.z - objective.position.z;
    const float radius = std::max(2.0f, player.bound_radius + objective.bound_radius);
    return dx * dx + dy * dy + dz * dz <= radius * radius;
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

uint8_t scoreboard_team_row_count(const MatchRules &rules) {
    const uint8_t teams = gt::active_team_count(rules.game_type, rules.team_count);
    return teams == 0 ? 0 : static_cast<uint8_t>(teams + 1);
}

void sort_scoreboard_players(std::vector<MatchResultPlayer> &players) {
    // Server_BuildEndOfRoundScoreboard first scans player slots in numeric
    // order, then sorts the {raw points, player} pairs with this exact
    // descending Knuth-gap shell sort. Row order is wire-visible through the
    // recipient index in S2C 0x1D.
    // [orig: slot scan @0x508F30; CPairList_ShellSortByValue @0x526CF0]
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
            const int32_t key = insert.stats[MatchStats::kPoints];
            size_t j = i;
            while (j >= gap && players[j - gap].stats[MatchStats::kPoints] < key) {
                players[j] = std::move(players[j - gap]);
                j -= gap;
            }
            players[j] = std::move(insert);
        }
    }
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
    teams_ = {};
    team_hold_ticks_ = {};
    periodic_second_timer_ = 0;
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
    // 3720*g_respawn_time @0x524F66; decrement @0x5266D6]
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

void Match::remove_player(EntityHandle entity) {
    players_.erase(
        std::remove_if(players_.begin(), players_.end(),
                       [&](const MatchPlayer &p) { return p.identity.entity == entity; }),
        players_.end());
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

void Match::add_event(MatchPlayer &player, size_t counter, int32_t points) {
    if (!gt::has_score_table(rules_.game_type))
        return;
    ++player.stats[counter];
    player.stats[MatchStats::kPoints] += points;
}

void Match::add_team_event(uint8_t team, size_t counter, int32_t points) {
    if (!gt::has_score_table(rules_.game_type) || team >= teams_.size())
        return;
    ++teams_[team][counter];
    teams_[team][MatchStats::kPoints] += points;
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
    if (entity == nullptr || !is_flag(entity->item_id))
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
    add_event(*scorer, MatchStats::kFlagPickups, score_value(11));
    add_team_event(carrier->team, MatchStats::kFlagPickups, score_value(11));
    gameplay_events_.push_back({MatchGameplayEventKind::FlagPickup,
                                player_handle,
                                flag_handle,
                                flag->position,
                                flag->position,
                                static_cast<uint8_t>(flag->flags),
                                player_handle,
                                flag->ground_target,
                                false});
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
                                false});
}

void Match::record_flag_save(World &world, EntityHandle player_handle,
                             EntityHandle flag_handle) {
    MatchPlayer *scorer = player(player_handle);
    const Entity *entity = world.registry.get(player_handle);
    if (outcome_.ended || scorer == nullptr || entity == nullptr)
        return;
    return_flag_home(world, flag_handle, MatchGameplayEventKind::FlagSave,
                     player_handle);
    add_event(*scorer, MatchStats::kFlagSaves, score_value(9));
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
    add_event(*scorer, MatchStats::kFlagCaptures, score_value(10));
    uint8_t scoring_team = carrier->team;
    // CTF's two globals are routed by the FLAG TYPE, not by a caller-supplied
    // scorer team: red captures advance team 1, blue captures team 2. Valid
    // bay interactions imply the same team, but keeping the original routing
    // matters for script-authored/scorer calls and hostile state.
    // [orig: Server_CheckWinConditions @0x51B0F0; the red/blue counter writes
    // reached from Server_ProcessScoringAndBroadcast @0x5169C0]
    if (rules_.game_type == gt::kCaptureTheFlag && flag != nullptr) {
        if (flag->item_id == kRedFlag)
            scoring_team = 1;
        else if (flag->item_id == kBlueFlag)
            scoring_team = 2;
    }
    add_team_event(scoring_team, MatchStats::kFlagCaptures, score_value(10));
    if (flag == nullptr)
        return;
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
                             remove};
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
    if (target == nullptr || attacker_entity == nullptr || attacker == nullptr ||
        (target->item_attrib & kItemAttribObjectiveTarget) == 0)
        return;
    // Scorer event 11 increments raw stats[14] and applies table[87], i.e.
    // status VAR index 13. [orig: GameEvent_ProcessScoring @0x52F550]
    add_event(*attacker, MatchStats::kTargetsDestroyed, score_value(13));
    add_team_event(attacker_entity->team, MatchStats::kTargetsDestroyed, score_value(13));
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
                                false});
}

void Match::record_death(World &world, EntityHandle victim_handle,
                         EntityHandle killer_handle) {
    if (outcome_.ended)
        return;
    drop_carried_object(world, victim_handle);
    const Entity *victim_entity = world.registry.get(victim_handle);
    if (victim_entity == nullptr)
        return;
    if ((victim_entity->item_attrib & kItemAttribObjectiveTarget) != 0)
        record_target_destroyed(world, victim_handle, killer_handle);

    MatchPlayer *victim = player(victim_handle);
    if (victim == nullptr)
        return;

    const uint8_t victim_team = victim_entity->team;
    // Event 3's victim-only call reaches scorer event 6: death + table[79]
    // (status value 5), for both Player and team stats.
    // [orig: GameEvent_PlayerDeath call @0x516F06; scorer @0x52FD75]
    add_event(*victim, MatchStats::kDeaths, score_value(5));
    add_team_event(victim_team, MatchStats::kDeaths, score_value(5));

    MatchPlayer *killer = player(killer_handle);
    if (killer == nullptr)
        return;
    const Entity *killer_entity = world.registry.get(killer_handle);
    if (killer_entity == nullptr)
        return;
    const uint8_t killer_team = killer_entity->team;
    if (killer_handle == victim_handle) {
        // suicide event 5 + table[78] (status value 4)
        // [orig: GameEvent_ProcessScoring @0x52FB80]
        add_event(*killer, MatchStats::kSuicides, score_value(4));
        add_team_event(killer_team, MatchStats::kSuicides, score_value(4));
    } else if (killer_team != 0 && killer_team == victim_team) {
        // team kill event 3 + table[76] (status value 2)
        // [orig: GameEvent_ProcessScoring @0x52FBC7]
        add_event(*killer, MatchStats::kTeamKills, score_value(2));
        add_team_event(killer_team, MatchStats::kTeamKills, score_value(2));
    } else {
        // enemy-player kill event 4 + table[77] (status value 3)
        // [orig: GameEvent_ProcessScoring @0x52FC99]
        add_event(*killer, MatchStats::kEnemyKills, score_value(3));
        add_team_event(killer_team, MatchStats::kEnemyKills, score_value(3));
    }
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
        add_event(*scorer, MatchStats::kZoneTakeovers, score_value(34));
        add_team_event(scorer_entity->team, MatchStats::kZoneTakeovers, score_value(34));
    }
}

void Match::update_hill_presence(const World &world) {
    if (rules_.game_type != gt::kKingOfTheHill &&
        rules_.game_type != gt::kTeamKingOfTheHill)
        return;

    std::array<int32_t, 5> holders{};
    for (MatchPlayer &match_player : players_) {
        const Entity *player_entity = world.registry.get(match_player.identity.entity);
        bool in_hill = false;
        if (player_entity != nullptr && player_entity->alive &&
            (player_entity->flags & kEntityFlagDead) == 0) {
            world.registry.for_each([&](const Entity &objective) {
                if (in_hill)
                    return;
                const bool hill = objective.item_id == kHill || objective.is_capture_trigger;
                if (!hill)
                    return;
                if (rules_.game_type == gt::kTeamKingOfTheHill && objective.team != 0)
                    return;
                const float radius = objective.bound_radius > 0.0f
                                         ? objective.bound_radius
                                         : static_cast<float>(objective.zone_radius);
                if (radius <= 0.0f)
                    return;
                const float dx = player_entity->position.x - objective.position.x;
                const float dy = player_entity->position.y - objective.position.y;
                in_hill = dx * dx + dy * dy <= radius * radius;
            });
        }
        if (in_hill) {
            if (player_entity->team < holders.size())
                ++holders[player_entity->team];
            if (match_player.objective_ticks < std::numeric_limits<int32_t>::max())
                ++match_player.objective_ticks;
        } else if (match_player.objective_ticks > 0) {
            --match_player.objective_ticks;
        }
    }
    if (rules_.game_type == gt::kTeamKingOfTheHill) {
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

void Match::update_flag_objectives(World &world, bool advance_return_timers) {
    // The retail collision dispatcher keys only on the objective item ID. It
    // has no game-type gate, which is observable in C&C's combined flag/zone
    // score schema. [orig: Entity_ProcessWaypointInteraction @0x4AD820;
    // GameType_CreateDefaultSettings @0x52DD00]
    ensure_objective_census(world);

    // Dropped flags count down to their authored home. A carried flag keeps
    // the timer armed but does not consume it.
    std::vector<EntityHandle> returns;
    for (CarryObjectiveState &state : carry_objectives_) {
        Entity *flag = world.registry.get(state.objective);
        if (flag == nullptr || flag->registry_spawn_id != state.spawn_id ||
            flag->primary_occupant.valid() || state.return_ticks <= 0 ||
            !advance_return_timers)
            continue;
        if (--state.return_ticks <= 0)
            returns.push_back(state.objective);
    }
    for (EntityHandle flag : returns)
        return_flag_home(world, flag, MatchGameplayEventKind::FlagReturn);

    std::vector<const MatchPlayer *> ordered;
    ordered.reserve(players_.size());
    for (const MatchPlayer &p : players_)
        ordered.push_back(&p);
    std::sort(ordered.begin(), ordered.end(), [](const MatchPlayer *a, const MatchPlayer *b) {
        return a->identity.slot < b->identity.slot;
    });

    std::vector<EntityHandle> objectives;
    world.registry.for_each([&](const Entity &entity) {
        // The movement resolver dispatches this callback only for a live
        // ItemDef carrying MoveCB and not Powerup. Preserve that target gate
        // here for both locally simulated and authority-snapped remote players.
        // [orig: Entity_MovementCollisionResolver @0x4B2F90..0x4B2FD0]
        const bool move_callback = entity.has_item_def &&
            (entity.item_attrib & kItemAttribMoveCallback) != 0 &&
            (entity.item_attrib & kItemAttribPowerup) == 0;
        if (move_callback &&
            (is_flag(entity.item_id) || is_flag_bay(entity.item_id)))
            objectives.push_back(entity.handle);
    });
    std::sort(objectives.begin(), objectives.end(),
              [](EntityHandle a, EntityHandle b) { return a.packed < b.packed; });

    for (const MatchPlayer *match_player : ordered) {
        Entity *carrier = world.registry.get(match_player->identity.entity);
        if (carrier == nullptr || !carrier->alive ||
            (carrier->flags & kEntityFlagDead) != 0 ||
            (carrier->net_move_input & Entity::kMoveOrderMoving) == 0)
            continue;

        // Retail only dispatches waypoint interactions from a successful
        // movement collision and repeats the MoveOrder bit-3 gate here.
        // [orig: Entity_MovementCollisionResolver ->
        // Entity_ProcessWaypointInteraction @0x4AD820]

        if (carrier->mounted_child.valid()) {
            Entity *flag = world.registry.get(carrier->mounted_child);
            if (flag == nullptr) {
                carrier->mounted_child = EntityHandle{};
                continue;
            }
            for (EntityHandle handle : objectives) {
                const Entity *bay = world.registry.get(handle);
                if (bay == nullptr || !is_flag_bay(bay->item_id) ||
                    !overlaps_objective(*carrier, *bay))
                    continue;
                const bool neutral = flag->item_id == kNeutralFlag;
                const bool accepted =
                    (bay->item_id == kBlueBay &&
                     (rules_.game_type == gt::kFlagMe || carrier->team == 1) &&
                     (flag->item_id == kRedFlag || neutral)) ||
                    (bay->item_id == kRedBay && carrier->team == 2 &&
                     (flag->item_id == kBlueFlag || neutral)) ||
                    (bay->item_id == kTeam3Bay && carrier->team == 3 && neutral) ||
                    (bay->item_id == kTeam4Bay && carrier->team == 4 && neutral);
                if (accepted) {
                    record_flag_capture(world, match_player->identity.entity,
                                        carrier->mounted_child);
                    break;
                }
            }
            continue;
        }

        for (EntityHandle handle : objectives) {
            Entity *flag = world.registry.get(handle);
            if (flag == nullptr || !is_flag(flag->item_id) ||
                flag->primary_occupant.valid() || !overlaps_objective(*carrier, *flag))
                continue;
            CarryObjectiveState *state = carry_state(world, handle);
            const bool own_flag = (carrier->team == 1 && flag->item_id == kBlueFlag) ||
                                  (carrier->team == 2 && flag->item_id == kRedFlag);
            if (own_flag && state != nullptr && state->return_ticks > 0) {
                record_flag_save(world, match_player->identity.entity, handle);
                break;
            }
            if (!own_flag || rules_.game_type == gt::kFlagBall ||
                rules_.game_type == gt::kFlagMe) {
                record_flag_pickup(world, match_player->identity.entity, handle);
                break;
            }
        }
    }
}

void Match::advance_tick(World &world) {
    if (outcome_.ended)
        return;
    ensure_objective_census(world);
    // The shared periodic service starts armed at zero, executes immediately,
    // then reloads 62 and decrements-before-testing on later simulation ticks.
    // KOTH accumulation and dropped-flag return callbacks both ride it.
    // [orig: g_periodic_second_timer in Server_TickUpdate @0x51D7E0;
    // Server_UpdateCaptureZoneProximity @0x5086A0; flag callback @0x408430]
    if (periodic_second_timer_ > 0)
        --periodic_second_timer_;
    const bool periodic_second = periodic_second_timer_ == 0;
    if (periodic_second) {
        periodic_second_timer_ = 62;
        update_hill_presence(world);
    }
    update_flag_objectives(world, periodic_second);
    if (remaining_ticks_ > 0)
        --remaining_ticks_;
}

std::vector<MatchGameplayEvent> Match::drain_gameplay_events() {
    std::vector<MatchGameplayEvent> out;
    out.swap(gameplay_events_);
    return out;
}

int32_t Match::primary_score(const MatchStats &stats, int32_t objective_ticks) const {
    // [orig: sub_52C850 @0x52C850]
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

int32_t Match::team_primary_score(const World &world, uint8_t team) const {
    if (team >= teams_.size())
        return 0;
    return primary_score(teams_[team], team_objective_ticks(world, team));
}

MatchLiveScoreboard Match::live_scoreboard(World &world) {
    MatchLiveScoreboard out;
    out.team_count = gt::active_team_count(rules_.game_type, rules_.team_count);
    out.team_mode = out.team_count != 0;
    // Bit 1 is literal game-type 1 only; team KOTH uses its per-team auxiliary
    // bytes instead. [orig: Server_BuildAndBroadcastScoreboard @0x50D960]
    out.timed_score_mode = rules_.game_type == gt::kKingOfTheHill;
    for (uint8_t team = 1; team <= out.team_count; ++team) {
        out.teams[team].primary_score = team_primary_score(world, team);
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
    uint8_t uniform_team = 0;
    for (const EntityHandle handle : world.zone_chain.zones) {
        const Entity *zone = world.registry.get(handle);
        if (zone == nullptr)
            continue;
        saw_zone = true;
        if (zone->team == 0) {
            uniform_team = 0;
            break;
        }
        if (uniform_team == 0)
            uniform_team = zone->team;
        else if (uniform_team != zone->team) {
            uniform_team = 0;
            break;
        }
    }
    if (saw_zone && uniform_team != 0)
        return static_cast<int32_t>(uniform_team);

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
        // expiry. [orig: Server_CheckWinConditions @0x51AE47]
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
        return unique_best_team(team_hold_ticks_);
    }

    if (rules_.game_type == gt::kCaptureTheFlag) {
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
        int32_t team1 = 0;
        int32_t team2 = 0;
        for (const EntityHandle handle : world.zone_chain.zones) {
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
    result_.team_row_count = scoreboard_team_row_count(rules_);
    if (rules_.game_type == gt::kAdvanceAndSecure ||
        rules_.game_type == gt::kConquerAndControl) {
        for (const EntityHandle handle : world.zone_chain.zones) {
            const Entity *zone = world.registry.get(handle);
            if (zone == nullptr)
                continue;
            if (zone->team == 1)
                ++result_.team_scores[0];
            else if (zone->team == 2)
                ++result_.team_scores[1];
        }
    } else {
        result_.team_scores[0] = team_primary_score(world, 1);
        result_.team_scores[1] = team_primary_score(world, 2);
    }
    result_.draw = result_.team_scores[0] == result_.team_scores[1];
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
    sort_scoreboard_players(result_.players);
    return true;
}

} // namespace opennova::world
