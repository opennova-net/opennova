// Authoritative multiplayer match rules for every retail game type: score-event
// accounting, objective interaction, win decisions, the GameTime clock, and
// Co-op's script-owned outcome path.
// [orig: CPlayerStats_RecordEvent @0x52C8E0; GameEvent_ProcessScoring @0x52F550;
// Server_CheckWinConditions @0x51AD40; Server_ProcessRoundEnd @0x5164F0]
#include "world/match.h"
#include "world/game_type.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
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

namespace gt = opennova::game_type;

constexpr uint32_t kTdm = gt::kTeamDeathmatch;
constexpr uint32_t kAas = gt::kAdvanceAndSecure;
constexpr uint32_t kCoop = gt::kCoop;

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
    entity.has_item_def = true;
    entity.player_class = 8;
    entity.team = team;
    entity.health = 100;
    entity.alive = true;
    entity.flags = kEntityFlagPlayer;
    entity.engine_flags = kEntityFlagPlayer;
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

EntityHandle objective(World &world, int32_t item_id, uint8_t team, Vec3 position = {},
                       uint32_t item_attrib = kItemAttribMoveCallback) {
    Entity entity;
    entity.kind = EntityKind::Item;
    entity.item_id = item_id;
    entity.has_item_def = true;
    entity.item_attrib = item_attrib;
    entity.team = team;
    entity.position = position;
    entity.spawn_position = position;
    entity.bound_radius = 2.0f;
    entity.alive = true;
    return world.registry.spawn(1, entity);
}

EntityHandle demolition_target(World &world, uint8_t team) {
    Entity entity;
    entity.kind = EntityKind::Item;
    entity.item_id = 9000 + team;
    entity.team = team;
    entity.item_attrib = 0x8000u;
    entity.alive = true;
    return world.registry.spawn(1, entity);
}

void tick_to_zero(World &world) {
    for (int i = 0; i < 60 * 62; ++i)
        world.match.advance_tick(world);
}

void expect_fields(uint32_t game_type,
                   std::initializer_list<std::pair<uint8_t, uint8_t>> expected) {
    const std::vector<MatchScoreField> actual = default_match_score_fields(game_type);
    CHECK(actual.size() == expected.size());
    size_t i = 0;
    for (const auto &[field, enabled] : expected) {
        if (i >= actual.size())
            break;
        CHECK(actual[i].field == field);
        CHECK(actual[i].enabled == enabled);
        ++i;
    }
}

void test_every_retail_default_score_row() {
    // These are the complete FIELD rows installed before score.ini overlays.
    // [orig: GameType_CreateDefaultSettings @0x52DD00]
    expect_fields(gt::kDeathmatch,
                  {{19, 1}, {3, 1}, {4, 1}, {1, 0}, {15, 0}, {16, 0}, {17, 0},
                   {27, 0}, {21, 1}});
    expect_fields(gt::kTeamDeathmatch,
                  {{19, 1}, {3, 1}, {2, 0}, {4, 1}, {1, 0}, {30, 1}, {10, 1},
                   {11, 0}, {12, 0}, {13, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}});
    expect_fields(gt::kKingOfTheHill,
                  {{19, 1}, {5, 1}, {22, 1}, {18, 1}, {3, 1}, {4, 1}, {1, 0},
                   {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}});
    expect_fields(gt::kTeamKingOfTheHill,
                  {{19, 1}, {5, 1}, {22, 1}, {18, 1}, {3, 1}, {2, 0}, {4, 1},
                   {1, 0}, {30, 1}, {10, 1}, {11, 0}, {12, 0}, {13, 1}, {15, 0},
                   {16, 0}, {17, 0}, {27, 0}, {21, 1}});
    const auto demolition = std::initializer_list<std::pair<uint8_t, uint8_t>>{
        {19, 1}, {8, 1}, {3, 1}, {2, 0}, {4, 1}, {1, 0}, {30, 1}, {10, 1},
        {11, 0}, {12, 0}, {13, 1}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}};
    expect_fields(gt::kSearchAndDestroy, demolition);
    expect_fields(gt::kAttackDefend, demolition);
    expect_fields(gt::kCaptureTheFlag,
                  {{19, 1}, {6, 1}, {7, 1}, {14, 1}, {3, 1}, {2, 0}, {4, 1},
                   {1, 0}, {30, 1}, {10, 1}, {11, 0}, {12, 0}, {13, 0}, {15, 0},
                   {16, 0}, {17, 0}, {27, 0}, {21, 1}});
    expect_fields(gt::kFlagBall,
                  {{19, 1}, {6, 1}, {14, 1}, {29, 1}, {28, 1}, {3, 1}, {2, 0},
                   {4, 1}, {1, 0}, {30, 1}, {10, 1}, {11, 0}, {12, 0}, {13, 0},
                   {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}});
    expect_fields(gt::kAdvanceAndSecure,
                  {{19, 1}, {3, 1}, {2, 0}, {4, 1}, {1, 0}, {30, 1}, {10, 1},
                   {11, 0}, {32, 1}, {12, 0}, {13, 1}, {15, 0}, {16, 0}, {17, 0},
                   {27, 0}, {21, 1}});
    expect_fields(gt::kConquerAndControl,
                  {{19, 1}, {6, 1}, {14, 1}, {29, 1}, {28, 1}, {3, 1}, {2, 0},
                   {4, 1}, {1, 0}, {30, 1}, {10, 1}, {11, 0}, {32, 1}, {12, 0},
                   {13, 1}, {15, 0}, {16, 0}, {17, 0}, {27, 0}, {21, 1}});
    expect_fields(gt::kCoop,
                  {{19, 1}, {3, 1}, {4, 1}, {30, 1}, {10, 1}, {11, 1}, {21, 1}});
    expect_fields(gt::kObjectiveCoop,
                  {{19, 1}, {3, 1}, {4, 1}, {30, 1}, {10, 1}, {11, 1}, {21, 1}});
    CHECK(default_match_score_fields(gt::kFlagMe).empty());

    const auto dm = default_match_score_values(gt::kDeathmatch);
    CHECK(dm[3] == 5 && dm[16] == 10 && dm[37] == 5);
    const auto tkoth = default_match_score_values(gt::kTeamKingOfTheHill);
    CHECK(tkoth[3] == 2 && tkoth[12] == 10 && tkoth[21] == 5 && tkoth[33] == 1);
    const auto sd = default_match_score_values(gt::kSearchAndDestroy);
    CHECK(sd[13] == 50 && sd[24] == 2 && sd[26] == 2);
    const auto ad = default_match_score_values(gt::kAttackDefend);
    CHECK(ad[13] == 50 && ad[23] == 1 && ad[24] == 2 && ad[26] == 0);
    const auto ctf = default_match_score_values(gt::kCaptureTheFlag);
    CHECK(ctf[9] == 10 && ctf[10] == 20 && ctf[11] == 2 && ctf[20] == 5);
    const auto fb = default_match_score_values(gt::kFlagBall);
    CHECK(fb[10] == 40 && fb[11] == 2 && fb[20] == 5);
    const std::array<int32_t, 39> zero_values{};
    CHECK(default_match_score_values(gt::kFlagMe) == zero_values);

    CHECK(gt::score_table_index(gt::kDeathmatch) == 11);
    CHECK(gt::score_table_index(gt::kObjectiveCoop) == 2);
    CHECK(gt::score_table_index(gt::kCoop) == 0);
    CHECK(gt::score_table_index(gt::kFlagMe) == 12);
    CHECK(gt::has_score_table(gt::kCoop));
    CHECK(!gt::has_score_table(gt::kFlagMe));
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
        world->match.advance_tick(*world);
    const auto clock_winner = world->match.winner_if_finished(*world);
    CHECK(clock_winner.has_value() && *clock_winner == 1);

    world->match.configure(rules(kTdm, 1, 99));
    world->match.upsert_player({blue, 0, "Blue"});
    world->match.upsert_player({red, 1, "Red"});
    world->match.record_death(*world, red, blue);
    world->match.record_death(*world, blue, red);
    for (int i = 0; i < 60 * 62; ++i)
        world->match.advance_tick(*world);
    const auto draw = world->match.winner_if_finished(*world);
    CHECK(draw.has_value() && *draw == 0);

    // Retail returns from the entire TDM arm when score_limit is zero, even at t=0.
    // [orig: Server_CheckWinConditions @0x51AE47]
    world->match.configure(rules(kTdm, 1, 0));
    for (int i = 0; i < 60 * 62; ++i)
        world->match.advance_tick(*world);
    CHECK(!world->match.winner_if_finished(*world).has_value());
}

void test_deathmatch_and_hill_outcomes() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->registry.configure_pool(3, 4);

    MatchRules dm;
    dm.game_type = gt::kDeathmatch;
    dm.game_time_minutes = 1;
    dm.score_limit = 2;
    world->match.configure(dm);
    const EntityHandle solo = player(*world, 0, 1, "Solo");
    world->match.player(solo)->stats[MatchStats::kEnemyKills] = 2;
    const auto dm_limit = world->match.winner_if_finished(*world);
    CHECK(dm_limit.has_value() && *dm_limit == 0);

    world->match.configure(dm);
    world->match.upsert_player({solo, 0, "Solo"});
    tick_to_zero(*world);
    const auto dm_clock = world->match.winner_if_finished(*world);
    CHECK(dm_clock.has_value() && *dm_clock == 0);

    MatchRules koth;
    koth.game_type = gt::kKingOfTheHill;
    koth.game_time_minutes = 1;
    koth.hill_limit_minutes = 2;
    world->match.configure(koth);
    world->match.upsert_player({solo, 0, "Solo"});
    world->match.player(solo)->objective_ticks = 120;
    const auto koth_limit = world->match.winner_if_finished(*world);
    CHECK(koth_limit.has_value() && *koth_limit == 0);

    // The retail one-second service increments the persistent hill counter
    // while inside a type-6006 hill and decays it one service step outside.
    // Its timer starts at zero, so the first match tick runs immediately and
    // subsequent runs are exactly 62 ticks apart.
    // [orig: g_periodic_second_timer in Server_TickUpdate @0x51D7E0;
    // Server_UpdateCaptureZoneProximity @0x5086A0]
    world->match.configure(koth);
    world->match.upsert_player({solo, 0, "Solo"});
    world->registry.get(solo)->position = {0.0f, 0.0f, 0.0f};
    Entity hill;
    hill.kind = EntityKind::Item;
    hill.item_id = 6006;
    hill.position = {0.0f, 0.0f, 0.0f};
    hill.bound_radius = 10.0f;
    hill.alive = true;
    world->registry.spawn(3, hill);
    world->match.advance_tick(*world);
    CHECK(world->match.player(solo)->objective_ticks == 1);
    for (int i = 0; i < 61; ++i)
        world->match.advance_tick(*world);
    CHECK(world->match.player(solo)->objective_ticks == 1);
    world->match.advance_tick(*world);
    CHECK(world->match.player(solo)->objective_ticks == 2);
    world->registry.get(solo)->position = {100.0f, 0.0f, 0.0f};
    for (int i = 0; i < 61; ++i)
        world->match.advance_tick(*world);
    CHECK(world->match.player(solo)->objective_ticks == 2);
    world->match.advance_tick(*world);
    CHECK(world->match.player(solo)->objective_ticks == 1);

    MatchRules tkoth = koth;
    tkoth.game_type = gt::kTeamKingOfTheHill;
    tkoth.hill_limit_minutes = 1;
    world->match.configure(tkoth);
    const EntityHandle red = player(*world, 1, 2, "Red");
    world->match.upsert_player({solo, 0, "Solo"});
    world->registry.get(solo)->position = {0.0f, 0.0f, 0.0f};
    world->registry.get(red)->position = {100.0f, 0.0f, 0.0f};
    // One minute is 60 service steps, not 60 simulation ticks.
    for (int i = 0; i <= 59 * 62; ++i)
        world->match.advance_tick(*world);
    const auto tkoth_limit = world->match.winner_if_finished(*world);
    CHECK(tkoth_limit.has_value() && *tkoth_limit == 1);
    CHECK(world->match.primary_score(*world->match.player(solo)) == 60);
    CHECK(world->match.team_primary_score(*world, 1) == 60);
}

void test_demolition_flag_and_flagball_gameplay() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->registry.configure_pool(1, 16);

    const EntityHandle blue = player(*world, 0, 1, "Blue");
    const EntityHandle red = player(*world, 1, 2, "Red");

    for (const uint32_t game_type : {gt::kSearchAndDestroy, gt::kAttackDefend}) {
        MatchRules demolition;
        demolition.game_type = game_type;
        demolition.game_time_minutes = 1;
        world->match.configure(demolition);
        world->match.upsert_player({blue, 0, "Blue"});
        world->match.upsert_player({red, 1, "Red"});
        const EntityHandle blue_target = demolition_target(*world, 1);
        const EntityHandle red_target = demolition_target(*world, 2);
        world->match.advance_tick(*world); // freezes the authored target census
        world->match.record_target_destroyed(*world, red_target, blue);
        CHECK(world->match.player(blue)->stats[MatchStats::kTargetsDestroyed] == 1);
        CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 50);
        const auto winner = world->match.winner_if_finished(*world);
        CHECK(winner.has_value() && *winner == 1);
        world->registry.despawn(blue_target);
        if (world->registry.get(red_target) != nullptr)
            world->registry.despawn(red_target);
    }

    MatchRules ctf;
    ctf.game_type = gt::kCaptureTheFlag;
    ctf.game_time_minutes = 1;
    ctf.flag_return_ticks = 210;
    world->match.configure(ctf);
    world->match.upsert_player({blue, 0, "Blue"});
    world->match.upsert_player({red, 1, "Red"});
    world->registry.get(blue)->position = {0.0f, 0.0f, 0.0f};
    const EntityHandle blue_flag = objective(*world, 4091, 1, {80.0f, 0.0f, 0.0f});
    const EntityHandle red_flag = objective(*world, 4093, 2, {0.0f, 0.0f, 0.0f});
    const EntityHandle blue_bay = objective(*world, 4098, 1, {50.0f, 0.0f, 0.0f});
    world->match.advance_tick(*world);
    CHECK(world->registry.get(blue)->mounted_child == EntityHandle{});
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagPickups] == 0);

    // Retail reaches waypoint interaction through the movement collision
    // resolver and also requires MoveOrder bit 3. A stationary overlap cannot
    // pick up or capture a flag. [orig: Entity_ProcessWaypointInteraction
    // @0x4AD820, caller in Entity_MovementCollisionResolver]
    world->registry.get(blue)->net_move_input |= Entity::kMoveOrderMoving;
    world->match.advance_tick(*world);
    CHECK(world->registry.get(blue)->mounted_child == red_flag);
    CHECK(world->registry.get(red_flag)->primary_occupant == blue);
    CHECK((world->registry.get(red_flag)->flags & kEntityFlagCarried) != 0);
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagPickups] == 1);

    world->registry.get(blue)->position = {50.0f, 0.0f, 0.0f};
    world->match.advance_tick(*world);
    CHECK(world->registry.get(blue)->mounted_child == EntityHandle{});
    CHECK(world->registry.get(red_flag) == nullptr); // CTF consumes captured flags
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagCaptures] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 22);
    CHECK(world->match.team_stats(1)[MatchStats::kFlagCaptures] == 1);
    const auto ctf_winner = world->match.winner_if_finished(*world);
    CHECK(ctf_winner.has_value() && *ctf_winner == 1);
    const std::vector<MatchGameplayEvent> flag_events = world->match.drain_gameplay_events();
    CHECK(flag_events.size() == 2);
    CHECK(flag_events[0].kind == MatchGameplayEventKind::FlagPickup);
    CHECK(flag_events[1].kind == MatchGameplayEventKind::FlagCapture);
    CHECK(flag_events[1].objective == red_flag);
    CHECK(flag_events[1].remove_objective);
    world->registry.despawn(blue_flag);
    world->registry.despawn(blue_bay);

    // FlagReturnTime is counted by the same one-second entity service (the
    // flag callback rearms itself at spawnPhase 62), not once per sim tick.
    // [orig: flag update callback @0x408430; g_FlagReturnTime_2 @0x24D2174]
    MatchRules timed_return;
    timed_return.game_type = gt::kFlagBall;
    timed_return.flag_return_ticks = 5;
    world->match.configure(timed_return);
    world->match.upsert_player({blue, 0, "Blue"});
    Entity *blue_entity = world->registry.get(blue);
    blue_entity->alive = true;
    blue_entity->flags &= ~kEntityFlagDead;
    blue_entity->net_move_input |= Entity::kMoveOrderMoving;
    blue_entity->position = {10.0f, 0.0f, 0.0f};
    const EntityHandle timed_flag =
        objective(*world, 4095, 0, {10.0f, 0.0f, 0.0f});
    world->match.advance_tick(*world); // immediate service + per-tick pickup
    CHECK(blue_entity->mounted_child == timed_flag);
    blue_entity->position = {0.0f, 0.0f, 0.0f};
    world->match.record_death(*world, blue);
    blue_entity->alive = false;
    blue_entity->flags |= kEntityFlagDead;
    CHECK(world->registry.get(timed_flag)->position.x == 0.0f);
    for (int i = 0; i < 4 * 62; ++i)
        world->match.advance_tick(*world);
    CHECK(world->registry.get(timed_flag)->position.x == 0.0f);
    for (int i = 0; i < 62; ++i)
        world->match.advance_tick(*world);
    CHECK(world->registry.get(timed_flag)->position.x == 10.0f);
    world->registry.despawn(timed_flag);

    MatchRules flag_ball;
    flag_ball.game_type = gt::kFlagBall;
    flag_ball.game_time_minutes = 1;
    flag_ball.max_score = 2;
    world->match.configure(flag_ball);
    world->match.upsert_player({blue, 0, "Blue"});
    world->match.record_flag_capture(*world, blue, EntityHandle{});
    world->match.record_flag_capture(*world, blue, EntityHandle{});
    const auto flag_ball_winner = world->match.winner_if_finished(*world);
    CHECK(flag_ball_winner.has_value() && *flag_ball_winner == 1);

    MatchRules zero_flag_ball;
    zero_flag_ball.game_type = gt::kFlagBall;
    zero_flag_ball.max_score = 0;
    world->match.configure(zero_flag_ball);
    world->match.upsert_player({blue, 0, "Blue"});
    const auto zero_winner = world->match.winner_if_finished(*world);
    CHECK(zero_winner.has_value() && *zero_winner == 1);
}

void test_flag_me_keeps_retails_unreachable_score_arm() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->registry.configure_pool(1, 4);

    MatchRules rules;
    rules.game_type = gt::kFlagMe;
    rules.max_score = 1;
    rules.score_values.emplace();
    (*rules.score_values)[3] = 99;
    (*rules.score_values)[5] = -7;
    (*rules.score_values)[10] = 40;
    (*rules.score_values)[11] = 2;
    world->match.configure(rules);

    const EntityHandle carrier = player(*world, 0, 1, "Carrier");
    const EntityHandle victim = player(*world, 1, 1, "Victim");
    Entity *carrier_entity = world->registry.get(carrier);
    carrier_entity->position = {0.0f, 0.0f, 0.0f};
    carrier_entity->net_move_input = Entity::kMoveOrderMoving;
    const EntityHandle flag = objective(*world, 4095, 0, {0.0f, 0.0f, 0.0f});
    objective(*world, 4098, 1, {20.0f, 0.0f, 0.0f});

    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == flag);
    carrier_entity->position = {20.0f, 0.0f, 0.0f};
    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == EntityHandle{});
    CHECK(world->registry.get(flag) != nullptr);
    CHECK(world->registry.get(flag)->position.x == 0.0f);

    world->match.record_death(*world, victim, carrier);
    CHECK(world->match.player(carrier)->stats[MatchStats::kFlagPickups] == 0);
    CHECK(world->match.player(carrier)->stats[MatchStats::kFlagCaptures] == 0);
    CHECK(world->match.player(carrier)->stats[MatchStats::kEnemyKills] == 0);
    CHECK(world->match.player(carrier)->stats[MatchStats::kPoints] == 0);
    CHECK(world->match.player(victim)->stats[MatchStats::kDeaths] == 0);
    CHECK(!world->match.winner_if_finished(*world).has_value());

    const std::vector<MatchGameplayEvent> events =
        world->match.drain_gameplay_events();
    CHECK(events.size() == 2);
    CHECK(events[0].kind == MatchGameplayEventKind::FlagPickup);
    CHECK(events[1].kind == MatchGameplayEventKind::FlagCapture);
    CHECK(!events[1].remove_objective);

    // The retail win check still reads raw capture field 12; the ordinary
    // scorer simply has no valid row capable of incrementing it.
    world->match.player(carrier)->stats[MatchStats::kFlagCaptures] = 1;
    const auto injected_winner = world->match.winner_if_finished(*world);
    CHECK(injected_winner.has_value() && *injected_winner == 0);
}

void test_flag_contact_requires_the_retail_move_callback_gate() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->registry.configure_pool(1, 4);

    MatchRules rules;
    rules.game_type = gt::kFlagBall;
    world->match.configure(rules);
    const EntityHandle carrier = player(*world, 0, 1, "Carrier");
    Entity *carrier_entity = world->registry.get(carrier);
    carrier_entity->net_move_input = Entity::kMoveOrderMoving;

    const EntityHandle inert = objective(*world, 4095, 0, {}, 0);
    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == EntityHandle{});

    world->registry.get(inert)->item_attrib =
        kItemAttribMoveCallback | kItemAttribPowerup;
    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == EntityHandle{});

    world->registry.get(inert)->item_attrib = kItemAttribMoveCallback;
    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == inert);
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
        world->match.advance_tick(*world);
    const auto tied_zones = world->match.winner_if_finished(*world);
    CHECK(tied_zones.has_value() && *tied_zones == 0);

    world->registry.get(z1)->team = 2;
    const auto red_all_owned = world->match.winner_if_finished(*world);
    CHECK(red_all_owned.has_value() && *red_all_owned == 2);
}

void test_cac_combines_flag_and_zone_objectives() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->registry.configure_pool(1, 8);

    MatchRules cac;
    cac.game_type = gt::kConquerAndControl;
    cac.game_time_minutes = 1;
    world->match.configure(cac);
    const EntityHandle blue = player(*world, 0, 1, "Blue");
    Entity *blue_entity = world->registry.get(blue);
    blue_entity->position = {0.0f, 0.0f, 0.0f};
    blue_entity->net_move_input = Entity::kMoveOrderMoving;
    const EntityHandle flag =
        objective(*world, 4095, 0, {0.0f, 0.0f, 0.0f});
    objective(*world, 4098, 1, {20.0f, 0.0f, 0.0f});

    world->match.advance_tick(*world);
    CHECK(blue_entity->mounted_child == flag);
    blue_entity->position = {20.0f, 0.0f, 0.0f};
    world->match.advance_tick(*world);
    CHECK(blue_entity->mounted_child == EntityHandle{});
    CHECK(world->registry.get(flag) != nullptr);
    CHECK(world->registry.get(flag)->position.x == 0.0f);
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagCaptures] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 0);

    // C&C's primary score and win arm remain the zone counter even though its
    // retail FIELD row also exposes flag stats. [orig: sub_52C850 @0x52C850;
    // GameType_CreateDefaultSettings @0x52DD00]
    world->match.record_numbered_zone_capture(*world, {blue});
    CHECK(world->match.player(blue)->stats[MatchStats::kZoneTakeovers] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 15);
    CHECK(world->match.primary_score(*world->match.player(blue)) == 1);

    const std::vector<MatchGameplayEvent> events =
        world->match.drain_gameplay_events();
    CHECK(events.size() == 2);
    if (events.size() >= 2) {
        CHECK(events[0].kind == MatchGameplayEventKind::FlagPickup);
        CHECK(events[1].kind == MatchGameplayEventKind::FlagCapture);
    }
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
    for (const uint32_t game_type : {gt::kCoop, gt::kObjectiveCoop}) {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->match.configure(rules(game_type, 1, 50));
        const EntityHandle blue = player(*world, 0, 1, "Blue");
        const EntityHandle red = player(*world, 1, 2, "Red");
        world->match.record_death(*world, red, blue);
        for (int i = 0; i < 60 * 62; ++i)
            world->match.advance_tick(*world);
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
}

} // namespace

int main() {
    test_every_retail_default_score_row();
    test_retail_default_score_values();
    test_tdm_scoring_is_event_exact();
    test_live_entity_team_and_class_drive_scoring_and_board();
    test_tdm_limit_and_clock_decisions();
    test_deathmatch_and_hill_outcomes();
    test_demolition_flag_and_flagball_gameplay();
    test_flag_me_keeps_retails_unreachable_score_arm();
    test_flag_contact_requires_the_retail_move_callback_gate();
    test_aas_capture_scoring_and_outcomes();
    test_cac_combines_flag_and_zone_objectives();
    test_end_result_is_frozen_in_retail_board_order();
    test_coop_remains_script_owned();
    if (failures != 0) {
        std::printf("match_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("match_test: all checks passed");
    return 0;
}
