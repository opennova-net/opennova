// Authoritative multiplayer match rules: retail score-event accounting, TDM/A&S
// win decisions, the GameTime clock, and Co-op's script-owned outcome path.
// [orig: CPlayerStats_RecordEvent @0x52C8E0; GameEvent_ProcessScoring @0x52F550;
// Server_CheckWinConditions @0x51AD40; Server_ProcessRoundEnd @0x5164F0]
#include "world/match.h"
#include "world/world.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

namespace {

constexpr uint32_t kTdm = 0x10000u;
constexpr uint32_t kAas = 0x10010u;
constexpr uint32_t kCoop = 0x10020u;

MatchRules rules(uint32_t game_type, uint32_t game_time_minutes, uint32_t score_limit) {
    MatchRules out;
    out.game_type = game_type;
    out.game_time_minutes = game_time_minutes;
    out.score_limit = score_limit;
    // row+300..+452 is the 39-value status block; scorer slots 74+n read value n.
    // [orig: Server_BuildStatusReport @0x530A60; GameEvent_ProcessScoring @0x52F550]
    out.score_values.emplace();
    (*out.score_values)[2] = -5;  // team kill, scoringTable[76]
    (*out.score_values)[3] = 10;  // enemy player kill, scoringTable[77]
    (*out.score_values)[4] = -3;  // suicide, scoringTable[78]
    (*out.score_values)[5] = -2;  // death, scoringTable[79]
    (*out.score_values)[34] = 25; // numbered-zone takeover, scoringTable[108]
    return out;
}

EntityHandle player(World &world, uint8_t slot, uint8_t team, const char *name) {
    Entity entity;
    entity.kind = EntityKind::Organic;
    entity.item_id = 5305;
    entity.player_class = 8;
    entity.team = team;
    entity.health = 100;
    entity.alive = true;
    const EntityHandle handle = world.registry.spawn(0, entity);
    MatchPlayerIdentity identity;
    identity.entity = handle;
    identity.slot = slot;
    identity.name = name;
    world.match.upsert_player(identity);
    return handle;
}

EntityHandle zone(World &world, uint8_t number, uint8_t team) {
    Entity entity;
    entity.kind = EntityKind::Item;
    entity.item_id = 1359;
    entity.zone_number = number;
    entity.team = team;
    entity.is_capture_trigger = true;
    entity.alive = true;
    const EntityHandle handle = world.registry.spawn(1, entity);
    world.zone_chain.zones.push_back(handle);
    return handle;
}

void test_retail_default_score_values() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);

    MatchRules tdm_rules;
    tdm_rules.game_type = kTdm;
    world->match.configure(tdm_rules);
    const EntityHandle tdm_blue = player(*world, 0, 1, "TdmBlue");
    const EntityHandle tdm_red = player(*world, 1, 2, "TdmRed");
    world->match.record_death(*world, tdm_red, tdm_blue);
    CHECK(world->match.player(tdm_blue)->stats[MatchStats::kPoints] == 5);
    CHECK(world->match.player(tdm_red)->stats[MatchStats::kPoints] == 0);

    MatchRules aas_rules;
    aas_rules.game_type = kAas;
    world->match.configure(aas_rules);
    const EntityHandle aas_blue = player(*world, 0, 1, "AasBlue");
    world->match.record_numbered_zone_capture(*world, {aas_blue});
    CHECK(world->match.player(aas_blue)->stats[MatchStats::kPoints] == 15);

    MatchRules coop_rules;
    coop_rules.game_type = kCoop;
    world->match.configure(coop_rules);
    const EntityHandle coop_blue = player(*world, 0, 1, "CoopBlue");
    const EntityHandle coop_red = player(*world, 1, 2, "CoopRed");
    world->match.record_death(*world, coop_red, coop_blue);
    CHECK(world->match.player(coop_blue)->stats[MatchStats::kPoints] == 5);
}

void test_tdm_scoring_is_event_exact() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->match.configure(rules(kTdm, 10, 50));
    const EntityHandle blue = player(*world, 0, 1, "Blue");
    const EntityHandle blue2 = player(*world, 1, 1, "BlueTwo");
    const EntityHandle red = player(*world, 2, 2, "Red");

    world->match.record_death(*world, red, blue);
    CHECK(world->match.player(blue)->stats[MatchStats::kEnemyKills] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 10);
    CHECK(world->match.player(red)->stats[MatchStats::kDeaths] == 1);
    CHECK(world->match.player(red)->stats[MatchStats::kPoints] == -2);
    CHECK(world->match.team_stats(1)[MatchStats::kEnemyKills] == 1);
    CHECK(world->match.team_stats(2)[MatchStats::kDeaths] == 1);

    // A team kill still records the victim's death/death points, then the attacker's
    // team-kill event/penalty. [orig: GameEvent_PlayerDeath @0x516EFF/@0x516FB0]
    world->match.record_death(*world, blue2, blue);
    CHECK(world->match.player(blue)->stats[MatchStats::kTeamKills] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 5);
    CHECK(world->match.player(blue2)->stats[MatchStats::kDeaths] == 1);
    CHECK(world->match.team_stats(1)[MatchStats::kTeamKills] == 1);

    // Suicide is two scorer calls in retail: death first, suicide second.
    world->match.record_death(*world, blue, blue);
    CHECK(world->match.player(blue)->stats[MatchStats::kDeaths] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kSuicides] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 0);
}

void test_live_entity_team_and_class_drive_scoring_and_board() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->match.configure(rules(kTdm, 10, 50));
    const EntityHandle blue = player(*world, 0, 1, "Blue");
    const EntityHandle red = player(*world, 1, 2, "Red");

    // Team/class are mutable entity state, not roster identity. Retail reads
    // entity+354 while scoring and entity+660 while freezing the board.
    // [orig: GameEvent_ProcessScoring @0x52F550;
    // Server_BuildEndOfRoundScoreboard @0x508F30]
    world->registry.get(blue)->team = 2;
    world->registry.get(blue)->player_class = 9;
    world->match.record_death(*world, red, blue);
    CHECK(world->match.player(blue)->stats[MatchStats::kTeamKills] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kEnemyKills] == 0);
    CHECK(world->match.team_stats(2)[MatchStats::kTeamKills] == 1);
    CHECK(world->match.team_stats(1)[MatchStats::kTeamKills] == 0);

    world->process_round_end(2);
    const MatchResult &result = world->match.result();
    CHECK(result.players.size() == 2);
    const auto changed =
        std::find_if(result.players.begin(), result.players.end(),
                     [blue](const MatchResultPlayer &row) { return row.identity.entity == blue; });
    CHECK(changed != result.players.end());
    if (changed != result.players.end()) {
        CHECK(changed->team == 2);
        CHECK(changed->player_class == 9);
        CHECK(changed->stats[MatchStats::kRoundMarker] == 2);
    }
}

void test_tdm_limit_and_clock_decisions() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->match.configure(rules(kTdm, 1, 2));
    const EntityHandle blue = player(*world, 0, 1, "Blue");
    const EntityHandle red = player(*world, 1, 2, "Red");

    world->match.record_death(*world, red, blue);
    CHECK(!world->match.winner_if_finished(*world).has_value());
    world->match.record_death(*world, red, blue);
    const auto limit_winner = world->match.winner_if_finished(*world);
    CHECK(limit_winner.has_value() && *limit_winner == 1);

    world->match.configure(rules(kTdm, 1, 99));
    world->match.upsert_player({blue, 0, "Blue"});
    world->match.upsert_player({red, 1, "Red"});
    world->match.record_death(*world, red, blue);
    for (int i = 0; i < 60 * 62; ++i)
        world->match.advance_tick();
    const auto clock_winner = world->match.winner_if_finished(*world);
    CHECK(clock_winner.has_value() && *clock_winner == 1);

    world->match.configure(rules(kTdm, 1, 99));
    world->match.upsert_player({blue, 0, "Blue"});
    world->match.upsert_player({red, 1, "Red"});
    world->match.record_death(*world, red, blue);
    world->match.record_death(*world, blue, red);
    for (int i = 0; i < 60 * 62; ++i)
        world->match.advance_tick();
    const auto draw = world->match.winner_if_finished(*world);
    CHECK(draw.has_value() && *draw == 0);

    // Retail returns from the entire TDM arm when score_limit is zero, even at t=0.
    // [orig: Server_CheckWinConditions @0x51AE47]
    world->match.configure(rules(kTdm, 1, 0));
    for (int i = 0; i < 60 * 62; ++i)
        world->match.advance_tick();
    CHECK(!world->match.winner_if_finished(*world).has_value());
}

void test_aas_capture_scoring_and_outcomes() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->registry.configure_pool(1, 8);
    world->match.configure(rules(kAas, 1, 0));
    const EntityHandle blue = player(*world, 0, 1, "Blue");
    const EntityHandle blue2 = player(*world, 1, 1, "BlueTwo");
    player(*world, 2, 2, "Red");

    // CaptureZone_CheckProximityScoring awards event 24 to every living teammate
    // inside a numbered zone, so the team row advances once per scorer.
    world->match.record_numbered_zone_capture(*world, {blue, blue2});
    CHECK(world->match.player(blue)->stats[MatchStats::kZoneTakeovers] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 25);
    CHECK(world->match.player(blue2)->stats[MatchStats::kZoneTakeovers] == 1);
    CHECK(world->match.team_stats(1)[MatchStats::kZoneTakeovers] == 2);
    CHECK(world->match.team_stats(1)[MatchStats::kPoints] == 50);

    const EntityHandle z1 = zone(*world, 1, 1);
    const EntityHandle z2 = zone(*world, 2, 1);
    const auto all_owned = world->match.winner_if_finished(*world);
    CHECK(all_owned.has_value() && *all_owned == 1);

    world->registry.get(z2)->team = 2;
    for (int i = 0; i < 60 * 62; ++i)
        world->match.advance_tick();
    const auto tied_zones = world->match.winner_if_finished(*world);
    CHECK(tied_zones.has_value() && *tied_zones == 0);

    world->registry.get(z1)->team = 2;
    const auto red_all_owned = world->match.winner_if_finished(*world);
    CHECK(red_all_owned.has_value() && *red_all_owned == 2);
}

void test_end_result_is_frozen_in_retail_board_order() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->match.configure(rules(kTdm, 10, 5));
    const EntityHandle blue = player(*world, 2, 1, "Blue");
    const EntityHandle red = player(*world, 1, 2, "Red");
    world->match.record_death(*world, red, blue);
    world->process_round_end(1);

    const MatchResult &tdm = world->match.result();
    CHECK(tdm.ready && tdm.winner_team == 1 && !tdm.draw);
    CHECK(tdm.team_scores[0] == 1 && tdm.team_scores[1] == 0);
    CHECK(tdm.players.size() == 2 && tdm.players[0].identity.slot == 2);
    CHECK(tdm.players[0].stats[MatchStats::kRoundMarker] == 2);
    CHECK(tdm.score_fields.size() == 14);
    CHECK(tdm.score_fields[0].field == 19 && tdm.score_fields[0].enabled);
    CHECK(tdm.score_fields[1].field == 3 && tdm.score_fields[1].enabled);
    CHECK(tdm.score_fields[2].field == 2 && !tdm.score_fields[2].enabled);
    CHECK(tdm.score_fields.back().field == 21 && tdm.score_fields.back().enabled);
    CHECK(tdm.team_row_count == 3);
    CHECK(match_score_field_value(tdm.players[0].stats, 19, kTdm) == 10);
    CHECK(match_score_field_value(tdm.players[0].stats, 3, kTdm) == 1);

    // The board is immutable once the round-end latch fires.
    world->match.remove_player(blue);
    world->process_round_end(2);
    CHECK(world->match.result().players.size() == 2);
    CHECK(world->match.result().winner_team == 1);

    world = std::make_unique<World>();
    world->registry.configure_pool(1, 8);
    world->match.configure(rules(kAas, 10, 0));
    zone(*world, 1, 1);
    zone(*world, 2, 2);
    world->process_round_end(0);
    const MatchResult &aas = world->match.result();
    CHECK(aas.team_scores[0] == 1 && aas.team_scores[1] == 1);
    CHECK(aas.draw);
    CHECK(aas.score_fields.size() == 16);
    CHECK(aas.score_fields[8].field == 32 && aas.score_fields[8].enabled);
    CHECK(aas.team_row_count == 3);
}

void test_coop_remains_script_owned() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->match.configure(rules(kCoop, 1, 50));
    const EntityHandle blue = player(*world, 0, 1, "Blue");
    const EntityHandle red = player(*world, 1, 2, "Red");
    world->match.record_death(*world, red, blue);
    for (int i = 0; i < 60 * 62; ++i)
        world->match.advance_tick();
    CHECK(world->match.remaining_ticks() == -1); // waypoint Co-op never seeds GameTime
    CHECK(!world->match.winner_if_finished(*world).has_value());
    world->process_round_end(1); // the WAC/BMS path used by cooperative missions
    CHECK(world->match.outcome().ended);
    CHECK(world->match.outcome().winner_team == 1);
    CHECK(world->match.result().team_scores[0] == 10);
    CHECK(world->match.result().team_scores[1] == -2);
    CHECK(!world->match.result().draw);
    CHECK(world->match.result().players[0].primary_score == 10);
    CHECK(world->match.result().score_fields.size() == 7);
    CHECK(world->match.result().score_fields.front().field == 19 &&
          world->match.result().score_fields.front().enabled);
    CHECK(world->match.result().score_fields.back().field == 21 &&
          world->match.result().score_fields.back().enabled);
    world->process_round_end(2);
    CHECK(world->match.outcome().winner_team == 1); // shared double-run latch
}

} // namespace

int main() {
    test_retail_default_score_values();
    test_tdm_scoring_is_event_exact();
    test_live_entity_team_and_class_drive_scoring_and_board();
    test_tdm_limit_and_clock_decisions();
    test_aas_capture_scoring_and_outcomes();
    test_end_result_is_frozen_in_retail_board_order();
    test_coop_remains_script_owned();
    if (failures != 0) {
        std::printf("match_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("match_test: all checks passed");
    return 0;
}
