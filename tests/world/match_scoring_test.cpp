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
    body->slot.f[AiSlot::kBehaviorFlags] |= 0x200; // BERSERK
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

// Scorer event 12 adds the victim's signed `score` word to field 30 with no
// points: on the killer's slot, and on the killer's team row whenever the
// team bit resolves one (an NPC killer's row too). A spectator is refused.
// [orig: Score_ProcessKillEvent @0x4FD400 (the event-12 call @0x4FD438);
// GameEvent_ProcessScoring case 12 @0x52FEC2..0x52FF0F]
void test_kill_event_adds_the_victim_score() {
    auto world = make_world(gt::kTeamDeathmatch);
    const EntityHandle ace = player(*world, 0, 1);
    const EntityHandle rifleman = npc(*world, 2);
    world->registry.get(rifleman)->item_score = 25;
    world->match.record_kill_event(*world, ace, rifleman);
    const MatchPlayer *killer = world->match.player(ace);
    CHECK(killer->stats[MatchStats::kUnitScore] == 25);
    CHECK(killer->stats[MatchStats::kPoints] == 0);
    CHECK(world->match.team_stats(1)[MatchStats::kUnitScore] == 25);

    const EntityHandle gunner = npc(*world, 2);
    world->match.record_kill_event(*world, gunner, npc(*world, 1));
    CHECK(world->match.team_stats(2)[MatchStats::kUnitScore] == 10);

    world->match.set_player_spectator(ace, true);
    world->match.record_kill_event(*world, ace, rifleman);
    CHECK(killer->stats[MatchStats::kUnitScore] == 25);

    auto dm = make_world(gt::kDeathmatch);
    const EntityHandle solo = player(*dm, 0, 1);
    dm->match.record_kill_event(*dm, solo, npc(*dm, 2));
    CHECK(dm->match.player(solo)->stats[MatchStats::kUnitScore] == 10);
    CHECK(dm->match.team_stats(1)[MatchStats::kUnitScore] == 0);
}

// Scorer event 1: one shot plus the FIRE value per accepted round, with the
// team-mode mirror; a spectator's round scores nothing.
// [orig: Server_ClientFiredRound @0x50BAA0 (the event-1 call @0x50C727);
// GameEvent_ProcessScoring case 1 @0x52FB2A..0x52FC13]
void test_shot_scores_fire() {
    MatchRules fire = rules(gt::kTeamDeathmatch);
    (*fire.score_values)[0] = 1; // FIRE
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->match.configure(fire);
    const EntityHandle ace = player(*world, 0, 2);
    world->match.record_shot(*world, ace);
    world->match.record_shot(*world, ace);
    const MatchPlayer *shooter = world->match.player(ace);
    CHECK(shooter->stats[MatchStats::kShotsFired] == 2);
    CHECK(shooter->stats[MatchStats::kPoints] == 2);
    CHECK(world->match.team_stats(2)[MatchStats::kShotsFired] == 2);
    world->match.set_player_spectator(ace, true);
    world->match.record_shot(*world, ace);
    CHECK(shooter->stats[MatchStats::kShotsFired] == 2);
}

// A killed enemy Player carrying a flag adds FLAGCARRIERKILL; the victim's
// cause bits add their bonuses; a sniper-class (6) victim adds the
// ENEMYSNIPERKILL points with no counter. Each mirrors onto the team row.
// [orig: GameEvent_ProcessScoring @0x530246..0x5304AB]
void test_enemy_player_kill_bonuses() {
    MatchRules bonus = rules(gt::kTeamDeathmatch);
    (*bonus.score_values)[20] = 17; // FLAGCARRIERKILL
    (*bonus.score_values)[28] = 19; // ENEMYSNIPERKILL
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->registry.configure_pool(1, 16);
    world->match.configure(bonus);
    const EntityHandle ace = player(*world, 0, 1);
    const EntityHandle carrier = player(*world, 1, 2);
    Entity flag;
    flag.kind = EntityKind::Item;
    flag.item_id = 4093;
    flag.has_item_def = true;
    const EntityHandle red_flag = world->registry.spawn(1, flag);
    world->registry.get(carrier)->mounted_child = red_flag;
    world->registry.get(red_flag)->primary_occupant = carrier;
    world->registry.get(carrier)->player_class = 6;

    world->match.record_death(*world, carrier, ace, 0x800u);
    const MatchPlayer *killer = world->match.player(ace);
    CHECK(killer->stats[MatchStats::kEnemyKills] == 1);
    CHECK(killer->stats[MatchStats::kFlagCarrierKills] == 1);
    CHECK(killer->stats[MatchStats::kHeadshotKills] == 1);
    CHECK(killer->stats[MatchStats::kPoints] == 10 + 17 + 11 + 19);
    CHECK(world->match.team_stats(1)[MatchStats::kFlagCarrierKills] == 1);
    CHECK(world->match.team_stats(1)[MatchStats::kPoints] == 10 + 17 + 11 + 19);
    CHECK(!world->registry.get(carrier)->mounted_child.valid());

    // The same kill of an empty-handed rifleman adds none of them.
    world->registry.get(carrier)->player_class = 8;
    world->match.record_death(*world, carrier, ace);
    CHECK(killer->stats[MatchStats::kFlagCarrierKills] == 1);
    CHECK(killer->stats[MatchStats::kPoints] == 57 + 10);
}

// Scorer event 6: the medic's MEDICSAVE counter and value, team-mirrored.
// [orig: GameEvent_RevivePlayer @0x517CD0 (the event-6 call @0x517DC5);
// GameEvent_ProcessScoring case 6 @0x52FCD8..0x52FD2B]
void test_revive_scores_medicsave() {
    MatchRules medic = rules(gt::kTeamDeathmatch);
    (*medic.score_values)[7] = 4; // MEDICSAVE
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->match.configure(medic);
    const EntityHandle doc = player(*world, 0, 1);
    world->match.record_revive(*world, doc);
    CHECK(world->match.player(doc)->stats[MatchStats::kMedicSaves] == 1);
    CHECK(world->match.player(doc)->stats[MatchStats::kPoints] == 4);
    CHECK(world->match.team_stats(1)[MatchStats::kMedicSaves] == 1);
}

// Every points award is RecordEvent 28 with param2 = 0, so it shares half
// with the Player on the earner's +0x170 link; event 25 alone keeps its award.
// A Player's death clears every pool-0 AI body's link to it.
// [orig: CPlayerStats_RecordEvent case 28 @0x52CAF8..0x52CBB3; case 25's
// param2 @0x530989; GameEvent_PlayerDeath @0x516E07..0x516E45]
void test_points_share_with_the_link() {
    MatchRules share = rules(gt::kTeamDeathmatch);
    (*share.score_values)[35] = 6; // ALIVE
    (*share.score_values)[36] = 1; // ALIVEQUANTUM
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->registry.configure_pool(1, 16);
    world->match.configure(share);
    world->rules.mp_session = true; // event 25 needs a network session
    const EntityHandle rider = player(*world, 0, 1);
    const EntityHandle driver = player(*world, 1, 1);
    const EntityHandle enemy = player(*world, 2, 2);
    world->registry.get(rider)->primary_occupant = driver;

    world->match.record_death(*world, enemy, rider);
    const MatchPlayer *earner = world->match.player(rider);
    const MatchPlayer *linked = world->match.player(driver);
    CHECK(earner->stats[MatchStats::kPoints] == 10);
    CHECK(linked->stats[MatchStats::kPoints] == 5);
    CHECK(linked->stats[MatchStats::kSharedPointAwards] == 1);

    for (int tick = 0; tick < 3 * 62; ++tick)
        world->match.advance_tick(*world);
    CHECK(earner->stats[MatchStats::kPeriodicScoreUnits] > 0);
    CHECK(linked->stats[MatchStats::kSharedPointAwards] == 1);
    CHECK(linked->stats[MatchStats::kPoints] - 5 ==
          linked->stats[MatchStats::kPeriodicScoreUnits] * 6);

    // The driver's death unlinks the pool-0 AI rider; a pool-0 body without an
    // AI component and a pool-1 hull keep theirs.
    const EntityHandle statue = npc(*world, 1);
    world->registry.get(statue)->primary_occupant = driver;
    const EntityHandle hull = npc(*world, 1, 1);
    world->registry.get(hull)->primary_occupant = driver;
    world->ai.attach(rider);
    world->match.record_death(*world, driver, enemy);
    CHECK(!world->registry.get(rider)->primary_occupant.valid());
    CHECK(world->registry.get(statue)->primary_occupant == driver);
    CHECK(world->registry.get(hull)->primary_occupant == driver);
}

// The round clock keeps counting after the round ends (no round-over latch),
// in the gameplay phase only. [orig: Game_ProcessMainFrame @0x5265DA..0x526602]
void test_round_clock_runs_through_the_linger() {
    MatchRules timed = rules(gt::kTeamDeathmatch);
    timed.game_time_minutes = 1;
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->match.configure(timed);
    world->rules.mp_session = true;
    player(*world, 0, 1);
    world->match.advance_tick(*world);
    world->process_round_end(1);
    const int32_t at_end = world->match.remaining_ticks();
    for (int tick = 0; tick < 10; ++tick)
        world->match.advance_tick(*world);
    CHECK(world->match.remaining_ticks() == at_end - 10);
    world->match.advance_tick(*world, TickPhase::PreRound);
    CHECK(world->match.remaining_ticks() == at_end - 10);
}

// The pre-board winner pass is a team-game pass (the team bit), refuses a
// spectator slot, and leaves non-team games to the post-board award.
// [orig: Server_ProcessRoundEnd @0x516536..0x51658C (the team bit @0x516541);
// GameEvent_ProcessScoring @0x52F6E5]
void test_team_winner_pass_needs_the_team_bit() {
    {
        auto world = make_world(gt::kTeamDeathmatch);
        const EntityHandle blue = player(*world, 0, 1);
        const EntityHandle watcher = player(*world, 1, 1);
        world->match.set_player_spectator(watcher, true);
        world->process_round_end(1);
        CHECK(world->match.player(blue)->stats[MatchStats::kRoundMarker] == 2);
        CHECK(world->match.player(watcher)->stats[MatchStats::kRoundMarker] == 0);
    }
    {
        // Every DM Player is team 1; a team-1 result marks only the top score.
        auto world = make_world(gt::kDeathmatch);
        const EntityHandle ace = player(*world, 0, 1);
        const EntityHandle bee = player(*world, 1, 1);
        world->match.record_death(*world, bee, ace);
        world->process_round_end(1);
        CHECK(world->match.player(ace)->stats[MatchStats::kRoundMarker] == 2);
        CHECK(world->match.player(bee)->stats[MatchStats::kRoundMarker] == 0);
    }
}

// The proximity pass skips a spectator after clearing its mask, and the TKOTH
// holder census never counts one. [orig: Server_UpdateCaptureZoneProximity
// @0x508795; Game_CountAlivePlayersPerTeam @0x500214]
void test_spectators_hold_no_objective() {
    MatchRules koth = rules(gt::kTeamKingOfTheHill);
    koth.game_time_minutes = 5;
    koth.hill_limit_minutes = 99;
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->registry.configure_pool(3, 16);
    world->match.configure(koth);
    world->rules.mp_session = true;
    const EntityHandle watcher = player(*world, 0, 1);
    world->match.set_player_spectator(watcher, true);
    Entity hill;
    hill.kind = EntityKind::Item;
    hill.item_id = 6006;
    hill.has_item_def = true;
    hill.bound_radius = 10.0f;
    hill.alive = true;
    world->registry.spawn(3, hill);
    for (int tick = 0; tick < 3 * 62; ++tick)
        world->match.advance_tick(*world);
    CHECK(world->match.player(watcher)->objective_proximity_mask == 0);
    CHECK(world->match.player(watcher)->stats[MatchStats::kHillTime] == 0);
    CHECK(world->match.team_primary_score(1) == 0);
}

// Event 25's counter runs for a live, deployed Player in a network session
// only: an undeployed (respawn-pending) slot and the SP game skip it.
// [orig: Server_UpdateCaptureZoneProximity — the pending bit @0x5087A2, the
//  session test @0x5087BC, the counter @0x5087C9..0x5087F1]
void test_periodic_score_needs_a_deployed_session_player() {
    MatchRules periodic = rules(gt::kTeamDeathmatch);
    (*periodic.score_values)[35] = 7;
    (*periodic.score_values)[36] = 1;
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->match.configure(periodic);
    world->rules.mp_session = true;
    const EntityHandle pending = player(*world, 0, 1);
    const EntityHandle deployed = player(*world, 1, 1);
    world->match.set_player_respawn_pending(pending, true);
    world->match.advance_tick(*world);
    CHECK(world->match.player(pending)->periodic_score_ticks == 0);
    CHECK(world->match.player(pending)->stats[MatchStats::kPoints] == 0);
    CHECK(world->match.player(deployed)->periodic_score_ticks == 1);
    CHECK(world->match.player(deployed)->stats[MatchStats::kPoints] == 7);

    world->match.set_player_respawn_pending(pending, false);
    world->rules.mp_session = false;
    for (int tick = 0; tick < 62; ++tick)
        world->match.advance_tick(*world);
    CHECK(world->match.player(pending)->periodic_score_ticks == 0);
    CHECK(world->match.player(deployed)->periodic_score_ticks == 1);
}

// Past the round end the proximity pass returns at its head, but the team
// hold census after it keeps running over the masks the last pass built;
// it still never counts a spectator slot.
// [orig: Server_TickUpdate — the round-over test @0x51DE58 skips only to
//  @0x51DF50, the calls @0x51DF50/@0x51DF55; Server_UpdateCaptureZoneProximity
//  @0x5086A3; Game_CountAlivePlayersPerTeam @0x500214]
void test_team_hold_counts_after_the_round() {
    MatchRules koth = rules(gt::kTeamKingOfTheHill);
    koth.game_time_minutes = 5;
    koth.hill_limit_minutes = 99;
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->registry.configure_pool(3, 16);
    world->match.configure(koth);
    world->rules.mp_session = true;
    player(*world, 0, 1);
    const EntityHandle red = player(*world, 1, 2);
    Entity hill;
    hill.kind = EntityKind::Item;
    hill.item_id = 6006;
    hill.has_item_def = true;
    hill.bound_radius = 10.0f;
    hill.alive = true;
    world->registry.spawn(3, hill);
    world->match.advance_tick(*world);
    CHECK(world->match.team_primary_score(1) == 1);
    CHECK(world->match.team_primary_score(2) == 1);

    world->process_round_end(1);
    world->match.set_player_spectator(red, true);
    for (int tick = 0; tick < 62; ++tick)
        world->match.advance_tick(*world);
    CHECK(world->match.team_primary_score(1) == 2);
    CHECK(world->match.team_primary_score(2) == 0);
    CHECK(world->match.result().team_hold_ticks[1] == 1);
}

// A game type without a score table gets no winner award: the scorer's
// category gate refuses event 21 (FlagMe maps past the table).
// [orig: GameEvent_ProcessScoring @0x52F617..0x52F640; Server_ProcessRoundEnd
//  @0x5167F6..0x5167FD]
void test_winner_award_needs_a_score_table() {
    auto world = make_world(gt::kFlagMe);
    const EntityHandle ace = player(*world, 0, 1);
    player(*world, 1, 1);
    world->match.player(ace)->stats[MatchStats::kFlagCaptures] = 1;
    world->process_round_end(0);
    CHECK(!world->match.result().draw);
    CHECK(world->match.result().players[0].identity.entity == ace);
    CHECK(world->match.player(ace)->stats[MatchStats::kRoundMarker] == 0);
}

// Scorer event 5: MEDICHEAL (RecordEvent 7, value 6) on the medic and its
// team row; the scorer refuses it when the medic or the patient is a
// spectator. [orig: GameEvent_HealPlayer @0x50DE30 (the call @0x50DEA4);
// GameEvent_ProcessScoring — the spectator tests @0x52F6E5/@0x52F6FA, case 5
// @0x52FD3A..0x52FD8D]
void test_heal_scores_medicheal() {
    MatchRules heal = rules(gt::kTeamDeathmatch);
    (*heal.score_values)[6] = 4;
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->match.configure(heal);
    const EntityHandle medic = player(*world, 0, 1);
    const EntityHandle patient = player(*world, 1, 1);
    world->match.record_heal(*world, medic, patient);
    CHECK(world->match.player(medic)->stats[MatchStats::kMedicHeals] == 1);
    CHECK(world->match.player(medic)->stats[MatchStats::kPoints] == 4);
    CHECK(world->match.player(patient)->stats[MatchStats::kMedicHeals] == 0);
    CHECK(world->match.team_stats(1)[MatchStats::kMedicHeals] == 1);
    CHECK(world->match.team_stats(1)[MatchStats::kPoints] == 4);
    world->match.set_player_spectator(patient, true);
    world->match.record_heal(*world, medic, patient);
    CHECK(world->match.player(medic)->stats[MatchStats::kMedicHeals] == 1);
    world->match.set_player_spectator(patient, false);
    world->match.set_player_spectator(medic, true);
    world->match.record_heal(*world, medic, patient);
    CHECK(world->match.team_stats(1)[MatchStats::kMedicHeals] == 1);
}

} // namespace

int main() {
    test_non_team_modes_never_touch_team_rows();
    test_team_kill_has_no_see_all_exemption();
    test_flag_capture_team_rows_follow_the_flag_type();
    test_npc_person_victims_score_their_killer();
    test_non_player_killer_credits_its_link();
    test_kill_event_adds_the_victim_score();
    test_shot_scores_fire();
    test_enemy_player_kill_bonuses();
    test_revive_scores_medicsave();
    test_points_share_with_the_link();
    test_round_clock_runs_through_the_linger();
    test_team_winner_pass_needs_the_team_bit();
    test_spectators_hold_no_objective();
    test_periodic_score_needs_a_deployed_session_player();
    test_team_hold_counts_after_the_round();
    test_winner_award_needs_a_score_table();
    test_heal_scores_medicheal();
    if (failures != 0) {
        std::printf("match_scoring_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("match_scoring_test: all checks passed");
    return 0;
}
