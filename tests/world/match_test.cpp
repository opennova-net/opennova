// Authoritative multiplayer match rules for every retail game type: score-event
// accounting, objective interaction, win decisions, the GameTime clock, and
// Co-op's script-owned outcome path.
// [orig: CPlayerStats_RecordEvent @0x52C8E0; GameEvent_ProcessScoring @0x52F550;
// Server_CheckWinConditions @0x51AD40; Server_ProcessRoundEnd @0x5164F0]
#include <runtime/world/match.h>
#include <runtime/world/collision.h>
#include <base/gameprofile/game_type.h>
#include <runtime/world/world.h>

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
    world.zones.chain.zones.push_back(handle);
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
    entity.yaw = 90;
    entity.alive = true;
    return world.registry.spawn(1, entity);
}

CollisionModel waypoint_contact_model() {
    CollisionModel model;
    auto plane = [&](int nx, int ny, int nz, float distance) {
        CollisionPlane value;
        value.nx = static_cast<int16_t>(nx);
        value.ny = static_cast<int16_t>(ny);
        value.nz = static_cast<int16_t>(nz);
        value.dist = static_cast<int32_t>(distance * 65536.0f);
        model.planes.push_back(value);
    };
    plane(16384, 0, 0, -2.0f);
    plane(-16384, 0, 0, -2.0f);
    plane(0, 16384, 0, -2.0f);
    plane(0, -16384, 0, -2.0f);
    plane(0, 0, 16384, -3.0f);
    plane(0, 0, -16384, 0.0f);

    CollisionVolume volume;
    volume.type = 1;
    volume.min_x = volume.min_y = -2 * 65536;
    volume.max_x = volume.max_y = 2 * 65536;
    volume.min_z = 0;
    volume.max_z = 3 * 65536;
    volume.plane_count = 6;
    model.volumes.push_back(volume);

    CollisionSection section;
    section.volume_count = 1;
    model.sections.push_back(section);
    return model;
}

struct WaypointContactHarness {
    CollisionWorld collision;
    int32_t model_id = -1;

    explicit WaypointContactHarness(World &world) {
        model_id = collision.add_model(waypoint_contact_model());
        world.collision = &collision;
    }

    void bind(EntityHandle target) {
        collision.assign_entity(target, model_id);
    }

    void touch(World &world, EntityHandle source, EntityHandle target,
               bool authority = true) {
        Entity *source_entity = world.registry.get(source);
        const Entity *target_entity = world.registry.get(target);
        CHECK(source_entity != nullptr && target_entity != nullptr);
        if (source_entity == nullptr || target_entity == nullptr)
            return;
        source_entity->position = {
            target_entity->position.x + 1.6f,
            target_entity->position.y,
            target_entity->position.z,
        };
        for (int i = 0; i < 17; ++i)
            collision.build_tick_tables(world);

        CollisionWorld::ResolveState state;
        state.prev_valid = true;
        state.prev_pos[0] = static_cast<int32_t>(
            (target_entity->position.x + 3.5f) * 65536.0f);
        state.prev_pos[1] = static_cast<int32_t>(
            target_entity->position.y * 65536.0f);
        state.prev_pos[2] = static_cast<int32_t>(
            target_entity->position.z * 65536.0f);
        int32_t pos[3] = {
            static_cast<int32_t>(source_entity->position.x * 65536.0f),
            static_cast<int32_t>(source_entity->position.y * 65536.0f),
            static_cast<int32_t>(source_entity->position.z * 65536.0f),
        };
        int32_t velocity[3] = {};
        int16_t health = source_entity->health;
        collision.resolve_entity(world, source, state, pos, velocity,
                                 velocity[2], 0, 2 * 65536, 0, 0, true,
                                 authority, 0, 43, 1u, health);
    }
};

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

void advance_initial_periodic_passes(World &world, int passes) {
    if (passes <= 0)
        return;
    world.match.advance_tick(world);
    for (int pass = 1; pass < passes; ++pass) {
        for (int tick = 0; tick < 62; ++tick)
            world.match.advance_tick(world);
    }
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
    world->match.record_zone_capture(*world, {aas_blue});
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
    hill.has_item_def = true;
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
    CHECK(world->match.team_primary_score(1) == 60);
}

void test_retail_objective_proximity_state_and_kill_bonuses() {
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->registry.configure_pool(1, 8);
        MatchRules tdm = rules(kTdm, 10, 99);
        (*tdm.score_values)[21] = 7;  // victim near neutral objective
        (*tdm.score_values)[22] = 11; // attacker near neutral objective
        world->match.configure(tdm);
        const EntityHandle blue = player(*world, 0, 1, "Blue");
        const EntityHandle red = player(*world, 1, 2, "Red");
        world->registry.get(blue)->position = {0.0f, 0.0f, 0.0f};
        world->registry.get(red)->position = {5.0f, 0.0f, 0.0f};
        objective(*world, 4095, 0, {0.0f, 0.0f, 0.0f});

        // The one-second proximity service runs for every game type, not only
        // KOTH. A neutral objective within 20 units sets bit 0 on both slots;
        // the subsequent enemy kill awards scorer events 21 and 22.
        // [orig: Server_UpdateCaptureZoneProximity @0x5086A0;
        // GameEvent_ProcessScoring @0x52F550]
        world->match.advance_tick(*world);
        world->match.record_death(*world, red, blue);
        const MatchPlayer *scorer = world->match.player(blue);
        CHECK(scorer->stats[MatchStats::kVictimNearNeutralObjectiveKills] == 1);
        CHECK(scorer->stats[MatchStats::kAttackerNearNeutralObjectiveKills] == 1);
        CHECK(scorer->stats[MatchStats::kPoints] == 28);
        CHECK(world->match.team_stats(1)[MatchStats::kVictimNearNeutralObjectiveKills] == 1);
        CHECK(world->match.team_stats(1)[MatchStats::kAttackerNearNeutralObjectiveKills] == 1);
    }

    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->registry.configure_pool(1, 8);
        MatchRules tdm = rules(kTdm, 10, 99);
        (*tdm.score_values)[23] = 3;
        (*tdm.score_values)[24] = 4;
        (*tdm.score_values)[25] = 5;
        (*tdm.score_values)[26] = 6;
        world->match.configure(tdm);
        const EntityHandle blue = player(*world, 0, 1, "Blue");
        const EntityHandle red = player(*world, 1, 2, "Red");
        world->registry.get(blue)->position = {0.0f, 0.0f, 0.0f};
        world->registry.get(red)->position = {0.0f, 0.0f, 0.0f};
        objective(*world, 4091, 1, {0.0f, 0.0f, 0.0f});
        objective(*world, 4093, 2, {0.0f, 0.0f, 0.0f});

        // Team-objective bits are literal 1<<team masks. With both players by
        // both flags, all four attacker/victim x own/enemy scorer cases fire.
        // [orig: Server_UpdateCaptureZoneProximity @0x5086A0;
        // GameEvent_ProcessScoring @0x52F550]
        world->match.advance_tick(*world);
        world->match.record_death(*world, red, blue);
        const MatchPlayer *scorer = world->match.player(blue);
        CHECK(scorer->stats[MatchStats::kVictimNearAttackerObjectiveKills] == 1);
        CHECK(scorer->stats[MatchStats::kAttackerNearOwnObjectiveKills] == 1);
        CHECK(scorer->stats[MatchStats::kVictimNearOwnObjectiveKills] == 1);
        CHECK(scorer->stats[MatchStats::kAttackerNearVictimObjectiveKills] == 1);
        CHECK(scorer->stats[MatchStats::kPoints] == 28);
    }

    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->registry.configure_pool(1, 4);
        world->registry.configure_pool(3, 4);
        MatchRules koth;
        koth.game_type = gt::kKingOfTheHill;
        world->match.configure(koth);
        const EntityHandle solo = player(*world, 0, 1, "Solo");
        MatchPlayer *state = world->match.player(solo);
        state->objective_ticks = 3;

        // With neither kind of capture source in the mission retail skips the
        // capture counters entirely; absence does not mean "outside."
        // [orig: Server_UpdateCaptureZoneProximity @0x5088F6..0x50890A]
        world->match.advance_tick(*world);
        CHECK(state->objective_ticks == 3);
    }

    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->registry.configure_pool(1, 4);
        world->registry.configure_pool(3, 4);
        MatchRules koth;
        koth.game_type = gt::kKingOfTheHill;
        world->match.configure(koth);
        const EntityHandle solo = player(*world, 0, 1, "Solo");
        world->registry.get(solo)->position = {0.0f, 0.0f, 0.0f};
        world->match.player(solo)->objective_ticks = 3;

        Entity hill;
        hill.kind = EntityKind::Item;
        hill.item_id = 6006;
        hill.has_item_def = true;
        hill.position = {0.0f, 0.0f, 0.0f};
        hill.bound_radius = 10.0f;
        hill.alive = true;
        world->registry.spawn(3, hill);

        Entity numbered;
        numbered.kind = EntityKind::Item;
        numbered.has_item_def = true;
        numbered.item_attrib = kItemAttribSpawnPoint;
        numbered.zone_number = 1;
        numbered.zone_radius = 10;
        numbered.position = {100.0f, 0.0f, 0.0f};
        numbered.alive = true;
        world->registry.spawn(1, numbered);

        // The existence of any numbered capturable entity globally supersedes
        // every type-6006 volume. This player is therefore outside and decays,
        // despite standing in the hill trigger.
        // [orig: Server_UpdateCaptureZoneProximity @0x508869..0x50890A]
        world->match.advance_tick(*world);
        CHECK(world->match.player(solo)->objective_ticks == 2);
    }
}

void test_retail_objective_proximity_scoring_events() {
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->registry.configure_pool(3, 4);
        MatchRules koth;
        koth.game_type = gt::kKingOfTheHill;
        world->match.configure(koth);
        const EntityHandle solo = player(*world, 0, 0, "Solo");
        world->registry.get(solo)->position = {0.0f, 0.0f, 0.0f};

        Entity hill;
        hill.kind = EntityKind::Item;
        hill.item_id = 6006;
        hill.has_item_def = true;
        hill.position = {0.0f, 0.0f, 0.0f};
        hill.bound_radius = 10.0f;
        hill.alive = true;
        world->registry.spawn(3, hill);

        // Event 18 fires only when the capture counter reaches status value
        // 12. It records that complete interval in raw stat 31 and awards
        // status value 33; the first four one-second passes award nothing.
        // [orig: threshold/call @0x508C67..0x508CEA; scorer case 18
        // @0x5307B8..0x530821]
        advance_initial_periodic_passes(*world, 4);
        CHECK(world->match.player(solo)->objective_ticks == 4);
        CHECK(world->match.player(solo)->stats[MatchStats::kHillTime] == 0);
        CHECK(world->match.player(solo)->stats[MatchStats::kPoints] == 0);
        for (int tick = 0; tick < 62; ++tick)
            world->match.advance_tick(*world);
        CHECK(world->match.player(solo)->objective_ticks == 5);
        CHECK(world->match.player(solo)->stats[MatchStats::kHillTime] == 5);
        CHECK(world->match.player(solo)->stats[MatchStats::kPoints] == 1);
        CHECK(world->match.team_stats(1)[MatchStats::kHillTime] == 0);
    }

    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->registry.configure_pool(1, 4);
        MatchRules aas;
        aas.game_type = gt::kAdvanceAndSecure;
        aas.score_values = default_match_score_values(aas.game_type);
        (*aas.score_values)[32] = 3;
        world->match.configure(aas);
        const EntityHandle blue = player(*world, 0, 1, "Blue");
        world->registry.get(blue)->position = {0.0f, 0.0f, 0.0f};

        Entity numbered;
        numbered.kind = EntityKind::Item;
        numbered.has_item_def = true;
        numbered.item_attrib = kItemAttribSpawnPoint;
        numbered.zone_number = 1;
        numbered.zone_radius = 10;
        numbered.team = 2;
        numbered.position = {0.0f, 0.0f, 0.0f};
        numbered.alive = true;
        const EntityHandle zone_handle = world->registry.spawn(1, numbered);

        // A hostile owner bit selects event 19: raw stat 32/status 12 and
        // points/status 31. Five passes are one complete default interval.
        // [orig: branch/call @0x508C95..0x508CB1; scorer case 19
        // @0x530824..0x53088D]
        advance_initial_periodic_passes(*world, 5);
        CHECK(world->match.player(blue)->stats[MatchStats::kHostileZoneTime] == 5);
        CHECK(world->match.player(blue)->stats[MatchStats::kFriendlyZoneTime] == 0);
        CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 1);
        CHECK(world->match.team_stats(1)[MatchStats::kHostileZoneTime] == 5);

        // A friendly owner bit selects event 20. Retail records stat 33 on
        // the player but stat 32 on the team; preserve that observable quirk.
        // [orig: branch/call @0x508CBE..0x508CEA; scorer case 20 player/team
        // split @0x530890..0x5308F9]
        world->registry.get(zone_handle)->team = 1;
        for (int tick = 0; tick < 5 * 62; ++tick)
            world->match.advance_tick(*world);
        CHECK(world->match.player(blue)->stats[MatchStats::kHostileZoneTime] == 5);
        CHECK(world->match.player(blue)->stats[MatchStats::kFriendlyZoneTime] == 5);
        CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 4);
        CHECK(world->match.team_stats(1)[MatchStats::kHostileZoneTime] == 10);
        CHECK(world->match.team_stats(1)[MatchStats::kFriendlyZoneTime] == 0);
        CHECK(world->match.team_stats(1)[MatchStats::kPoints] == 4);
    }

    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        MatchRules tdm = rules(kTdm, 1, 99);
        (*tdm.score_values)[35] = 7;
        (*tdm.score_values)[36] = 3;
        world->match.configure(tdm);
        const EntityHandle blue = player(*world, 0, 1, "Blue");

        // The independent live-player counter calls event 25 at each status
        // value 36 interval. It records that interval in raw stat 40, awards
        // status value 35, and deliberately has no team-stat leg.
        // [orig: interval/call @0x5087C9..0x5087F1; scorer case 25
        // @0x530968..0x530990]
        advance_initial_periodic_passes(*world, 2);
        CHECK(world->match.player(blue)->stats[MatchStats::kPeriodicScoreUnits] == 0);
        CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 0);
        for (int tick = 0; tick < 62; ++tick)
            world->match.advance_tick(*world);
        CHECK(world->match.player(blue)->stats[MatchStats::kPeriodicScoreUnits] == 3);
        CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 7);
        CHECK(world->match.team_stats(1)[MatchStats::kPeriodicScoreUnits] == 0);
        CHECK(world->match.team_stats(1)[MatchStats::kPoints] == 0);
    }
}

void test_retail_zone_and_tkoth_win_quirks() {
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(1, 4);
        MatchRules aas;
        aas.game_type = gt::kAdvanceAndSecure;
        aas.game_time_minutes = 1;
        world->match.configure(aas);
        zone(*world, 1, 0);
        zone(*world, 2, 0);
        tick_to_zero(*world);

        // A uniform neutral chain makes the retail helper return true with
        // outTeamId=0. Server_CheckWinConditions then returns without ending
        // the round, so the ordinary A&S clock-expiry draw arm is unreachable.
        // [orig: ZoneSlotChain_GetWinningTeamIfAllOwned @0x4A2920;
        // Server_CheckWinConditions @0x51AD8A..0x51ADA3]
        CHECK(!world->match.winner_if_finished(*world).has_value());
    }

    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        world->registry.configure_pool(3, 4);
        MatchRules tkoth;
        tkoth.game_type = gt::kTeamKingOfTheHill;
        tkoth.game_time_minutes = 1;
        tkoth.hill_limit_minutes = 99;
        world->match.configure(tkoth);
        const EntityHandle team4 = player(*world, 0, 4, "TeamFour");
        world->registry.get(team4)->position = {0.0f, 0.0f, 0.0f};
        Entity hill;
        hill.kind = EntityKind::Item;
        hill.item_id = 6006;
        hill.has_item_def = true;
        hill.position = {0.0f, 0.0f, 0.0f};
        hill.bound_radius = 10.0f;
        hill.alive = true;
        world->registry.spawn(3, hill);
        tick_to_zero(*world);

        // The retail timeout comparison for a unique team-4 lead jumps to
        // LABEL_95, the team-1 round-end label. Preserve that observable bug;
        // the earlier hill-limit arm still reports team 4 normally.
        // [orig: Server_CheckWinConditions @0x51B01A..0x51B040]
        const auto timeout_winner = world->match.winner_if_finished(*world);
        CHECK(timeout_winner.has_value() && *timeout_winner == 1);
    }
}

// Event 11's occupant awards run through event 28, which recurses through both
// occupant links: the first link takes bonus>>1, the second the nested
// (bonus>>1)>>1 plus the direct bonus>>2 with TWO event-27 counts.
// [orig: GameEvent_ProcessScoring @0x52fa61..0x52fb0e;
// CPlayerStats_RecordEvent @0x52caf8..0x52cbb3]
void test_target_destroyed_shares_through_both_occupant_links() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->registry.configure_pool(1, 16);
    const EntityHandle blue = player(*world, 0, 1, "Blue");
    const EntityHandle first = player(*world, 1, 1, "First");
    const EntityHandle second = player(*world, 2, 1, "Second");
    world->registry.get(blue)->primary_occupant = first;
    world->registry.get(first)->primary_occupant = second;

    MatchRules demolition;
    demolition.game_type = gt::kSearchAndDestroy;
    demolition.game_time_minutes = 1;
    world->match.configure(demolition);
    world->match.upsert_player({blue, 0, "Blue"});
    world->match.upsert_player({first, 1, "First"});
    world->match.upsert_player({second, 2, "Second"});
    const EntityHandle red_target = demolition_target(*world, 2);
    world->match.advance_tick(*world); // freezes the authored target census
    world->match.record_target_destroyed(*world, red_target, blue);
    const int32_t bonus = 50; // scoringTable[87] for the demolition family
    CHECK(world->match.player(blue)->stats[MatchStats::kTargetsDestroyed] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == bonus);
    CHECK(world->match.player(first)->stats[MatchStats::kPoints] == (bonus >> 1));
    CHECK(world->match.player(first)->stats[MatchStats::kSharedPointAwards] == 1);
    CHECK(world->match.player(second)->stats[MatchStats::kPoints] ==
          ((bonus >> 1) >> 1) + (bonus >> 2));
    CHECK(world->match.player(second)->stats[MatchStats::kSharedPointAwards] == 2);
}

void test_demolition_flag_and_flagball_gameplay() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->registry.configure_pool(1, 16);
    WaypointContactHarness contacts(*world);

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
    contacts.bind(red_flag);
    contacts.bind(blue_bay);
    world->match.advance_tick(*world);
    CHECK(world->registry.get(blue)->mounted_child == EntityHandle{});
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagPickups] == 0);

    // Retail reaches waypoint interaction through the movement collision
    // resolver and also requires MoveOrder bit 3. A stationary overlap cannot
    // pick up or capture a flag. [orig: Entity_ProcessWaypointInteraction
    // @0x4AD820, caller in Entity_MovementCollisionResolver]
    world->registry.get(blue)->net_move_input |= Entity::kMoveOrderMoving;
    world->match.advance_tick(*world);
    CHECK(world->registry.get(blue)->mounted_child == EntityHandle{});
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagPickups] == 0);
    contacts.touch(*world, blue, red_flag);
    world->match.advance_tick(*world);
    CHECK(world->registry.get(blue)->mounted_child == red_flag);
    CHECK(world->registry.get(red_flag)->primary_occupant == blue);
    CHECK((world->registry.get(red_flag)->flags & kEntityFlagCarried) != 0);
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagPickups] == 1);

    contacts.touch(*world, blue, blue_bay);
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

    // FlagReturnTime uses the flag's pool-1 class clock. A moved flag first
    // rearms its return counter; subsequent idle callbacks consume it.
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
    ItemDeathTraits flag_traits;
    flag_traits.death_class = ItemDeathClass::kFlag;
    world->tables.item_death_traits.set(4095, flag_traits);
    contacts.bind(timed_flag);
    contacts.touch(*world, blue, timed_flag);
    world->match.advance_tick(*world); // immediate service + per-tick pickup
    CHECK(blue_entity->mounted_child == timed_flag);
    destruction_notify_item_damage(*world, *world->registry.get(timed_flag), 0);
    world->registry.get(timed_flag)->class_think_ticks = 0;
    blue_entity->position = {0.0f, 0.0f, 0.0f};
    world->match.record_death(*world, blue);
    blue_entity->alive = false;
    blue_entity->flags |= kEntityFlagDead;
    CHECK(world->registry.get(timed_flag)->position.x == 0.0f);
    for (int i = 0; i < 5 * 62; ++i) {
        world->logic_tick = i;
        tick_item_event_pool(*world, 1);
        world->match.advance_tick(*world);
    }
    CHECK(world->registry.get(timed_flag)->position.x == 0.0f);
    world->logic_tick = 5 * 62;
    tick_item_event_pool(*world, 1);
    CHECK(world->registry.get(timed_flag)->position.x == 10.0f);
    world->registry.despawn(timed_flag);

    MatchRules flag_ball;
    flag_ball.game_type = gt::kFlagBall;
    flag_ball.game_time_minutes = 1;
    flag_ball.max_score = 2;
    world->match.configure(flag_ball);
    world->match.upsert_player({blue, 0, "Blue"});
    // The capture's team award is keyed by the captured flag's type: a null
    // flag awards the capturer alone, and FlagBall's ball (the neutral flag)
    // scores the capturer's own team row.
    // [orig: GameEvent_ProcessScoring case 9 @0x52F7CF..0x52F810]
    world->match.record_flag_capture(*world, blue, EntityHandle{});
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagCaptures] == 1);
    CHECK(world->match.team_stats(1)[MatchStats::kFlagCaptures] == 0);
    const EntityHandle ball = objective(*world, 4095, 0);
    world->match.record_flag_capture(*world, blue, ball);
    world->match.record_flag_capture(*world, blue, ball);
    const auto flag_ball_winner = world->match.winner_if_finished(*world);
    CHECK(flag_ball_winner.has_value() && *flag_ball_winner == 1);
    world->registry.despawn(ball);

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
    WaypointContactHarness contacts(*world);

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
    const EntityHandle bay = objective(*world, 4098, 1, {20.0f, 0.0f, 0.0f});
    contacts.bind(flag);
    contacts.bind(bay);

    contacts.touch(*world, carrier, flag);
    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == flag);
    contacts.touch(*world, carrier, bay);
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
    WaypointContactHarness contacts(*world);

    MatchRules rules;
    rules.game_type = gt::kFlagBall;
    world->match.configure(rules);
    const EntityHandle carrier = player(*world, 0, 1, "Carrier");
    Entity *carrier_entity = world->registry.get(carrier);
    carrier_entity->net_move_input = Entity::kMoveOrderMoving;

    const EntityHandle inert = objective(*world, 4095, 0, {}, 0);
    contacts.bind(inert);
    contacts.touch(*world, carrier, inert);
    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == EntityHandle{});

    world->registry.get(inert)->item_attrib =
        kItemAttribMoveCallback | kItemAttribPowerup;
    contacts.touch(*world, carrier, inert);
    world->match.advance_tick(*world);
    CHECK(carrier_entity->mounted_child == EntityHandle{});

    world->registry.get(inert)->item_attrib = kItemAttribMoveCallback;
    contacts.touch(*world, carrier, inert);
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
    world->match.record_zone_capture(*world, {blue, blue2});
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

void test_flagball_four_team_bays_consume_exact_contacts() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->registry.configure_pool(1, 8);
    WaypointContactHarness contacts(*world);

    MatchRules flagball;
    flagball.game_type = gt::kFlagBall;
    flagball.max_score = 99;
    world->match.configure(flagball);

    struct Side {
        uint8_t team;
        int32_t bay_item_id;
        float x;
    };
    for (const Side side : {Side{3, 4103, 0.0f}, Side{4, 4102, 30.0f}}) {
        const EntityHandle carrier = player(
            *world, side.team, side.team, side.team == 3 ? "TeamThree" : "TeamFour");
        Entity *carrier_entity = world->registry.get(carrier);
        carrier_entity->net_move_input = Entity::kMoveOrderMoving;
        const EntityHandle flag = objective(
            *world, 4095, 0, {side.x, 0.0f, 0.0f});
        const EntityHandle bay = objective(
            *world, side.bay_item_id, side.team, {side.x + 10.0f, 0.0f, 0.0f});
        contacts.bind(flag);
        contacts.bind(bay);

        contacts.touch(*world, carrier, flag);
        world->match.advance_tick(*world);
        CHECK(carrier_entity->mounted_child == flag);
        contacts.touch(*world, carrier, bay);
        world->match.advance_tick(*world);
        CHECK(carrier_entity->mounted_child == EntityHandle{});
        CHECK(world->registry.get(flag) != nullptr);
        CHECK(world->registry.get(flag)->position.x == side.x);
        CHECK(world->match.player(carrier)->stats[MatchStats::kFlagPickups] == 1);
        CHECK(world->match.player(carrier)->stats[MatchStats::kFlagCaptures] == 1);
        CHECK(world->match.player(carrier)->stats[MatchStats::kPoints] == 42);
        CHECK(world->match.team_stats(side.team)[MatchStats::kFlagCaptures] == 1);
    }

    const std::vector<MatchGameplayEvent> events =
        world->match.drain_gameplay_events();
    CHECK(events.size() == 4);
    if (events.size() == 4) {
        CHECK(events[0].kind == MatchGameplayEventKind::FlagPickup);
        CHECK(events[1].kind == MatchGameplayEventKind::FlagCapture);
        CHECK(events[2].kind == MatchGameplayEventKind::FlagPickup);
        CHECK(events[3].kind == MatchGameplayEventKind::FlagCapture);
        CHECK(!events[1].remove_objective && !events[3].remove_objective);
    }
}

void test_cac_combines_flag_and_zone_objectives() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->registry.configure_pool(1, 8);
    WaypointContactHarness contacts(*world);

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
    const EntityHandle bay = objective(*world, 4098, 1, {20.0f, 0.0f, 0.0f});
    contacts.bind(flag);
    contacts.bind(bay);

    contacts.touch(*world, blue, flag);
    world->match.advance_tick(*world);
    CHECK(blue_entity->mounted_child == flag);
    contacts.touch(*world, blue, bay);
    world->match.advance_tick(*world);
    CHECK(blue_entity->mounted_child == EntityHandle{});
    CHECK(world->registry.get(flag) != nullptr);
    CHECK(world->registry.get(flag)->position.x == 0.0f);
    CHECK(world->match.player(blue)->stats[MatchStats::kFlagCaptures] == 1);
    CHECK(world->match.player(blue)->stats[MatchStats::kPoints] == 0);

    // C&C's primary score and win arm remain the zone counter even though its
    // retail FIELD row also exposes flag stats. [orig: ScoreRules_GetPrimaryScoreField @0x52C850;
    // GameType_CreateDefaultSettings @0x52DD00]
    world->match.record_zone_capture(*world, {blue});
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
    world->match.remove_player(*world, blue);
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

// The trailing matrix row count: 0 for a non-team type, 3 for a team type,
// 5 for four-team TDM and for Team KOTH / FlagBall at ANY configured team
// count. [orig: Server_BuildEndOfRoundScoreboard @0x509259..0x50929C]
void test_end_round_team_row_count_rule() {
    auto expect_rows = [](uint32_t game_type, uint8_t team_count, uint8_t expected) {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 4);
        MatchRules configured = rules(game_type, 10, 0);
        configured.team_count = team_count;
        world->match.configure(configured);
        world->process_round_end(1);
        const uint8_t rows = world->match.result().team_row_count;
        if (rows != expected) {
            std::printf("FAIL %s:%d  team_row_count(game_type=0x%x, teams=%u) == %u, expected %u\n",
                        __FILE__, __LINE__, unsigned(game_type), unsigned(team_count),
                        unsigned(rows), unsigned(expected));
            ++failures;
        }
    };
    expect_rows(gt::kTeamKingOfTheHill, 2, 5);
    expect_rows(gt::kFlagBall, 2, 5);
    expect_rows(gt::kTeamDeathmatch, 4, 5);
    expect_rows(gt::kTeamDeathmatch, 2, 3);
    expect_rows(gt::kCaptureTheFlag, 4, 3);
    expect_rows(gt::kAdvanceAndSecure, 4, 3);
    expect_rows(gt::kDeathmatch, 2, 0);
    expect_rows(gt::kKingOfTheHill, 4, 0);
}

EntityHandle spawn_hill(World &world) {
    Entity entity;
    entity.kind = EntityKind::Item;
    entity.item_id = 6006;
    entity.has_item_def = true;
    entity.position = {0.0f, 0.0f, 0.0f};
    entity.bound_radius = 10.0f;
    entity.alive = true;
    return world.registry.spawn(3, entity);
}

// The team primary score and the frozen per-team hold word both read the
// TeamRecord+0x150 hill hold timer (one tick per service pass with a holder),
// never the per-second fold of the players' own slot ticks (+0x148).
// [orig: Server_BuildEndOfRoundScoreboard @0x508FA7/@0x5092E7;
// Server_BuildAndBroadcastScoreboard @0x50DCB8; Game_AccumulateTeamScores
// @0x508DAD (the timer) / @0x508E0A (the fold)]
void test_end_result_freezes_team_hold_timer() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 8);
    world->registry.configure_pool(3, 4);
    MatchRules tkoth;
    tkoth.game_type = gt::kTeamKingOfTheHill;
    tkoth.game_time_minutes = 10;
    tkoth.hill_limit_minutes = 99;
    world->match.configure(tkoth);
    const EntityHandle blue_a = player(*world, 0, 1, "BlueA");
    const EntityHandle blue_b = player(*world, 1, 1, "BlueB");
    const EntityHandle red = player(*world, 2, 2, "Red");
    world->registry.get(blue_a)->position = {0.0f, 0.0f, 0.0f};
    world->registry.get(blue_b)->position = {0.0f, 0.0f, 0.0f};
    world->registry.get(red)->position = {100.0f, 0.0f, 0.0f};
    spawn_hill(*world);
    advance_initial_periodic_passes(*world, 3);
    CHECK(world->match.player(blue_a)->objective_ticks == 3);
    CHECK(world->match.player(blue_b)->objective_ticks == 3);
    // Two holders accumulate one team tick per pass, not two.
    CHECK(world->match.team_primary_score(1) == 3);
    CHECK(world->match.team_primary_score(2) == 0);
    CHECK(world->match.live_scoreboard(*world).teams[1].primary_score == 3);

    world->process_round_end(1);
    const MatchResult &result = world->match.result();
    CHECK(result.team_row_count == 5);
    CHECK(result.team_hold_ticks[1] == 3 && result.team_hold_ticks[2] == 0);
    CHECK(result.team_scores[0] == 3 && result.team_scores[1] == 0);
    CHECK(!result.draw);
}

// Non-team boards: the frozen order follows the game-type primary
// (Player_ComputeScore), the draw byte is the all-tied test, and the winner
// marker goes to every non-spectator row whose key equals row 0's unless the
// board is a draw or its first two rows tie.
// [orig: Player_ComputeScore @0x500AD0..0x500ADF; Server_BuildEndOfRoundScoreboard
// @0x509053/@0x50920E/@0x50926A/@0x5092B2; Server_ProcessRoundEnd
// @0x5165A3..0x5165C3 and @0x5167E2..0x5167FD; GameEvent_ProcessScoring @0x52F6FA]
void test_nonteam_board_order_draw_and_winner_marker() {
    auto marker = [](const MatchResult &result, const char *name) {
        for (const MatchResultPlayer &row : result.players) {
            if (row.identity.name == name)
                return row.stats[MatchStats::kRoundMarker];
        }
        return int32_t{-1};
    };

    // (a) Points and kills disagree: Ace has 2 kills and 3 suicides (5 points),
    // Bee 1 kill (8 points). DM orders by kills. Every non-team Player sits on
    // team 1, and the kills stay enemy kills: the scorer's team rows exist
    // only in team modes. [orig: Server_AssignPlayerTeam @0x4FE3EC;
    // GameEvent_ProcessScoring @0x52F657]
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 8);
        world->match.configure(rules(gt::kDeathmatch, 10, 0));
        const EntityHandle ace = player(*world, 3, 1, "Ace");
        const EntityHandle bee = player(*world, 7, 1, "Bee");
        const EntityHandle cid = player(*world, 9, 1, "Cid");
        world->match.record_death(*world, bee, ace);
        world->match.record_death(*world, cid, ace);
        for (int i = 0; i < 3; ++i)
            world->match.record_death(*world, ace, ace);
        world->match.record_death(*world, cid, bee);
        CHECK(world->match.player(ace)->stats[MatchStats::kPoints] == 5);
        CHECK(world->match.player(bee)->stats[MatchStats::kPoints] == 8);
        world->process_round_end(0);
        const MatchResult &result = world->match.result();
        CHECK(result.players.size() == 3);
        CHECK(result.players[0].identity.name == "Ace" && result.players[0].primary_score == 2);
        CHECK(result.players[1].identity.name == "Bee" && result.players[1].primary_score == 1);
        CHECK(result.players[2].identity.name == "Cid" && result.players[2].primary_score == 0);
        CHECK(!result.draw);
        CHECK(marker(result, "Ace") == 2);
        CHECK(world->match.player(ace)->stats[MatchStats::kRoundMarker] == 2);
        CHECK(marker(result, "Bee") == 0 && marker(result, "Cid") == 0);
    }

    // (b) Every row tied at zero: a draw, no marker.
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 8);
        world->match.configure(rules(gt::kDeathmatch, 10, 0));
        player(*world, 3, 1, "Ace");
        player(*world, 7, 1, "Bee");
        world->process_round_end(0);
        const MatchResult &result = world->match.result();
        CHECK(result.draw);
        CHECK(marker(result, "Ace") == 0 && marker(result, "Bee") == 0);
    }

    // (c) A lone row: a draw at zero, a win with any positive score.
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 8);
        world->match.configure(rules(gt::kDeathmatch, 10, 0));
        player(*world, 3, 1, "Solo");
        world->process_round_end(0);
        CHECK(world->match.result().draw);
        CHECK(marker(world->match.result(), "Solo") == 0);

        world->match.configure(rules(gt::kDeathmatch, 10, 0));
        const EntityHandle solo = player(*world, 3, 1, "Solo");
        world->match.player(solo)->stats[MatchStats::kEnemyKills] = 1;
        world->process_round_end(0);
        CHECK(!world->match.result().draw);
        CHECK(marker(world->match.result(), "Solo") == 2);
    }

    // (d) Two rows tied at the top over a third: not a draw, but no marker.
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 8);
        world->match.configure(rules(gt::kDeathmatch, 10, 0));
        const EntityHandle ace = player(*world, 3, 1, "Ace");
        const EntityHandle bee = player(*world, 7, 1, "Bee");
        const EntityHandle cid = player(*world, 9, 1, "Cid");
        world->match.record_death(*world, cid, ace);
        world->match.record_death(*world, cid, bee);
        world->process_round_end(0);
        const MatchResult &result = world->match.result();
        CHECK(!result.draw);
        CHECK(result.players[0].identity.name == "Ace" && result.players[1].identity.name == "Bee");
        CHECK(marker(result, "Ace") == 0 && marker(result, "Bee") == 0 && marker(result, "Cid") == 0);
    }

    // (e) KOTH orders by the hill ticks the primary selects, not points.
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 8);
        MatchRules koth;
        koth.game_type = gt::kKingOfTheHill;
        koth.game_time_minutes = 10;
        koth.hill_limit_minutes = 99;
        world->match.configure(koth);
        const EntityHandle solo = player(*world, 3, 1, "Solo");
        const EntityHandle red = player(*world, 7, 1, "Red");
        world->match.player(solo)->objective_ticks = 5;
        world->match.player(solo)->stats[MatchStats::kPoints] = 50;
        world->match.player(red)->objective_ticks = 9;
        world->process_round_end(0);
        const MatchResult &result = world->match.result();
        CHECK(result.players[0].identity.name == "Red" && result.players[0].primary_score == 9);
        CHECK(!result.draw);
        CHECK(marker(result, "Red") == 2 && marker(result, "Solo") == 0);
    }

    // (f) A spectator-flagged top scorer receives no award.
    {
        auto world = std::make_unique<World>();
        world->registry.configure_pool(0, 8);
        world->match.configure(rules(gt::kDeathmatch, 10, 0));
        const EntityHandle ace = player(*world, 3, 1, "Ace");
        const EntityHandle bee = player(*world, 7, 1, "Bee");
        world->match.record_death(*world, bee, ace);
        world->match.set_player_spectator(ace, true);
        world->process_round_end(0);
        const MatchResult &result = world->match.result();
        CHECK(!result.draw);
        CHECK(result.players[0].identity.name == "Ace");
        CHECK(marker(result, "Ace") == 0 && marker(result, "Bee") == 0);
        CHECK(world->match.player(ace)->stats[MatchStats::kRoundMarker] == 0);
    }
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
    test_retail_objective_proximity_state_and_kill_bonuses();
    test_retail_objective_proximity_scoring_events();
    test_retail_zone_and_tkoth_win_quirks();
    test_demolition_flag_and_flagball_gameplay();
    test_target_destroyed_shares_through_both_occupant_links();
    test_flag_me_keeps_retails_unreachable_score_arm();
    test_flag_contact_requires_the_retail_move_callback_gate();
    test_aas_capture_scoring_and_outcomes();
    test_flagball_four_team_bays_consume_exact_contacts();
    test_cac_combines_flag_and_zone_objectives();
    test_end_result_is_frozen_in_retail_board_order();
    test_end_round_team_row_count_rule();
    test_end_result_freezes_team_hold_timer();
    test_nonteam_board_order_draw_and_winner_marker();
    test_coop_remains_script_owned();
    if (failures != 0) {
        std::printf("match_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("match_test: all checks passed");
    return 0;
}
