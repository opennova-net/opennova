// Advance & Secure zone-slot chain (net-re §5.61): the frontier rule, ownership masks,
// the owned-zone (0x0F) mask, the control latch, the 0xFFFE auto-deploy pick, and the
// 0x0E pick resolve — pinned against the ASH_I5A authored shape (four type-1359 zone
// objects: zone 1 team 1, zone 2 x2 neutral, zone 3 team 2; 6003/6004 base markers).
// [orig: ZoneSlotChain_* @0x4A2350..0x4A2DE0; Server_ResolveSpawnTargetHandle @0x4fe110;
//  find_spawn_entity_for_team @0x4fc810]
#include <runtime/world/entity.h>
#include <runtime/world/collision.h>
#include <base/gameprofile/game_type.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>
#include <runtime/world/zone_capture.h>
#include <runtime/world/zone_chain.h>

#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

constexpr int32_t fx(double units) {
    return static_cast<int32_t>(units * 65536.0);
}

// One authored CT convex in section-local space. Retail produces capture
// contact from the type-10 Change Team Box, not the objective's zone radius.
// [orig: Entity_ComputeBoneCollisionForce @0x4AE150 type dispatch @0x4AEB7B;
// movement resolver capture callsite @0x4B31DD..0x4B3238]
CollisionModel capture_box_model(double half_x, double half_y, double height,
                                 int32_t type = bvol_type::kChangeTeamCT) {
    CollisionModel model;
    auto plane = [&](int nx, int ny, int nz, double distance) {
        CollisionPlane value;
        value.nx = static_cast<int16_t>(nx);
        value.ny = static_cast<int16_t>(ny);
        value.nz = static_cast<int16_t>(nz);
        value.dist = fx(distance);
        model.planes.push_back(value);
    };
    plane(16384, 0, 0, -half_x);
    plane(-16384, 0, 0, -half_x);
    plane(0, 16384, 0, -half_y);
    plane(0, -16384, 0, -half_y);
    plane(0, 0, 16384, -height);
    plane(0, 0, -16384, 0.0);

    CollisionVolume volume;
    volume.type = type;
    volume.min_x = fx(-half_x);
    volume.max_x = fx(half_x);
    volume.min_y = fx(-half_y);
    volume.max_y = fx(half_y);
    volume.min_z = 0;
    volume.max_z = fx(height);
    volume.plane_count = 6;
    model.volumes.push_back(volume);

    CollisionSection section;
    section.volume_count = 1;
    model.sections.push_back(section);
    return model;
}

EntityHandle spawn_zone(World &w, uint8_t zone_no, uint8_t team, Vec3 pos) {
    Entity e;
    e.kind = EntityKind::Item; // the ASH_I5A 1359 zone objects ride pool 1 (items)
    e.item_id = 1359;
    e.position = pos;
    e.yaw = 90; // mission yaw 90 -> identity collision placement
    e.team = team;
    e.zone_number = zone_no;
    e.is_capture_trigger = true; // ItemDefAttrib 0x20000 "ChangeTeam"
    e.is_spawn_point = true;     // ItemDefAttrib 0x40000 "SpawnPoint"
    e.alive = true;
    return w.registry.spawn(1, e);
}

void spawn_team_marker(World &w, int32_t type, Vec3 pos, uint8_t zone_no = 0) {
    Entity e;
    e.kind = EntityKind::Marker;
    e.item_id = type;
    e.position = pos;
    e.zone_number = zone_no;
    w.registry.spawn(3, e);
}

// The ASH_I5A chain: zone 1 (team 1) - zone 2 x2 (neutral) - zone 3 (team 2), plus the
// 6003/6004 base start markers (zone_number 0, as authored).
struct AshFixture {
    World w;
    CollisionWorld collision;
    EntityHandle z1, z2a, z2b, z3;
    AshFixture() {
        w.registry.configure_pool(0, 32); // organics (capture-loop soldiers)
        w.registry.configure_pool(1, 64); // items (the 1359 zone objects)
        w.registry.configure_pool(2, 64); // buildings
        w.registry.configure_pool(3, 64); // markers
        z1 = spawn_zone(w, 1, 1, {-444.9f, -413.6f, 21.5f});
        z2a = spawn_zone(w, 2, 0, {163.3f, 4.8f, 46.0f});
        z2b = spawn_zone(w, 2, 0, {-310.1f, -74.0f, 62.0f});
        z3 = spawn_zone(w, 3, 2, {338.2f, 371.2f, 26.6f});
        const int32_t capture_model =
                collision.add_model(capture_box_model(80.0, 80.0, 80.0));
        collision.assign_entity(z1, capture_model);
        collision.assign_entity(z2a, capture_model);
        collision.assign_entity(z2b, capture_model);
        collision.assign_entity(z3, capture_model);
        w.collision = &collision;
        for (int tick = 0; tick < 17; ++tick)
            collision.build_tick_tables(w);
        spawn_team_marker(w, 6003, {-394.1f, 449.6f, 11.0f});
        spawn_team_marker(w, 6004, {443.5f, -172.5f, 11.0f});
        MatchRules rules;
        rules.game_type = opennova::game_type::kAdvanceAndSecure;
        w.match.configure(rules);
        w.zones.build_chain_from_mission();
        w.zones.latch_control();
    }
};

void test_build_and_masks() {
    AshFixture f;
    CHECK(f.w.zones.chain.zones.size() == 4);
    // ownedMask = OR(1 << zone_no) per team [orig: ZoneSlotChain_RebuildOwnershipMasks @0x4A26C0].
    CHECK(f.w.zones.chain.owned_mask[1] == (1u << 1));
    CHECK(f.w.zones.chain.owned_mask[2] == (1u << 3));
    CHECK(f.w.zones.chain.owned_mask[0] == (1u << 2));
    // The ASH markers carry zone_number 0, so no assigned slot is seeded (as authored).
    CHECK(f.w.zones.chain.assigned_slot[1] == 0);
    CHECK(f.w.zones.chain.assigned_slot[2] == 0);
}

void test_frontier_rule() {
    AshFixture f;
    const Entity *z1 = f.w.registry.get(f.z1);
    const Entity *z2a = f.w.registry.get(f.z2a);
    const Entity *z3 = f.w.registry.get(f.z3);
    // Team 1 owns zone 1: zone 2 is adjacent (1+1) -> capturable; zone 3 is two hops -> not.
    // [orig: ZoneSlotChain_IsZoneCapturableByTeam @0x4A2450 adjacency walk]
    CHECK(f.w.zones.is_capturable(1, *z2a));
    CHECK(!f.w.zones.is_capturable(1, *z3));
    // A team's own zone is never "capturable" by it.
    CHECK(!f.w.zones.is_capturable(1, *z1));
    // Team 2 owns zone 3: zone 2 adjacent -> capturable; zone 1 not.
    CHECK(f.w.zones.is_capturable(2, *z2a));
    CHECK(!f.w.zones.is_capturable(2, *z1));
    // Both teams' frontier number is 2 [orig: ZoneSlotChain_FindFrontierZone @0x4A2AC0].
    CHECK(f.w.zones.frontier_zone(1) == 2);
    CHECK(f.w.zones.frontier_zone(2) == 2);
}

void test_owned_zone_mask_and_latch() {
    AshFixture f;
    // The 0x0F mask: team 2 wholly owns zone 3 -> 0x8, the golden ASH_I5A steady value.
    // [orig: ZoneSlotChain_GetOwnedZoneMask @0x4A2620; golden uniformMask=0x8]
    CHECK(f.w.zones.owned_zone_mask(2) == 0x8u);
    CHECK(f.w.zones.owned_zone_mask(1) == 0x2u);
    // Neither mid-zone entity is team 1's or 2's -> zone 2 in neither mask.
    // The latch: base zones sit on the enemy frontier? zone 1/3 are NOT reachable by the
    // enemy (two hops) -> control snaps to 1.0; the neutral mid zones ARE on both
    // frontiers -> control stays 0. [orig: Server_UpdateCaptureZoneEntities @0x519764]
    CHECK(f.w.registry.get(f.z1)->zone_control == 0x10000);
    CHECK(f.w.registry.get(f.z3)->zone_control == 0x10000);
    CHECK(f.w.registry.get(f.z2a)->zone_control == 0);
    CHECK(f.w.registry.get(f.z2b)->zone_control == 0);
}

void test_capture_progression() {
    AshFixture f;
    // Team 1 takes BOTH mid-zone entities (the instant numbered-zone flip zeroes control)
    // [orig: Server_UpdateCaptureZones queue drain @0x53bc46..0x53bc94].
    Entity *z2a = f.w.registry.get(f.z2a);
    Entity *z2b = f.w.registry.get(f.z2b);
    z2a->team = 1;
    z2a->zone_control = 0;
    z2b->team = 1;
    z2b->zone_control = 0;
    f.w.zones.rebuild_masks();
    // Now zone 3 is adjacent to team 1's zone 2 -> capturable; team 2 can push back on 2.
    const Entity *z3 = f.w.registry.get(f.z3);
    CHECK(f.w.zones.is_capturable(1, *z3));
    CHECK(f.w.zones.frontier_zone(1) == 2 ||
          f.w.zones.frontier_zone(1) == 3);
    // Owned mask now spans zones 1+2 for team 1 (both number-2 entities held).
    CHECK(f.w.zones.owned_zone_mask(1) == 0x6u);
    // One mid entity lost back to neutral -> the number-2 bit drops (not wholly owned).
    z2b->team = 0;
    f.w.zones.rebuild_masks();
    CHECK(f.w.zones.owned_zone_mask(1) == 0x2u);
}

void test_auto_deploy_pick() {
    AshFixture f;
    // 0xFFFE auto-deploy (AS 0x10010): team 1's zone 1 is NOT on team 2's frontier and
    // carries number 1 != frontier 2, so the first walk misses; the frontier then
    // steps down to 1 (team 1's mask holds number 1) and the retry picks the secured
    // zone 1. [orig: find_spawn_entity_for_team @0x4fc810 — the direction
    // @0x4fc88f..0x4fc8bb, the step @0x4fc941..0x4fc952]
    const Entity *base = f.w.zones.find_spawn_zone_for_team(1, 0x10010u);
    CHECK(base != nullptr && base->zone_number == 1);
    // Take zone 2 for team 1 (secured): now zone 2 IS on team 2's frontier -> the front line.
    Entity *z2a = f.w.registry.get(f.z2a);
    Entity *z2b = f.w.registry.get(f.z2b);
    z2a->team = 1;
    z2a->zone_control = 0x10000;
    z2b->team = 1;
    z2b->zone_control = 0x10000;
    f.w.zones.rebuild_masks();
    const Entity *pick = f.w.zones.find_spawn_zone_for_team(1, 0x10010u);
    CHECK(pick != nullptr && pick->zone_number == 2);
    // Contested (control < 1.0) removes it again [orig: @0x4fc92c entity+540 >= 0x10000];
    // the frontier (3) then steps down through the owned numbers 2 and 1 to the
    // secured zone 1.
    z2a->zone_control = 0x8000;
    z2b->zone_control = 0x8000;
    const Entity *rear = f.w.zones.find_spawn_zone_for_team(1, 0x10010u);
    CHECK(rear != nullptr && rear->zone_number == 1);
    // With the base unsecured too, every owned step misses.
    f.w.registry.get(f.z1)->zone_control = 0x8000;
    CHECK(f.w.zones.find_spawn_zone_for_team(1, 0x10010u) == nullptr);
}

void test_resolve_spawn_target() {
    AshFixture f;
    // A valid pick: team 1 picks its own zone-1 object [orig: Server_ResolveSpawnTargetHandle
    // @0x4fe110 — pool 0/1/2, SpawnPoint attrib, team match].
    const Entity *t = f.w.zones.resolve_spawn_target(1, f.z1.packed);
    CHECK(t != nullptr && t->zone_number == 1);
    // Cross-team pick refused; a teamless requester passes.
    CHECK(f.w.zones.resolve_spawn_target(2, f.z1.packed) == nullptr);
    CHECK(f.w.zones.resolve_spawn_target(0, f.z1.packed) != nullptr);
    // 0xFFFF and pool-3 handles refused.
    CHECK(f.w.zones.resolve_spawn_target(1, 0xFFFF) == nullptr);
    CHECK(f.w.zones.resolve_spawn_target(1, static_cast<uint16_t>(0x3000)) == nullptr);
    // The deploy pose: target origin z+1, target yaw [orig: @0x50d01c].
    const SpawnPointResult pose = resolve_player_spawn_pose(
        f.w, EntityHandle{}, t->handle, 0, 1, 0x10010u);
    CHECK(pose.found);
    CHECK(pose.position.z > t->position.z + 0.5f && pose.position.z < t->position.z + 1.5f);
}

void test_team_marker_selection() {
    AshFixture f;
    // AS (0x10010): team 1 -> the 6003 marker, team 2 -> the 6004 marker (net-re §5.61 —
    // without the split both teams landed on the first family type present).
    const SpawnPointResult t1 = resolve_player_spawn_pose(
        f.w, EntityHandle{}, EntityHandle{}, 0, 1, 0x10010u);
    const SpawnPointResult t2 = resolve_player_spawn_pose(
        f.w, EntityHandle{}, EntityHandle{}, 1, 2, 0x10010u);
    CHECK(t1.found && t1.position.x < 0.0f);  // 6003 sits west
    CHECK(t2.found && t2.position.x > 0.0f);  // 6004 sits east
}

void test_spawn_zone_presence_and_zone_info() {
    AshFixture f;
    // The join-time respawn-pending gate: ASH offers deploy-selectable zones
    // [orig: SpawnZoneList_GetCount() > 0 @0x51a6f2 -> stateByte |= 0x10; D-NET-156].
    CHECK(f.w.zones.has_spawn_zone());
    // The 0x0D packed zone byte = zoneNumber + 32*rank [orig:
    // serialize_entity_pool_to_packet_0 @0x503940 (the ZoneSlotChain_GetZoneInfo call
    // @0x503EEB)]. The two zone-2 entities share a number: descending rank within it —
    // golden ASH_I5A bunker 0x22 = zone 2 rank 1.
    const Entity *z2a = f.w.registry.get(f.z2a);
    const Entity *z2b = f.w.registry.get(f.z2b);
    const Entity *z1 = f.w.registry.get(f.z1);
    CHECK(zone_chain_zone_info_byte(f.w.zones.chain, *z2a) == (2 + 32 * 1));
    CHECK(zone_chain_zone_info_byte(f.w.zones.chain, *z2b) == (2 + 32 * 0));
    CHECK(zone_chain_zone_info_byte(f.w.zones.chain, *z1) == 1); // sole zone 1 -> rank 0
    // A zone-less world offers nothing to hold the deploy screen for.
    World bare;
    bare.registry.configure_pool(1, 4);
    CHECK(!bare.zones.has_spawn_zone());
}

// ---- Slice 2: the 1 Hz capture loop [orig: the Server_TickUpdate 1 Hz block] ----

EntityHandle spawn_soldier(World &w, uint8_t team, Vec3 pos) {
    Entity e;
    e.kind = EntityKind::Organic;
    e.item_id = 5305;
    e.player_class = 8;
    e.team = team;
    e.position = pos;
    e.health = 150;
    e.alive = true;
    e.net_move_input = Entity::kMoveOrderMoving;
    const EntityHandle handle = w.registry.spawn(0, e);
    // The capture census and scoring walk the player slots.
    // [orig: calculate_capture_zone_control_delta @0x501120;
    // CaptureZone_CheckProximityScoring @0x500C50]
    w.match.upsert_player({handle, static_cast<uint8_t>(handle.slot()), "Soldier"});
    return handle;
}

void capture_second(World &w, ZoneCaptureEvents &events) {
    if (w.collision != nullptr) {
        w.collision->refresh_after_registry_change(w);
        std::vector<EntityHandle> players;
        w.registry.for_each([&](const Entity &entity) {
            if (entity.handle.pool() == 0 && entity.player_class != 0 &&
                    entity.alive && entity.health > 0)
                players.push_back(entity.handle);
        });
        for (const EntityHandle handle : players) {
            Entity *player = w.registry.get(handle);
            int32_t pos[3] = {fx(player->position.x), fx(player->position.y),
                              fx(player->position.z)};
            int32_t vel[2] = {0, 0};
            int32_t vel_z = 0;
            int16_t health = static_cast<int16_t>(player->health);
            CollisionWorld::ResolveState state;
            w.collision->resolve_entity(
                    w, handle, state, pos, vel, vel_z, 0, fx(1.8), 0, 0,
                    /*is_player=*/true, /*is_authority=*/true, 0, 43, 1u,
                    health);
        }
    }
    w.zones.capture_contact_tick();
    w.zones.capture_second_tick(events);
}

template <typename T>
std::vector<T> events_of(const ZoneCaptureEvents &events) {
    std::vector<T> selected;
    for (const ZoneCaptureEvents::Event &event : events.ordered)
        if (const T *value = std::get_if<T>(&event))
            selected.push_back(*value);
    return selected;
}

// The control-delta formula pins [orig: calculate_capture_zone_control_delta @0x501120].
void test_control_delta_formula() {
    // A team-1 zone; `team1`/`team2` are the in-game census.
    auto delta = [](int presence, int team1, int team2, int speed_setting,
                    int shared_n, uint8_t zone_team = 1) {
        ZoneCaptureDeltaInput input;
        input.presence = presence;
        input.zone_team = zone_team;
        input.team_players = {0, team1, team2, 0, 0};
        input.speed_setting = speed_setting;
        input.spawn_zone_count = shared_n > 1 ? 1 : 0;
        input.shared_zone_entities = shared_n;
        return zone_capture_control_delta(input);
    };
    // 1 owner, 3-per-team server (6 total, no small-server boost), fallback base 12:
    // speed = 3*12 = 36 -> delta = 65536/36 = 1820 (secure in ~36 s at 1 Hz).
    CHECK(delta(1, 3, 3, -1, 1) == 65536 / 36);
    // Small-server boost: 1v1 (2 total) -> teamSize = 1 + (6-2)/2 = 3 -> speed 36.
    CHECK(delta(1, 1, 1, -1, 1) == 65536 / 36);
    // Retail's configured default, speed setting 1, selects base 24; negative
    // presence mirrors the sign. [orig: Config_SetDefaults @0x54D030]
    CHECK(delta(-2, 3, 3, 1, 1) == -(2 * 65536) / (3 * 24));
    // A zone number shared by 2 entities halves the speed (doubles the rate).
    CHECK(delta(1, 3, 3, -1, 2) == 65536 / 18);
    // Minimum magnitude 1.
    CHECK(delta(1, 200, 200, 2, 1) >= 1);
    CHECK(delta(0, 3, 3, -1, 1) == 0);
    // The sizing side follows the presence sign and the zone's 1/2 owner, not
    // the attacking team: a falling team-1 zone sizes by team 2, a falling
    // team-2 zone by team 1, and a rising neutral zone by team 2.
    // [orig: @0x5012AC..0x5012D7]
    CHECK(delta(-1, 9, 3, -1, 1, 1) == -(65536 / 36));
    CHECK(delta(-1, 3, 9, -1, 1, 2) == -(65536 / 36));
    CHECK(delta(1, 9, 3, -1, 1, 0) == 65536 / 36);
    // No speed guard: an empty sizing side on a full server divides by zero,
    // and the x87 conversion yields the integer indefinite.
    // [orig: `fdivr` @0x501456, _ftol2_sse @0x50145C]
    CHECK(delta(1, 6, 0, -1, 1, 0) == static_cast<int32_t>(0x80000000u));

    ZoneCaptureDeltaInput endgame;
    endgame.presence = 1;
    endgame.zone_team = 1;
    endgame.team_players = {0, 10, 10, 0, 0};
    endgame.spawn_zone_count = 4;
    endgame.team1_zones = 3;
    endgame.team2_zones = 1;
    endgame.numbered_zones = 4;
    endgame.game_time_minutes = 10;
    endgame.remaining_ticks = 0;
    // In the last half of a timed round, the side already holding more numbered
    // spawn zones gets up to a 50% speed-denominator reduction. Here
    // 120 - trunc(120 * 2/4 * 1/2) = 90.
    // [orig: calculate_capture_zone_control_delta @0x5013AB..0x50142A]
    CHECK(zone_capture_control_delta(endgame) == 65536 / 90);
    endgame.team1_zones = 1;
    endgame.team2_zones = 3;
    CHECK(zone_capture_control_delta(endgame) == 65536 / 120);
    endgame.team1_zones = 3;
    endgame.team2_zones = 1;
    endgame.remaining_ticks = 5 * 60 * 62; // halfway: the late-round factor is zero
    CHECK(zone_capture_control_delta(endgame) == 65536 / 120);
}

// An attacker on an unsecured frontier zone: instant flip to NEUTRAL (owned zones pass
// through neutral), control zeroed, masks rebuilt, spawn objects enforced; the next
// touch takes it; friendly presence then SECURES it (control -> 1.0 -> the 0x3B edge).
void test_capture_loop_flip_and_secure() {
    AshFixture f;
    // z2a starts NEUTRAL (team 0): a team-1 soldier standing in it flips it instantly
    // (neutral -> capturer). ASH bunkers author radius 70.
    Entity *z2a = f.w.registry.get(f.z2a);
    z2a->zone_radius = 70;
    f.w.registry.get(f.z2b)->zone_radius = 70;
    f.w.registry.get(f.z1)->zone_radius = 70;
    f.w.registry.get(f.z3)->zone_radius = 70;
    const EntityHandle s1 = spawn_soldier(f.w, 1, z2a->position);
    ZoneCaptureEvents ev;
    capture_second(f.w, ev);
    const auto controls = events_of<ZoneCaptureEvents::Control>(ev);
    const auto flips = events_of<ZoneCaptureEvents::Flip>(ev);
    CHECK(controls.size() == 4);            // 0x6F body per registered zone, every pass
    CHECK(flips.size() == 1);               // the instant numbered flip
    if (!flips.empty()) {
        CHECK(flips[0].old_team == 0);
        CHECK(flips[0].new_team == 1);      // neutral -> capturer directly
        CHECK(flips[0].capturer_team == 1);
        CHECK(flips[0].capturer == s1);     // scoring follows the actual touching Player
        CHECK(flips[0].scorers.size() == 1 && flips[0].scorers[0] == s1);
        // GameEvent_FlagCapture's numbered pair inputs: z2a's zone number and
        // chain rank, a neutral zone never in the enemy mask (changed), and
        // team 1's frontier after the refresh. [orig: GameEvent_FlagCapture
        // @0x50F737..0x50F912]
        CHECK(flips[0].numbered && flips[0].announce);
        CHECK(!flips[0].decided && flips[0].capturer_is_player);
        CHECK(!flips[0].unchanged);
        CHECK(flips[0].zone_number == 2);
    }
    CHECK(z2a->team == 1);
    CHECK(z2a->zone_control == 0);            // the new owner must SECURE it
    CHECK((f.w.zones.chain.owned_mask[1] & (1u << 2)) != 0); // masks rebuilt

    // Securing: the soldier stays; control rises by delta each pass until the latch/edge.
    int passes = 0;
    bool edged = false;
    while (passes < 200 && !edged) {
        capture_second(f.w, ev);
        for (const auto &se : events_of<ZoneCaptureEvents::Secure>(ev))
            if (se.zone == f.z2a && se.secured) edged = true;
        ++passes;
    }
    CHECK(edged);
    CHECK(z2a->zone_control == 0x10000);
    // 1 securer on a 1-player server: teamSize = 1 + (6-1)/2 = 3, retail's
    // default TakeoverSpeed setting 1 selects base 24; the shared number halves
    // speed 72 to 36 -> delta 1820 -> ~36 s. [orig: Config_SetDefaults @0x54D030]
    CHECK(passes >= 32 && passes <= 40);

    // An ENEMY (team 2) walks in while it is secured: control must FALL first (the
    // touch gate rejects a flip at control > 0), then the zero edge (0x3C) fires,
    // then the numbered-zone transaction changes through neutral and to team 2
    // in one drain.
    Entity *s1e = f.w.registry.get(s1);
    s1e->position = {0.0f, 0.0f, 0.0f}; // the defender leaves
    const EntityHandle s2 = spawn_soldier(f.w, 2, z2a->position);
    (void)s2;
    capture_second(f.w, ev);
    CHECK(events_of<ZoneCaptureEvents::Flip>(ev).empty()); // still partially secured
    CHECK(z2a->zone_control < 0x10000);
    bool zero_edge = false;
    int flip_pass = -1;
    for (int i = 0; i < 200 && flip_pass < 0; ++i) {
        capture_second(f.w, ev);
        for (const auto &se : events_of<ZoneCaptureEvents::Secure>(ev))
            if (se.zone == f.z2a && !se.secured) zero_edge = true;
        if (!events_of<ZoneCaptureEvents::Flip>(ev).empty()) flip_pass = i;
    }
    CHECK(zero_edge);
    CHECK(flip_pass >= 0);
    CHECK(z2a->team == 2);
}

// A neutral frontier is not implicitly a team-1 objective. Either team can take it,
// but simultaneous eligible Players contest it and must not pick a winner from pool
// iteration order. The queued capture keeps the exact touching Player for the later
// scoring/event pass. [orig: Server_OnPlayerTouchCaptureZone @0x500BA0 ->
// Server_UpdateCaptureZones @0x53B8F0 -> GameEvent_FlagCapture @0x50F6F0]
void test_neutral_capture_is_symmetric_and_actor_attributed() {
    {
        AshFixture f;
        Entity *zone = f.w.registry.get(f.z2a);
        zone->zone_radius = 70;
        const EntityHandle red = spawn_soldier(f.w, 2, zone->position);
        ZoneCaptureEvents ev;
        capture_second(f.w, ev);
        const auto flips = events_of<ZoneCaptureEvents::Flip>(ev);
        CHECK(flips.size() == 1);
        CHECK(zone->team == 2);
        if (!flips.empty()) {
            CHECK(flips[0].capturer_team == 2);
            CHECK(flips[0].capturer == red);
            CHECK(flips[0].scorers.size() == 1 && flips[0].scorers[0] == red);
        }
    }
    {
        AshFixture f;
        Entity *zone = f.w.registry.get(f.z2a);
        zone->zone_radius = 70;
        spawn_soldier(f.w, 1, zone->position);
        spawn_soldier(f.w, 2, zone->position);
        ZoneCaptureEvents ev;
        capture_second(f.w, ev);
        CHECK(events_of<ZoneCaptureEvents::Flip>(ev).empty());
        CHECK(zone->team == 0);
        CHECK(zone->zone_control == 0);
    }
}

EntityHandle spawn_unnumbered_zone(World &w, uint8_t team, Vec3 pos) {
    Entity zone;
    zone.kind = EntityKind::Item;
    zone.item_id = 1359;
    zone.position = pos;
    zone.yaw = 90; // identity collision placement
    zone.team = team;
    zone.zone_radius = 70;
    zone.zone_number = 0;
    zone.is_capture_trigger = true; // ItemDefAttrib 0x20000 ChangeTeam
    zone.is_spawn_point = true;     // ItemDefAttrib 0x40000 event gate
    zone.alive = true;
    const EntityHandle handle = w.registry.spawn(1, zone);
    if (w.collision != nullptr) {
        const int32_t model =
                w.collision->add_model(capture_box_model(80.0, 80.0, 80.0));
        w.collision->assign_entity(handle, model);
    }
    return handle;
}

// A movement collision queues the request; the periodic drain starts the timed
// transaction. The active entry then reports unique presence and advances by
// that rate until it changes the zone owner. [orig:
// Entity_MovementCollisionResolver @0x4B2BD0 -> Server_OnPlayerTouchCaptureZone
// @0x500BA0; the CaptureCtx_* / Server_UpdateCaptureZones run from
// CaptureCtx_RemoveQueueEntries @0x53B340 to 0x53B8F0]
void test_unnumbered_timed_capture_and_presence() {
    AshFixture f;
    MatchRules rules = f.w.match.rules();
    rules.capture_duration_seconds = 3;
    f.w.match.configure(rules);

    const Vec3 pos{20.0f, 30.0f, 4.0f};
    const EntityHandle zone = spawn_unnumbered_zone(f.w, 2, pos);
    const EntityHandle first = spawn_soldier(f.w, 1, pos);
    ZoneCaptureEvents ev;

    capture_second(f.w, ev);
    CHECK(f.w.registry.get(zone)->team == 0); // old owner neutralized at start
    const auto starts = events_of<ZoneCaptureEvents::TimedStart>(ev);
    const auto start_windows = events_of<ZoneCaptureEvents::TimerWindow>(ev);
    CHECK(starts.size() == 1);
    CHECK(start_windows.size() == 1);
    if (!start_windows.empty()) {
        CHECK(start_windows[0].zone == zone);
        // The start window carries the owner as the drain found it (team 2),
        // not the neutral it just applied. [orig: state0 @0x53BADC, the window
        // @0x53BBBE..0x53BC02]
        CHECK(start_windows[0].current_team == 2);
        CHECK(start_windows[0].capturing_team == 1);
        CHECK(start_windows[0].progress == 0);
        CHECK(start_windows[0].limit == 3);
        CHECK(start_windows[0].rate == 1);
    }

    // A second unique live mover raises the active rate to two and emits 0x6C.
    const EntityHandle second = spawn_soldier(f.w, 1, pos);
    capture_second(f.w, ev);
    const auto advanced_presence = events_of<ZoneCaptureEvents::Presence>(ev);
    const auto advanced_windows = events_of<ZoneCaptureEvents::TimerWindow>(ev);
    CHECK(advanced_presence.size() == 1);
    CHECK(advanced_presence[0].zone == zone && advanced_presence[0].count == 2);
    CHECK(advanced_windows.size() == 1);
    CHECK(advanced_windows[0].progress == 2 && advanced_windows[0].rate == 2);
    CHECK(events_of<ZoneCaptureEvents::TimedCompletion>(ev).empty());

    // Back to one occupant: 0x6C reports one, progress reaches the authored limit,
    // and completion retains the original capturer for score/event attribution.
    f.w.registry.get(second)->position = {500.0f, 500.0f, 0.0f};
    capture_second(f.w, ev);
    const auto completed_presence = events_of<ZoneCaptureEvents::Presence>(ev);
    const auto completed_windows = events_of<ZoneCaptureEvents::TimerWindow>(ev);
    const auto completions = events_of<ZoneCaptureEvents::TimedCompletion>(ev);
    CHECK(completed_presence.size() == 1);
    CHECK(completed_presence[0].count == 1);
    CHECK(completed_windows.size() == 1 && completed_windows[0].progress == 3);
    CHECK(completions.size() == 1);
    CHECK(f.w.registry.get(zone)->team == 1);
    if (!completions.empty()) {
        CHECK(completions[0].zone == zone);
        CHECK(completions[0].capturer == first);
        CHECK(completions[0].new_team == 1);
    }
}

// Contact is produced by the movement resolver's authored CT shape even when
// MoveOrder is idle. Opposing requests in one drain contest and cancel rather
// than selecting pool order. [orig: movement callsite @0x4B31DD..0x4B3238;
// Server_UpdateCaptureZones @0x53B8F0 — the conflicting-request walk
// @0x53BAFB..0x53BB1C, the drop @0x53BC1A]
void test_capture_contact_has_no_move_gate_and_contests() {
    const Vec3 pos{20.0f, 30.0f, 4.0f};
    {
        AshFixture f;
        MatchRules rules = f.w.match.rules();
        rules.capture_duration_seconds = 3;
        f.w.match.configure(rules);
        const EntityHandle zone = spawn_unnumbered_zone(f.w, 0, pos);
        const EntityHandle blue = spawn_soldier(f.w, 1, pos);
        f.w.registry.get(blue)->net_move_input = 0;
        ZoneCaptureEvents ev;
        capture_second(f.w, ev);
        CHECK(events_of<ZoneCaptureEvents::TimedStart>(ev).size() == 1);
        CHECK(f.w.registry.get(zone)->team == 0);
    }
    {
        AshFixture f;
        MatchRules rules = f.w.match.rules();
        rules.capture_duration_seconds = 3;
        f.w.match.configure(rules);
        const EntityHandle zone = spawn_unnumbered_zone(f.w, 0, pos);
        spawn_soldier(f.w, 1, pos);
        spawn_soldier(f.w, 2, pos);
        ZoneCaptureEvents ev;
        capture_second(f.w, ev);
        CHECK(events_of<ZoneCaptureEvents::TimedStart>(ev).empty());
        CHECK(events_of<ZoneCaptureEvents::TimerWindow>(ev).empty());
        CHECK(f.w.registry.get(zone)->team == 0);
    }
}

// A point can be well inside the gameplay/proximity radius while remaining
// outside the authored CT convex. Only the latter is the retail capture touch;
// changing a trigger's authored collision model changes capture without
// changing zone_radius. [orig: contact flag 0x200 @0x4AEB7B; gated callback
// @0x4B31DD..0x4B3238; Server_OnPlayerTouchCaptureZone @0x500BA0]
void test_capture_contact_uses_authored_change_team_box() {
    AshFixture f;
    MatchRules rules = f.w.match.rules();
    rules.capture_duration_seconds = 3;
    f.w.match.configure(rules);

    const Vec3 center{20.0f, 30.0f, 4.0f};
    const EntityHandle zone = spawn_unnumbered_zone(f.w, 0, center);
    Entity *zone_entity = f.w.registry.get(zone);
    zone_entity->zone_radius = 100;
    const int32_t narrow_ct =
            f.collision.add_model(capture_box_model(1.0, 4.0, 3.0));
    f.collision.assign_entity(zone, narrow_ct);

    const EntityHandle player =
            spawn_soldier(f.w, 1, {center.x + 10.0f, center.y, center.z});
    ZoneCaptureEvents events;
    capture_second(f.w, events);
    CHECK(events_of<ZoneCaptureEvents::TimedStart>(events).empty());
    CHECK(f.w.registry.get(zone)->team == 0);

    f.w.registry.get(player)->position = center;
    capture_second(f.w, events);
    CHECK(events_of<ZoneCaptureEvents::TimedStart>(events).size() == 1);
}

// The secure pass also converts live pool-1/2 entities carrying ItemDefAttrib2
// bit 2 when they sit inside a numbered zone. Pool 3 and out-of-radius entities
// are untouched. [orig: Server_UpdateCaptureZoneEntities @0x519690]
void test_numbered_zone_converts_attrib2_entities() {
    AshFixture f;
    Entity *zone = f.w.registry.get(f.z1);
    zone->zone_radius = 70;
    auto spawn_convertible = [&](int pool, Vec3 pos) {
        Entity e;
        e.kind = pool == 2 ? EntityKind::Building : EntityKind::Item;
        e.has_item_def = true;
        e.position = pos;
        e.team = 2;
        e.item_attrib2 = 2;
        e.alive = true;
        return f.w.registry.spawn(pool, e);
    };
    const EntityHandle inside = spawn_convertible(2, zone->position);
    const EntityHandle outside = spawn_convertible(1, {500.0f, 500.0f, 0.0f});
    const EntityHandle wrong_pool = spawn_convertible(3, zone->position);
    ZoneCaptureEvents ev;
    capture_second(f.w, ev);
    CHECK(f.w.registry.get(inside)->team == 1);
    CHECK(f.w.registry.get(outside)->team == 2);
    CHECK(f.w.registry.get(wrong_pool)->team == 2);
    const auto changes = events_of<ZoneCaptureEvents::TeamChange>(ev);
    CHECK(changes.size() == 1);
    if (!changes.empty()) {
        CHECK(changes[0].entity == inside && changes[0].team == 1);
        CHECK(changes[0].net_id == 0 && changes[0].anim_slot == 0);
    }
}

// An owned instant capture emits two distinct S2C 0x50 snapshots before its
// capture announcement: old owner -> neutral, then neutral -> capturer. Keeping
// only the final entity state would turn both records into team 2.
// [orig: Server_UpdateCaptureZones @0x53BC46..0x53BC68;
// Server_ChangeEntityTeam @0x518D70]
void test_instant_capture_preserves_team_change_order() {
    AshFixture f;
    Entity *zone = f.w.registry.get(f.z2a);
    zone->zone_radius = 70;
    zone->team = 1;
    zone->zone_control = 0;
    f.w.zones.rebuild_masks();
    spawn_soldier(f.w, 2, zone->position);

    ZoneCaptureEvents ev;
    capture_second(f.w, ev);
    CHECK(ev.ordered.size() >= 3);
    if (ev.ordered.size() >= 3) {
        const size_t tail = ev.ordered.size() - 3;
        const auto *neutral = std::get_if<ZoneCaptureEvents::TeamChange>(
                &ev.ordered[tail]);
        const auto *captured = std::get_if<ZoneCaptureEvents::TeamChange>(
                &ev.ordered[tail + 1]);
        const auto *flip = std::get_if<ZoneCaptureEvents::Flip>(
                &ev.ordered[tail + 2]);
        CHECK(neutral != nullptr && neutral->entity == f.z2a &&
              neutral->team == 0);
        CHECK(captured != nullptr && captured->entity == f.z2a &&
              captured->team == 2);
        CHECK(flip != nullptr && flip->zone == f.z2a &&
              flip->old_team == 1 && flip->new_team == 2);
    }
}

// The enforcement list is ItemDefAttrib2 FARP (0x2000), not SpawnPoint. It can
// force a numbered entity neutral, and because enforcement precedes the capture
// queue drain it observes a new owner on the following second.
// [orig: Entity_BuildProximityListFromPools @0x43ED60;
// Server_EnforceZoneEntityTeams @0x519600; Server_TickUpdate @0x51DF7D]
void test_farp_enforcement_uses_prior_capture_masks() {
    AshFixture f;
    Entity *zone = f.w.registry.get(f.z2a);
    zone->zone_radius = 70;

    Entity farp;
    farp.kind = EntityKind::Building;
    farp.has_item_def = true;
    farp.item_attrib2 = 0x2000;
    farp.zone_number = 2;
    farp.team = 2;
    farp.alive = true;
    const EntityHandle farp_handle = f.w.registry.spawn(2, farp);
    spawn_soldier(f.w, 1, zone->position);

    ZoneCaptureEvents ev;
    capture_second(f.w, ev);
    CHECK(zone->team == 1);
    CHECK(f.w.registry.get(farp_handle)->team == 0);
    auto changes = events_of<ZoneCaptureEvents::TeamChange>(ev);
    CHECK(changes.size() == 2);
    if (changes.size() == 2) {
        CHECK(changes[0].entity == farp_handle && changes[0].team == 0);
        CHECK(changes[1].entity == f.z2a && changes[1].team == 1);
    }

    // The masks are the live wholly-owned ones: zone 2's other entity is still
    // neutral, so number 2 is nobody's and the FARP stays neutral.
    // [orig: Server_EnforceZoneEntityTeams @0x519600 — ZoneSlotChain_GetOwnedZoneMask
    //  @0x51960A / @0x519618]
    capture_second(f.w, ev);
    CHECK(f.w.registry.get(farp_handle)->team == 0);
    CHECK(events_of<ZoneCaptureEvents::TeamChange>(ev).empty());

    f.w.registry.get(f.z2b)->team = 1;
    capture_second(f.w, ev);
    CHECK(f.w.registry.get(farp_handle)->team == 1);
    changes = events_of<ZoneCaptureEvents::TeamChange>(ev);
    CHECK(changes.size() == 1 && changes[0].entity == farp_handle &&
          changes[0].team == 1);
}

// The deploy/spawn-zone registry: collect pools 2 then 1, sort by the composite
// (typePriority, unitType, zone#) key, AABB over the registered zones — the letter/
// pick-index space the deploy screen and the 0x6E zoneIdx ride.
// [orig: Entity_BuildSpawnZoneList @0x43EAE0; SpawnZoneList_IndexOf @0x43B990]
void test_spawn_zone_registry() {
    AshFixture f;
    // A pool-2 building zone (zone 0, ground type) and a pool-1 VEHICLE spawn point:
    // vehicles sort LAST (typePriority 2), ground zones lead by zone number.
    Entity building;
    building.kind = EntityKind::Building;
    building.item_id = 0x0500;
    building.position = {500.0f, 600.0f, 0.0f};
    building.team = 1;
    building.is_spawn_point = true;
    building.alive = true;
    const EntityHandle bh = f.w.registry.spawn(2, building);
    Entity vehicle;
    vehicle.kind = EntityKind::Item;
    vehicle.item_id = 2001;
    vehicle.position = {-600.0f, -700.0f, 0.0f};
    vehicle.team = 1;
    vehicle.item_type = 1; // ItemDef.type 1 = vehicle -> typePriority 2
    vehicle.is_spawn_point = true;
    vehicle.alive = true;
    const EntityHandle vh = f.w.registry.spawn(1, vehicle);
    const SpawnZoneRegistry reg = f.w.zones.build_spawn_zone_list();
    CHECK(reg.entries.size() == 6); // 4 zones + building + vehicle
    // Ground zones ascend by zone number (building zone 0 first), vehicle trails.
    CHECK(reg.entries[0].packed == bh.packed);
    CHECK(reg.entries[1].packed == f.z1.packed);
    CHECK(reg.entries[2].packed == f.z2a.packed);
    CHECK(reg.entries[3].packed == f.z2b.packed);
    CHECK(reg.entries[4].packed == f.z3.packed);
    CHECK(reg.entries[5].packed == vh.packed);
    CHECK(spawn_zone_index_of(reg, f.z3) == 4);
    CHECK(spawn_zone_index_of(reg, EntityHandle::make(0, 5)) == -1);
    // The AABB spans every registered zone (16.16 world).
    CHECK(reg.min_x == to_fixed(-600.0) && reg.max_x == to_fixed(500.0));
    CHECK(reg.min_y == to_fixed(-700.0) && reg.max_y == to_fixed(600.0));
}

// A both-zero composite key falls through to raw entity-address order. Retail
// owns all five pools in one contiguous allocation: pool 1 starts at +232420,
// pool 2 at +1865416, so an unnumbered pool-1 spawn point sorts before a
// pool-2 spawn point even though collection walks pool 2 first.
// [orig: EntityPool_Allocate @0x442130; Entity_BuildSpawnZoneList
// @0x43EAE0, address compare @0x43ECC6]
void test_spawn_zone_zero_key_uses_retail_pool_address_order() {
    World world;
    world.registry.configure_pool(1, 2);
    world.registry.configure_pool(2, 2);

    Entity pool2;
    pool2.kind = EntityKind::Building;
    pool2.is_spawn_point = true;
    pool2.alive = true;
    const EntityHandle pool2_handle = world.registry.spawn(2, pool2);

    Entity pool1;
    pool1.kind = EntityKind::Item;
    pool1.is_spawn_point = true;
    pool1.alive = true;
    const EntityHandle pool1_handle = world.registry.spawn(1, pool1);

    const SpawnZoneRegistry reg = world.zones.build_spawn_zone_list();
    CHECK(reg.entries.size() == 2);
    CHECK(reg.entries[0] == pool1_handle);
    CHECK(reg.entries[1] == pool2_handle);
}

} // namespace

int main() {
    test_build_and_masks();
    test_frontier_rule();
    test_owned_zone_mask_and_latch();
    test_capture_progression();
    test_auto_deploy_pick();
    test_resolve_spawn_target();
    test_team_marker_selection();
    test_spawn_zone_presence_and_zone_info();
    test_control_delta_formula();
    test_capture_loop_flip_and_secure();
    test_neutral_capture_is_symmetric_and_actor_attributed();
    test_unnumbered_timed_capture_and_presence();
    test_capture_contact_has_no_move_gate_and_contests();
    test_capture_contact_uses_authored_change_team_box();
    test_numbered_zone_converts_attrib2_entities();
    test_instant_capture_preserves_team_change_order();
    test_farp_enforcement_uses_prior_capture_masks();
    test_spawn_zone_registry();
    test_spawn_zone_zero_key_uses_retail_pool_address_order();
    if (failures == 0) std::printf("zone_chain_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
