#include "world/match.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "world/world.h"

namespace opennova::world {
namespace {

constexpr uint32_t kGameTypeTeamDeathmatch = 0x10000u;
constexpr uint32_t kGameTypeTeamKingOfTheHill = 0x10001u;
constexpr uint32_t kGameTypeFlagBall = 0x10008u;
constexpr uint32_t kGameTypeAdvanceAndSecure = 0x10010u;
constexpr uint32_t kGameTypeConquerAndControl = 0x50010u;
constexpr uint32_t kWaypointFamilyMask = 0xFFFDFFFFu;
constexpr uint32_t kWaypointFamilyValue = 0x10020u;
constexpr int32_t kTicksPerMinute = 60 * 62;

bool is_waypoint_family(uint32_t game_type) {
    return (game_type & kWaypointFamilyMask) == kWaypointFamilyValue;
}

std::vector<MatchScoreField> make_default_score_fields(uint32_t game_type) {
    // These are the four target rows installed by
    // GameType_CreateDefaultSettings, including disabled columns and their
    // original order. score.ini can replace the whole row later.
    // [orig: GameType_CreateDefaultSettings @0x52DD00]
    if (game_type == kGameTypeTeamDeathmatch) {
        return {
            {19, 1}, {3, 1},  {2, 0},  {4, 1},  {1, 0},  {30, 1}, {10, 1},
            {11, 0}, {12, 0}, {13, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1},
        };
    }
    if (game_type == kGameTypeAdvanceAndSecure) {
        return {
            {19, 1}, {3, 1},  {2, 0},  {4, 1},  {1, 0},  {30, 1}, {10, 1}, {11, 0},
            {32, 1}, {12, 0}, {13, 1}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1},
        };
    }
    if (game_type == kGameTypeConquerAndControl) {
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
    if ((rules.game_type & 0x10000u) == 0)
        return 0;
    if (rules.team_count == 4 &&
        (rules.game_type == kGameTypeTeamDeathmatch ||
         rules.game_type == kGameTypeTeamKingOfTheHill || rules.game_type == kGameTypeFlagBall))
        return 5;
    return 3;
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
    const bool common_scoring = game_type == kGameTypeTeamDeathmatch ||
                                game_type == kGameTypeAdvanceAndSecure ||
                                game_type == kGameTypeConquerAndControl ||
                                is_waypoint_family(game_type);
    if (!common_scoring)
        return values;

    // The TDM and Co-op rows share these nonzero VAR defaults. A&S/CAC add
    // their zone-presence and takeover values below. Indices are scorer slots
    // 74+n and therefore match MatchRules::score_values directly.
    // [orig: GameType_CreateDefaultSettings @0x52DD00]
    values[3] = 5;   // ENEMYKILL
    values[6] = 1;   // MEDICHEAL
    values[7] = 2;   // MEDICSAVE
    values[15] = 12; // PSPTAKEOVER
    values[16] = 10; // MULTIPLEKILL
    values[17] = 5;  // HEADSHOTKILL
    values[18] = 1;  // KNIFEKILL
    values[37] = 5;  // VATTACHKILL
    if (game_type == kGameTypeAdvanceAndSecure ||
        game_type == kGameTypeConquerAndControl) {
        values[12] = 5;  // ZONEQUANTUM
        values[24] = 2;  // MEINMYZONEKILL
        values[26] = 2;  // MEINTHEIRZONEKILL
        values[31] = 1;  // INAZONE
        values[34] = 15; // LFPTAKEOVER
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
        return game_type == 1u || game_type == kGameTypeTeamKingOfTheHill ? stats[31]
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
    outcome_ = {};
    result_ = {};
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
        *by_slot = MatchPlayer{identity, {}};
        return;
    }
    players_.push_back(MatchPlayer{identity, {}});
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

int32_t Match::score_value(size_t status_index) const {
    return rules_.score_values.has_value() && status_index < rules_.score_values->size()
               ? (*rules_.score_values)[status_index]
               : 0;
}

void Match::add_event(MatchPlayer &player, size_t counter, int32_t points) {
    ++player.stats[counter];
    player.stats[MatchStats::kPoints] += points;
}

void Match::add_team_event(uint8_t team, size_t counter, int32_t points) {
    if (team >= teams_.size())
        return;
    ++teams_[team][counter];
    teams_[team][MatchStats::kPoints] += points;
}

void Match::record_death(const World &world, EntityHandle victim_handle,
                         EntityHandle killer_handle) {
    if (outcome_.ended)
        return;
    MatchPlayer *victim = player(victim_handle);
    if (victim == nullptr)
        return;
    const Entity *victim_entity = world.registry.get(victim_handle);
    if (victim_entity == nullptr)
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

void Match::record_numbered_zone_capture(const World &world,
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

void Match::advance_tick() {
    if (!outcome_.ended && remaining_ticks_ > 0)
        --remaining_ticks_;
}

int32_t Match::primary_score(const MatchStats &stats) const {
    // [orig: sub_52C850 @0x52C850]
    if (is_waypoint_family(rules_.game_type))
        return stats[MatchStats::kPoints];
    if (rules_.game_type == kGameTypeAdvanceAndSecure ||
        rules_.game_type == kGameTypeConquerAndControl)
        return stats[MatchStats::kZoneTakeovers];
    if (rules_.game_type == kGameTypeTeamDeathmatch || rules_.game_type == 0)
        return stats[MatchStats::kEnemyKills];
    return 0;
}

std::optional<int32_t> Match::winner_if_finished(const World &world) const {
    if (outcome_.ended)
        return std::nullopt;

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

    if (rules_.game_type == kGameTypeTeamDeathmatch) {
        // A zero score limit returns from the TDM arm before even considering
        // clock expiry. [orig: @0x51AE47]
        if (rules_.score_limit == 0)
            return std::nullopt;
        for (uint8_t team = 0; team < teams_.size(); ++team) {
            if (teams_[team][MatchStats::kEnemyKills] >= static_cast<int32_t>(rules_.score_limit))
                return static_cast<int32_t>(team);
        }
        if (remaining_ticks_ != 0)
            return std::nullopt;

        int32_t best = std::numeric_limits<int32_t>::min();
        int32_t winner = 0;
        bool tied = false;
        for (uint8_t team = 1; team <= 4; ++team) {
            const int32_t score = teams_[team][MatchStats::kEnemyKills];
            if (score > best) {
                best = score;
                winner = team;
                tied = false;
            } else if (score == best) {
                tied = true;
            }
        }
        return tied ? 0 : winner;
    }

    if ((rules_.game_type == kGameTypeAdvanceAndSecure ||
         rules_.game_type == kGameTypeConquerAndControl) &&
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
        // [orig: A&S clock-expiry tail @0x51B35B]
        if (team1 == team2)
            return 0;
        return team1 > team2 ? 1 : 2;
    }

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
    if (rules_.game_type == kGameTypeAdvanceAndSecure ||
        rules_.game_type == kGameTypeConquerAndControl) {
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
        result_.team_scores[0] = primary_score(teams_[1]);
        result_.team_scores[1] = primary_score(teams_[2]);
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
            primary_score(player.stats),
        });
    }
    sort_scoreboard_players(result_.players);
    return true;
}

} // namespace opennova::world
