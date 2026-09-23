// The capture transaction's retail structure beyond the zone-chain basics: the
// 1 Hz secure pass (latch before read, every enemy counted, the pool walk, the
// delta's low word), the numbered flip's GameEvent_FlagCapture inputs, the
// unnumbered PSPTAKEOVER scoring, the timed restart window, the C&C exemption,
// the A&S/C&C-only all-owned arm, the live FARP masks, and the sorted
// auto-deploy walk.
// [orig: Server_UpdateCaptureZoneEntities @0x519690;
// calculate_capture_zone_control_delta @0x501120; Server_UpdateCaptureZones
// @0x53B8F0; GameEvent_FlagCapture @0x50F6F0; CaptureZone_CheckProximityScoring
// @0x500C50; ZoneSlotChain_IsZoneCapturableByTeam @0x4A2450;
// ZoneSlotChain_GetWinningTeamIfAllOwned @0x4A2920; Server_EnforceZoneEntityTeams
// @0x519600; find_spawn_entity_for_team @0x4FC810]
#include <base/gameprofile/game_type.h>
#include <runtime/world/match.h>
#include <runtime/world/world.h>
#include <runtime/world/zone_capture.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <variant>
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

std::unique_ptr<World> make_world(uint32_t game_type, int32_t duration = 15) {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 16);
    world->registry.configure_pool(1, 16);
    world->registry.configure_pool(2, 16);
    MatchRules rules;
    rules.game_type = game_type;
    rules.capture_duration_seconds = duration;
    world->match.configure(rules);
    return world;
}

EntityHandle zone(World &world, uint8_t number, uint8_t team, Vec3 position,
                  int pool = 1) {
    Entity entity;
    entity.kind = pool == 2 ? EntityKind::Building : EntityKind::Item;
    entity.item_id = 1359;
    entity.has_item_def = true;
    entity.position = position;
    entity.team = team;
    entity.zone_number = number;
    entity.zone_radius = 20;
    entity.is_capture_trigger = true;
    entity.is_spawn_point = true;
    entity.alive = true;
    return world.registry.spawn(pool, entity);
}

EntityHandle player(World &world, uint8_t slot, uint8_t team, Vec3 position) {
    Entity entity;
    entity.kind = EntityKind::Organic;
    entity.item_id = 5305;
    entity.has_item_def = true;
    entity.item_type = 3;
    entity.player_class = 8;
    entity.team = team;
    entity.position = position;
    entity.health = 100;
    entity.alive = true;
    entity.flags = kEntityFlagPlayer;
    entity.engine_flags = kEntityFlagPlayer;
    const EntityHandle handle = world.registry.spawn(0, entity);
    world.match.upsert_player({handle, slot, "P"});
    return handle;
}

template <typename T>
std::vector<T> events_of(const ZoneCaptureEvents &events) {
    std::vector<T> out;
    for (const ZoneCaptureEvents::Event &event : events.ordered)
        if (const T *value = std::get_if<T>(&event)) out.push_back(*value);
    return out;
}

const ZoneCaptureEvents::Control *control_of(const std::vector<ZoneCaptureEvents::Control> &list,
                                              EntityHandle handle) {
    for (const ZoneCaptureEvents::Control &control : list)
        if (control.zone == handle) return &control;
    return nullptr;
}

// The latch reads only enemy_of(owner) (a neutral zone asks team 1), and the
// pass reads `before` after the latch, so latching alone raises no 0x3B edge.
// [orig: Server_UpdateCaptureZoneEntities @0x519745..0x519775]
void test_secure_pass_latches_before_reading_control() {
    auto world = make_world(gt::kAdvanceAndSecure);
    const EntityHandle rear = zone(*world, 1, 2, {0.0f, 0.0f, 0.0f});
    const EntityHandle middle = zone(*world, 2, 0, {500.0f, 0.0f, 0.0f});
    const EntityHandle front = zone(*world, 3, 2, {1000.0f, 0.0f, 0.0f});
    world->zones.build_chain_from_mission();
    world->registry.get(middle)->zone_control = 0;
    world->registry.get(front)->zone_control = 0;

    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    // Team 1 owns nothing, so it can reach no zone: every zone latches, the
    // neutral one included (its enemy_of is team 1).
    CHECK(world->registry.get(rear)->zone_control == 0x10000);
    CHECK(world->registry.get(middle)->zone_control == 0x10000);
    CHECK(world->registry.get(front)->zone_control == 0x10000);
    CHECK(events_of<ZoneCaptureEvents::Secure>(events).empty());
    // One 0x6F record per walked zone, in pool order.
    const auto controls = events_of<ZoneCaptureEvents::Control>(events);
    CHECK(controls.size() == 3);
    CHECK(controls.size() == 3 && controls[0].zone == rear && controls[1].zone == middle &&
          controls[2].zone == front);
}

// Every other-team Player in radius counts in the enemies byte; only one whose
// team may capture the zone counts against the owner. A zone number above 30
// never joins the chain but is still walked.
// [orig: calculate_capture_zone_control_delta @0x50121D..0x501248;
// ZoneSlotChain_AddZoneEntity @0x4A2D90; the walk @0x5196A0..0x51973F]
void test_secure_pass_counts_every_enemy() {
    auto world = make_world(gt::kAdvanceAndSecure);
    const EntityHandle base = zone(*world, 1, 1, {0.0f, 0.0f, 0.0f});
    const EntityHandle mid = zone(*world, 2, 0, {500.0f, 0.0f, 0.0f});
    const EntityHandle far = zone(*world, 3, 2, {1000.0f, 0.0f, 0.0f});
    const EntityHandle high = zone(*world, 31, 1, {2000.0f, 0.0f, 0.0f});
    world->zones.build_chain_from_mission();
    CHECK(world->zones.chain.zones.size() == 3);
    // Team 2 cannot reach the base (two hops): its soldier is counted but does
    // not erode control.
    player(*world, 0, 2, {1.0f, 0.0f, 0.0f});
    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    const auto controls = events_of<ZoneCaptureEvents::Control>(events);
    const ZoneCaptureEvents::Control *base_control = control_of(controls, base);
    CHECK(base_control != nullptr && base_control->enemies == 1 &&
          base_control->friendlies == 0 && base_control->delta == 0);
    CHECK(world->registry.get(base)->zone_control == 0x10000);
    CHECK(control_of(controls, high) != nullptr);
    (void)mid;
    (void)far;
}

// An empty sizing side on a full server divides by zero: the delta is the x87
// integer indefinite, control clamps at zero, and the 0x6F word is its low half.
// [orig: _ftol2_sse @0x50145C; the clamp @0x5014A5..0x5014AE; `movzx ebx, ax`
// @0x51978C]
void test_secure_pass_empty_side_divides_by_zero() {
    auto world = make_world(gt::kAdvanceAndSecure);
    const EntityHandle neutral = zone(*world, 1, 0, {0.0f, 0.0f, 0.0f});
    world->zones.build_chain_from_mission();
    // A team-0 Player stands in the neutral zone; six team-1 Players elsewhere
    // fill the census so no small-server boost applies. The rising side of a
    // neutral zone is team 2, which has nobody.
    player(*world, 0, 0, {1.0f, 0.0f, 0.0f});
    for (uint8_t slot = 1; slot <= 6; ++slot)
        player(*world, slot, 1, {3000.0f, float(slot) * 10.0f, 0.0f});
    world->registry.get(neutral)->zone_control = 0x8000;
    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    const auto controls = events_of<ZoneCaptureEvents::Control>(events);
    const ZoneCaptureEvents::Control *control = control_of(controls, neutral);
    CHECK(control != nullptr && control->control == 0 && control->delta == 0 &&
          control->friendlies == 1);
}

// A numbered SpawnPoint flip carries GameEvent_FlagCapture's inputs: the
// zone's number and rank, whether the enemy mask held, the capturer's frontier
// after the refresh, the decided test and the capturer's slot.
// [orig: GameEvent_FlagCapture @0x50F737..0x50F912;
// ZoneSlotChain_RebuildMasksAndCheckUnchanged @0x4A2B60]
void test_numbered_flip_feeds_the_capture_pair() {
    auto world = make_world(gt::kAdvanceAndSecure);
    zone(*world, 1, 1, {0.0f, 0.0f, 0.0f});
    const EntityHandle held_a = zone(*world, 2, 2, {500.0f, 0.0f, 0.0f});
    zone(*world, 2, 2, {600.0f, 0.0f, 0.0f});
    zone(*world, 3, 2, {1000.0f, 0.0f, 0.0f});
    world->zones.build_chain_from_mission();
    const EntityHandle capturer = player(*world, 0, 1, {3000.0f, 0.0f, 0.0f});
    world->zones.capture.requests.push_back({held_a, 1, capturer});
    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    const auto flips = events_of<ZoneCaptureEvents::Flip>(events);
    CHECK(flips.size() == 1);
    if (!flips.empty()) {
        const ZoneCaptureEvents::Flip &flip = flips[0];
        CHECK(flip.numbered && flip.announce && flip.new_team == 1 && flip.old_team == 2);
        // Team 2 still holds the other number-2 entity: its mask held.
        CHECK(flip.unchanged);
        CHECK(!flip.decided && flip.capturer_is_player);
        CHECK(flip.zone_number == 2);
        // Two entities share number 2: the first registered takes rank 1.
        CHECK(flip.rank == 1);
        // Team 1 now reaches the other number-2 entity.
        CHECK(flip.frontier == 2);
        CHECK(flip.scorers.empty()); // the capturer stands outside the radius
    }

    // Taking a number the enemy never held reports a change.
    auto second = make_world(gt::kAdvanceAndSecure);
    zone(*second, 1, 1, {0.0f, 0.0f, 0.0f});
    const EntityHandle open = zone(*second, 2, 0, {500.0f, 0.0f, 0.0f});
    zone(*second, 3, 2, {1000.0f, 0.0f, 0.0f});
    second->zones.build_chain_from_mission();
    const EntityHandle taker = player(*second, 0, 1, {500.0f, 0.0f, 0.0f});
    second->zones.capture.requests.push_back({open, 1, taker});
    second->zones.capture_second_tick(events);
    const auto open_flips = events_of<ZoneCaptureEvents::Flip>(events);
    CHECK(open_flips.size() == 1 && !open_flips[0].unchanged &&
          open_flips[0].scorers.size() == 1 && open_flips[0].scorers[0] == taker);
}

// The all-owned helper answers only in A&S and C&C: a flip that leaves one owner
// on every chain entry is "decided" there, while TDM keeps its pair and its
// ordinary win arms. [orig: ZoneSlotChain_GetWinningTeamIfAllOwned
// @0x4A2921..0x4A294E; Server_CheckWinConditions @0x51AD40 (the call @0x51AD8C)]
void test_all_owned_arm_is_as_and_cc_only() {
    for (const uint32_t game_type : {gt::kAdvanceAndSecure, gt::kTeamDeathmatch}) {
        auto world = make_world(game_type);
        zone(*world, 1, 1, {0.0f, 0.0f, 0.0f});
        const EntityHandle last = zone(*world, 2, 2, {500.0f, 0.0f, 0.0f});
        world->zones.build_chain_from_mission();
        const EntityHandle capturer = player(*world, 0, 1, {3000.0f, 0.0f, 0.0f});
        world->zones.capture.requests.push_back({last, 1, capturer});
        ZoneCaptureEvents events;
        world->zones.capture_second_tick(events);
        const auto flips = events_of<ZoneCaptureEvents::Flip>(events);
        const bool as = game_type == gt::kAdvanceAndSecure;
        CHECK(flips.size() == 1 && flips[0].decided == as);
        const std::optional<int32_t> winner = world->match.winner_if_finished(*world);
        CHECK(as ? (winner.has_value() && *winner == 1) : !winner.has_value());
    }
}

// C&C makes every chain zone capturable by every team.
// [orig: ZoneSlotChain_IsZoneCapturableByTeam @0x4A2476..0x4A2480]
void test_conquer_and_control_exempts_the_frontier() {
    for (const uint32_t game_type : {gt::kAdvanceAndSecure, gt::kConquerAndControl}) {
        auto world = make_world(game_type);
        zone(*world, 1, 1, {0.0f, 0.0f, 0.0f});
        zone(*world, 2, 0, {500.0f, 0.0f, 0.0f});
        const EntityHandle far = zone(*world, 3, 2, {1000.0f, 0.0f, 0.0f});
        world->zones.build_chain_from_mission();
        CHECK(world->zones.is_capturable(1, *world->registry.get(far)) ==
              (game_type == gt::kConquerAndControl));
    }
}

// An unnumbered zone scores PSPTAKEOVER (event 14) on its capturer, instantly
// under a zero duration and at a timed completion; LFPTAKEOVER stays with
// numbered zones. [orig: CaptureZone_CheckProximityScoring @0x500CAF (the
// unnumbered test) -> @0x500DC5; GameEvent_ProcessScoring case 14 @0x52FDFE]
void test_unnumbered_capture_scores_psp_takeover() {
    auto world = make_world(gt::kAdvanceAndSecure, 0);
    const EntityHandle objective = zone(*world, 0, 2, {0.0f, 0.0f, 0.0f});
    const EntityHandle capturer = player(*world, 0, 1, {0.0f, 0.0f, 0.0f});
    world->zones.capture.requests.push_back({objective, 1, capturer});
    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    const auto flips = events_of<ZoneCaptureEvents::Flip>(events);
    CHECK(flips.size() == 1 && !flips[0].numbered && flips[0].takeover &&
          flips[0].scorers.empty());
    world->match.record_psp_takeover(*world, capturer);
    const MatchPlayer *row = world->match.player(capturer);
    CHECK(row->stats[MatchStats::kPspTakeovers] == 1);
    CHECK(row->stats[MatchStats::kZoneTakeovers] == 0);
    CHECK(row->stats[MatchStats::kPoints] == 12); // A&S PSPTAKEOVER
    CHECK(world->match.team_stats(1)[MatchStats::kPspTakeovers] == 1);

    // A timed completion carries the same scoring.
    auto timed = make_world(gt::kAdvanceAndSecure, 1);
    const EntityHandle slow = zone(*timed, 0, 0, {0.0f, 0.0f, 0.0f});
    const EntityHandle runner = player(*timed, 0, 2, {0.0f, 0.0f, 0.0f});
    timed->zones.capture.active.push_back({slow, 2, 0, 1, runner, {}, 1});
    timed->zones.capture_second_tick(events);
    const auto completions = events_of<ZoneCaptureEvents::TimedCompletion>(events);
    CHECK(completions.size() == 1 && completions[0].takeover &&
          completions[0].capturer == runner);
}

// The restart window carries progress 0 AND limit 0, while the entry keeps the
// configured limit. [orig: Server_UpdateCaptureZones @0x53B9E3..0x53BA36 —
// `push 0; push 0` @0x53BA07/@0x53BA09; the entry store @0x53B9ED..0x53B9F3]
void test_timed_restart_window_sends_limit_zero() {
    auto world = make_world(gt::kAdvanceAndSecure, 9);
    const EntityHandle objective = zone(*world, 0, 0, {0.0f, 0.0f, 0.0f});
    const EntityHandle first = player(*world, 0, 1, {0.0f, 0.0f, 0.0f});
    const EntityHandle second = player(*world, 1, 2, {0.0f, 0.0f, 0.0f});
    world->zones.capture.active.push_back({objective, 1, 4, 9, first, {}, 1});
    world->zones.capture.requests.push_back({objective, 2, second});
    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    const auto windows = events_of<ZoneCaptureEvents::TimerWindow>(events);
    CHECK(!windows.empty() && windows[0].capturing_team == 2 &&
          windows[0].progress == 0 && windows[0].limit == 0);
    CHECK(!world->zones.capture.active.empty() &&
          world->zones.capture.active[0].limit == 9);
}

// FARP enforcement reads the live wholly-owned masks: a number whose entities
// disagree is nobody's, even when the cached mask still lists it.
// [orig: Server_EnforceZoneEntityTeams @0x51960A / @0x519618 ->
// ZoneSlotChain_GetOwnedZoneMask @0x4A2620]
void test_farp_reads_the_live_owned_masks() {
    auto world = make_world(gt::kAdvanceAndSecure);
    zone(*world, 1, 1, {0.0f, 0.0f, 0.0f});
    zone(*world, 2, 1, {500.0f, 0.0f, 0.0f});
    const EntityHandle split = zone(*world, 2, 1, {600.0f, 0.0f, 0.0f});
    zone(*world, 3, 2, {1000.0f, 0.0f, 0.0f});
    world->zones.build_chain_from_mission();
    Entity farp;
    farp.kind = EntityKind::Building;
    farp.has_item_def = true;
    farp.item_attrib2 = 0x2000;
    farp.zone_number = 2;
    farp.team = 1;
    farp.alive = true;
    const EntityHandle farp_handle = world->registry.spawn(2, farp);
    world->registry.get(split)->team = 2; // the cached mask still says team 1
    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    CHECK(world->registry.get(farp_handle)->team == 0);
}

// The cached ownership masks refresh only inside a numbered SpawnPoint flip:
// its RebuildMasksAndCheckUnchanged is RebuildOwnershipMasks' only caller, so
// an ownership change that is no flip leaves them as they were.
// [orig: ZoneSlotChain_RebuildMasksAndCheckUnchanged @0x4A2B7F;
// ZoneSlotChain_GetTeamMask @0x4A2350]
void test_masks_refresh_only_on_numbered_flips() {
    auto world = make_world(gt::kAdvanceAndSecure);
    const EntityHandle first = zone(*world, 1, 1, {0.0f, 0.0f, 0.0f});
    zone(*world, 2, 2, {500.0f, 0.0f, 0.0f});
    world->zones.build_chain_from_mission();
    world->registry.get(first)->team = 2;
    ZoneCaptureEvents events;
    world->zones.capture_second_tick(events);
    CHECK(world->zones.team_mask(1) == (1u << 1));
    CHECK(world->zones.team_mask(2) == (1u << 2));
}

// Objective gametypes pick the LAST team-matching unnumbered entry of the
// SORTED spawn list: a vehicle (type priority 2) sorts after a building.
// [orig: find_spawn_entity_for_team @0x4FC834..0x4FC864;
// Entity_BuildSpawnZoneList @0x43EAE0]
void test_objective_auto_deploy_walks_the_sorted_list() {
    auto world = make_world(gt::kObjectiveCoop);
    Entity truck;
    truck.kind = EntityKind::Item;
    truck.item_id = 700;
    truck.has_item_def = true;
    truck.item_type = 1;
    truck.team = 1;
    truck.is_spawn_point = true;
    truck.alive = true;
    const EntityHandle truck_handle = world->registry.spawn(1, truck);
    Entity bunker;
    bunker.kind = EntityKind::Building;
    bunker.item_id = 701;
    bunker.has_item_def = true;
    bunker.team = 1;
    bunker.is_spawn_point = true;
    bunker.alive = true;
    world->registry.spawn(2, bunker);
    const Entity *pick = world->zones.find_spawn_zone_for_team(1, gt::kObjectiveCoop);
    CHECK(pick != nullptr && pick->handle == truck_handle);
}

} // namespace

int main() {
    test_secure_pass_latches_before_reading_control();
    test_secure_pass_counts_every_enemy();
    test_secure_pass_empty_side_divides_by_zero();
    test_numbered_flip_feeds_the_capture_pair();
    test_all_owned_arm_is_as_and_cc_only();
    test_conquer_and_control_exempts_the_frontier();
    test_unnumbered_capture_scores_psp_takeover();
    test_timed_restart_window_sends_limit_zero();
    test_farp_reads_the_live_owned_masks();
    test_masks_refresh_only_on_numbered_flips();
    test_objective_auto_deploy_walks_the_sorted_list();
    if (failures != 0) {
        std::printf("zone_capture_parity_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("zone_capture_parity_test: all checks passed");
    return 0;
}
