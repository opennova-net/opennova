// The kill scorer's retail structure: GameEvent_ProcessScoring case 3 as the
// Player death path (GameEvent_PlayerDeath) and the non-Player person death
// edge (Entity_CheckAndProcessDeath) reach it, the team-mode gate on every team
// leg, and the flag capture's hard-routed team rows.
// [orig: GameEvent_ProcessScoring @0x52F550; GameEvent_PlayerDeath @0x516DD0;
// Entity_CheckAndProcessDeath @0x51B550; CPlayerStats_RecordEvent @0x52C8E0]
#include <runtime/world/match.h>
#include <base/gameprofile/game_type.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

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

// Distinct score.ini VAR values so every award is attributable.
// [orig: scoringTable[74 + slot] reads, GameEvent_ProcessScoring @0x52F550]
MatchRules rules(uint32_t game_type) {
    MatchRules out;
    out.game_type = game_type;
    out.score_values.emplace();
    (*out.score_values)[2] = -5;  // FRIENDLYKILL
    (*out.score_values)[3] = 10;  // ENEMYKILL
    (*out.score_values)[4] = -3;  // SUICIDE
    (*out.score_values)[5] = -2;  // DEATH
    (*out.score_values)[16] = 7;  // MULTIPLEKILL
    (*out.score_values)[17] = 11; // HEADSHOTKILL
    (*out.score_values)[18] = 13; // KNIFEKILL
    return out;
}

EntityHandle player(World &world, uint8_t slot, uint8_t team) {
    Entity entity;
    entity.kind = EntityKind::Organic;
    entity.item_id = 5305;
    entity.has_item_def = true;
    entity.item_type = 3;
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
    identity.name = "P";
    world.match.upsert_player(identity);
    return handle;
}

EntityHandle npc(World &world, uint8_t team, uint8_t item_type = 3) {
    Entity entity;
    entity.kind = item_type == 3 ? EntityKind::Organic : EntityKind::Item;
    entity.item_id = 1798;
    entity.has_item_def = true;
    entity.item_type = item_type;
    entity.item_score = 10;
    entity.team = team;
    entity.health = 100;
    entity.alive = true;
    return world.registry.spawn(item_type == 3 ? 0 : 1, entity);
}

std::unique_ptr<World> make_world(uint32_t game_type) {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->registry.configure_pool(1, 16);
    world->match.configure(rules(game_type));
    return world;
}

// Deathmatch Players all sit on team 1, yet a kill between them is an enemy
// kill: the team rows (and with them the team-kill test) exist only when
// g_GameType carries the team bit, and no team row changes.
// [orig: Server_AssignPlayerTeam @0x4FE3EC (team 1 for every non-team slot);
// GameEvent_ProcessScoring @0x52F657 / @0x52F6A3 team-row gates, the team-kill
// test @0x5301E4..0x5301EE]
void test_non_team_modes_never_touch_team_rows() {
    for (const uint32_t game_type : {gt::kDeathmatch, gt::kKingOfTheHill}) {
        auto world = make_world(game_type);
        const EntityHandle ace = player(*world, 0, 1);
        const EntityHandle bee = player(*world, 1, 1);
        world->match.record_death(*world, bee, ace);
        const MatchPlayer *killer = world->match.player(ace);
        const MatchPlayer *victim = world->match.player(bee);
        CHECK(killer->stats[MatchStats::kEnemyKills] == 1);
        CHECK(killer->stats[MatchStats::kTeamKills] == 0);
        CHECK(killer->stats[MatchStats::kPoints] == 10);
        CHECK(victim->stats[MatchStats::kDeaths] == 1);
        CHECK(victim->stats[MatchStats::kPoints] == -2);
        CHECK(world->match.team_stats(1)[MatchStats::kEnemyKills] == 0);
        CHECK(world->match.team_stats(1)[MatchStats::kDeaths] == 0);
        CHECK(world->match.team_stats(1)[MatchStats::kPoints] == 0);
    }
}

// The scorer's team-kill test is the two actors resolving the same team row,
// with no see-all exemption (that test belongs to the kill feed's event type).
// [orig: GameEvent_ProcessScoring @0x5301E4..0x5301EE; feed-only see-all
// GameEvent_PlayerDeath @0x5170A6..0x517113]
void test_team_kill_has_no_see_all_exemption() {
    auto world = make_world(gt::kTeamDeathmatch);
    const EntityHandle ace = player(*world, 0, 1);
    const EntityHandle bee = player(*world, 1, 1);
    AiEntity *body = world->ai.at(world->ai.attach(ace));
    body->see_all = true;
    world->match.record_death(*world, bee, ace);
    const MatchPlayer *killer = world->match.player(ace);
    CHECK(killer->stats[MatchStats::kTeamKills] == 1);
    CHECK(killer->stats[MatchStats::kEnemyKills] == 0);
    CHECK(killer->stats[MatchStats::kPoints] == -5);
    CHECK(world->match.team_stats(1)[MatchStats::kTeamKills] == 1);
}

// The capture's team leg names its row by the FLAG TYPE in every game type,
// with no team-bit test: blue flag -> row 2, red flag -> row 1. Only the
// neutral flag uses the capturer's own team-mode row.
// [orig: GameEvent_ProcessScoring case 9 @0x52F7CF..0x52F810]
void test_flag_capture_team_rows_follow_the_flag_type() {
    {
        auto world = make_world(gt::kFlagBall);
        const EntityHandle carrier = player(*world, 0, 3);
        Entity flag;
        flag.kind = EntityKind::Item;
        flag.item_id = 4091;
        flag.has_item_def = true;
        const EntityHandle blue_flag = world->registry.spawn(1, flag);
        world->match.record_flag_capture(*world, carrier, blue_flag);
        CHECK(world->match.player(carrier)->stats[MatchStats::kFlagCaptures] == 1);
        CHECK(world->match.team_stats(2)[MatchStats::kFlagCaptures] == 1);
        CHECK(world->match.team_stats(3)[MatchStats::kFlagCaptures] == 0);
    }
    {
        auto world = make_world(gt::kDeathmatch);
        const EntityHandle carrier = player(*world, 0, 1);
        Entity flag;
        flag.kind = EntityKind::Item;
        flag.item_id = 4093;
        flag.has_item_def = true;
        const EntityHandle red_flag = world->registry.spawn(1, flag);
        flag.item_id = 4095;
        const EntityHandle neutral_flag = world->registry.spawn(1, flag);
        world->match.record_flag_capture(*world, carrier, red_flag);
        world->match.record_flag_capture(*world, carrier, neutral_flag);
        CHECK(world->match.team_stats(1)[MatchStats::kFlagCaptures] == 1);
    }
}

// A non-Player person's death edge runs one killer-victim scorer call: a
// Player killing a person of another nonzero team scores ENEMYKILL plus the
// entity+0x2C cause bonuses; a same-team or team-0 person scores nothing.
// [orig: Entity_CheckAndProcessDeath @0x51B5B3; GameEvent_ProcessScoring
// @0x52FFC3..0x530178]
void test_npc_person_victims_score_their_killer() {
    auto world = make_world(gt::kCoop);
    const EntityHandle ace = player(*world, 0, 1);
    const EntityHandle enemy = npc(*world, 2);
    const EntityHandle friendly = npc(*world, 1);
    const EntityHandle civilian = npc(*world, 0);
    const EntityHandle truck = npc(*world, 2, 1);

    world->match.record_death(*world, enemy, ace, 0x800u);
    const MatchPlayer *killer = world->match.player(ace);
    CHECK(killer->stats[MatchStats::kEnemyKills] == 1);
    CHECK(killer->stats[MatchStats::kHeadshotKills] == 1);
    CHECK(killer->stats[MatchStats::kPoints] == 10 + 11);
    CHECK(world->match.team_stats(1)[MatchStats::kEnemyKills] == 1);
    CHECK(world->match.team_stats(1)[MatchStats::kHeadshotKills] == 1);
    CHECK(killer->stats[MatchStats::kDeaths] == 0);

    world->match.record_death(*world, friendly, ace);
    world->match.record_death(*world, civilian, ace);
    world->match.record_death(*world, truck, ace);
    CHECK(killer->stats[MatchStats::kEnemyKills] == 1);
    CHECK(killer->stats[MatchStats::kTeamKills] == 0);
    CHECK(killer->stats[MatchStats::kPoints] == 21);

    world->match.record_death(*world, npc(*world, 3), ace, 0x100u | 0x400u);
    CHECK(killer->stats[MatchStats::kEnemyKills] == 2);
    CHECK(killer->stats[MatchStats::kMultipleKills] == 1);
    CHECK(killer->stats[MatchStats::kKnifeKills] == 1);
    CHECK(killer->stats[MatchStats::kPoints] == 21 + 10 + 7 + 13);
}

// A non-Player killer credits the Player on its +0x170 link (a vehicle's
// first occupant) half the ENEMYKILL value by a LOGICAL shift, counted as one
// shared-points award; a killer with no Player link scores nobody.
// [orig: GameEvent_ProcessScoring @0x52FF47..0x52FF79 (`shr edx, 1`) ->
// RecordEvent 28 / 27 @0x52F8AA..0x52F8C0]
void test_non_player_killer_credits_its_link() {
    auto world = make_world(gt::kTeamDeathmatch);
    const EntityHandle driver = player(*world, 0, 1);
    const EntityHandle victim = player(*world, 1, 2);
    const EntityHandle tank = npc(*world, 1, 1);
    world->registry.get(tank)->primary_occupant = driver;
    world->match.record_death(*world, victim, tank);
    const MatchPlayer *linked = world->match.player(driver);
    CHECK(linked->stats[MatchStats::kPoints] == 5);
    CHECK(linked->stats[MatchStats::kSharedPointAwards] == 1);
    CHECK(linked->stats[MatchStats::kEnemyKills] == 0);
    CHECK(world->match.player(victim)->stats[MatchStats::kDeaths] == 1);

    // A negative ENEMYKILL value shifts as unsigned.
    MatchRules negative = rules(gt::kTeamDeathmatch);
    (*negative.score_values)[3] = -4;
    auto second = std::make_unique<World>();
    second->registry.configure_pool(0, 16);
    second->registry.configure_pool(1, 16);
    second->match.configure(negative);
    const EntityHandle driver2 = player(*second, 0, 1);
    const EntityHandle victim2 = player(*second, 1, 2);
    const EntityHandle tank2 = npc(*second, 1, 1);
    second->registry.get(tank2)->primary_occupant = driver2;
    second->match.record_death(*second, victim2, tank2);
    CHECK(second->match.player(driver2)->stats[MatchStats::kPoints] ==
          static_cast<int32_t>(static_cast<uint32_t>(-4) >> 1));

    const EntityHandle loose = npc(*world, 2, 1);
    world->match.record_death(*world, npc(*world, 1), loose);
    CHECK(linked->stats[MatchStats::kPoints] == 5);
}

} // namespace

int main() {
    test_non_team_modes_never_touch_team_rows();
    test_team_kill_has_no_see_all_exemption();
    test_flag_capture_team_rows_follow_the_flag_type();
    test_npc_person_victims_score_their_killer();
    test_non_player_killer_credits_its_link();
    if (failures != 0) {
        std::printf("match_scoring_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("match_scoring_test: all checks passed");
    return 0;
}
