// Advance & Secure zone-slot chain (net-re §5.61): the frontier rule, ownership masks,
// the owned-zone (0x0F) mask, the control latch, the 0xFFFE auto-deploy pick, and the
// 0x0E pick resolve — pinned against the ASH_I5A authored shape (four type-1359 zone
// objects: zone 1 team 1, zone 2 x2 neutral, zone 3 team 2; 6003/6004 base markers).
// [orig: ZoneSlotChain_* @0x4A2350..0x4A2DE0; Server_ResolveSpawnTargetHandle @0x4fe110;
//  find_spawn_entity_for_team @0x4fc810]
#include "world/entity.h"
#include "world/spawn_select.h"
#include "world/world.h"
#include "world/zone_capture.h"
#include "world/zone_chain.h"

#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

EntityHandle spawn_zone(World &w, uint8_t zone_no, uint8_t team, Vec3 pos) {
    Entity e;
    e.kind = EntityKind::Item; // the ASH_I5A 1359 zone objects ride pool 1 (items)
    e.item_id = 1359;
    e.position = pos;
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
        spawn_team_marker(w, 6003, {-394.1f, 449.6f, 11.0f});
        spawn_team_marker(w, 6004, {443.5f, -172.5f, 11.0f});
        zone_chain_build_from_mission(w, w.zone_chain);
        zone_chain_latch_control(w, w.zone_chain);
    }
};

void test_build_and_masks() {
    AshFixture f;
    CHECK(f.w.zone_chain.zones.size() == 4);
    // ownedMask = OR(1 << zone_no) per team [orig: ZoneSlotChain_RebuildOwnershipMasks @0x4A26C0].
    CHECK(f.w.zone_chain.owned_mask[1] == (1u << 1));
    CHECK(f.w.zone_chain.owned_mask[2] == (1u << 3));
    CHECK(f.w.zone_chain.owned_mask[0] == (1u << 2));
    // The ASH markers carry zone_number 0, so no assigned slot is seeded (as authored).
    CHECK(f.w.zone_chain.assigned_slot[1] == 0);
    CHECK(f.w.zone_chain.assigned_slot[2] == 0);
}

void test_frontier_rule() {
    AshFixture f;
    const Entity *z1 = f.w.registry.get(f.z1);
    const Entity *z2a = f.w.registry.get(f.z2a);
    const Entity *z3 = f.w.registry.get(f.z3);
    // Team 1 owns zone 1: zone 2 is adjacent (1+1) -> capturable; zone 3 is two hops -> not.
    // [orig: ZoneSlotChain_IsZoneCapturableByTeam @0x4A2450 adjacency walk]
    CHECK(zone_chain_is_capturable(f.w, f.w.zone_chain, 1, *z2a));
    CHECK(!zone_chain_is_capturable(f.w, f.w.zone_chain, 1, *z3));
    // A team's own zone is never "capturable" by it.
    CHECK(!zone_chain_is_capturable(f.w, f.w.zone_chain, 1, *z1));
    // Team 2 owns zone 3: zone 2 adjacent -> capturable; zone 1 not.
    CHECK(zone_chain_is_capturable(f.w, f.w.zone_chain, 2, *z2a));
    CHECK(!zone_chain_is_capturable(f.w, f.w.zone_chain, 2, *z1));
    // Both teams' frontier number is 2 [orig: ZoneSlotChain_FindFrontierZone @0x4A2AC0].
    CHECK(zone_chain_frontier_zone(f.w, f.w.zone_chain, 1) == 2);
    CHECK(zone_chain_frontier_zone(f.w, f.w.zone_chain, 2) == 2);
}

void test_owned_zone_mask_and_latch() {
    AshFixture f;
    // The 0x0F mask: team 2 wholly owns zone 3 -> 0x8, the golden ASH_I5A steady value.
    // [orig: ZoneSlotChain_GetOwnedZoneMask @0x4A2620; golden uniformMask=0x8]
    CHECK(zone_chain_owned_zone_mask(f.w, f.w.zone_chain, 2) == 0x8u);
    CHECK(zone_chain_owned_zone_mask(f.w, f.w.zone_chain, 1) == 0x2u);
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
    zone_chain_rebuild_masks(f.w, f.w.zone_chain);
    // Now zone 3 is adjacent to team 1's zone 2 -> capturable; team 2 can push back on 2.
    const Entity *z3 = f.w.registry.get(f.z3);
    CHECK(zone_chain_is_capturable(f.w, f.w.zone_chain, 1, *z3));
    CHECK(zone_chain_frontier_zone(f.w, f.w.zone_chain, 1) == 2 ||
          zone_chain_frontier_zone(f.w, f.w.zone_chain, 1) == 3);
    // Owned mask now spans zones 1+2 for team 1 (both number-2 entities held).
    CHECK(zone_chain_owned_zone_mask(f.w, f.w.zone_chain, 1) == 0x6u);
    // One mid entity lost back to neutral -> the number-2 bit drops (not wholly owned).
    z2b->team = 0;
    zone_chain_rebuild_masks(f.w, f.w.zone_chain);
    CHECK(zone_chain_owned_zone_mask(f.w, f.w.zone_chain, 1) == 0x2u);
}

void test_auto_deploy_pick() {
    AshFixture f;
    // 0xFFFE auto-deploy (AS 0x10010): team 1's zone 1 is NOT on team 2's frontier and
    // carries number 1 != frontier 2 -> no zone auto-pick (falls back to base markers).
    // [orig: find_spawn_entity_for_team @0x4fc810 -> requestedHandle -1 on miss]
    CHECK(find_spawn_zone_for_team(f.w, f.w.zone_chain, 1, 0x10010u) == nullptr);
    // Take zone 2 for team 1 (secured): now zone 2 IS on team 2's frontier -> the front line.
    Entity *z2a = f.w.registry.get(f.z2a);
    Entity *z2b = f.w.registry.get(f.z2b);
    z2a->team = 1;
    z2a->zone_control = 0x10000;
    z2b->team = 1;
    z2b->zone_control = 0x10000;
    zone_chain_rebuild_masks(f.w, f.w.zone_chain);
    const Entity *pick = find_spawn_zone_for_team(f.w, f.w.zone_chain, 1, 0x10010u);
    CHECK(pick != nullptr && pick->zone_number == 2);
    // Contested (control < 1.0) removes it again [orig: @0x4fc92c entity+540 >= 0x10000].
    z2a->zone_control = 0x8000;
    z2b->zone_control = 0x8000;
    CHECK(find_spawn_zone_for_team(f.w, f.w.zone_chain, 1, 0x10010u) == nullptr);
}

void test_resolve_spawn_target() {
    AshFixture f;
    // A valid pick: team 1 picks its own zone-1 object [orig: Server_ResolveSpawnTargetHandle
    // @0x4fe110 — pool 0/1/2, SpawnPoint attrib, team match].
    const Entity *t = resolve_spawn_target(f.w, 1, f.z1.packed);
    CHECK(t != nullptr && t->zone_number == 1);
    // Cross-team pick refused; a teamless requester passes.
    CHECK(resolve_spawn_target(f.w, 2, f.z1.packed) == nullptr);
    CHECK(resolve_spawn_target(f.w, 0, f.z1.packed) != nullptr);
    // 0xFFFF and pool-3 handles refused.
    CHECK(resolve_spawn_target(f.w, 1, 0xFFFF) == nullptr);
    CHECK(resolve_spawn_target(f.w, 1, static_cast<uint16_t>(0x3000)) == nullptr);
    // The deploy pose: target origin z+1, target yaw [orig: @0x50d01c].
    const SpawnPointResult pose = spawn_pose_for_target(*t);
    CHECK(pose.found);
    CHECK(pose.position.z > t->position.z + 0.5f && pose.position.z < t->position.z + 1.5f);
}

void test_team_marker_selection() {
    AshFixture f;
    // AS (0x10010): team 1 -> the 6003 marker, team 2 -> the 6004 marker (net-re §5.61 —
    // without the split both teams landed on the first family type present).
    const SpawnPointResult t1 = select_player_spawn_for_team(f.w, 1, 0x10010u);
    const SpawnPointResult t2 = select_player_spawn_for_team(f.w, 2, 0x10010u);
    CHECK(t1.found && t1.position.x < 0.0f);  // 6003 sits west
    CHECK(t2.found && t2.position.x > 0.0f);  // 6004 sits east
}

void test_spawn_zone_presence_and_zone_info() {
    AshFixture f;
    // The join-time respawn-pending gate: ASH offers deploy-selectable zones
    // [orig: SpawnZoneList_GetCount() > 0 @0x51a6f2 -> stateByte |= 0x10; D-NET-156].
    CHECK(world_has_spawn_zone(f.w));
    // The 0x0D packed zone byte = zoneNumber + 32*rank [orig: ZoneSlotChain_GetZoneInfo
    // @0x503eeb]. The two zone-2 entities share a number: descending rank within it —
    // golden ASH_I5A bunker 0x22 = zone 2 rank 1.
    const Entity *z2a = f.w.registry.get(f.z2a);
    const Entity *z2b = f.w.registry.get(f.z2b);
    const Entity *z1 = f.w.registry.get(f.z1);
    CHECK(zone_chain_zone_info_byte(f.w.zone_chain, *z2a) == (2 + 32 * 1));
    CHECK(zone_chain_zone_info_byte(f.w.zone_chain, *z2b) == (2 + 32 * 0));
    CHECK(zone_chain_zone_info_byte(f.w.zone_chain, *z1) == 1); // sole zone 1 -> rank 0
    // A zone-less world offers nothing to hold the deploy screen for.
    World bare;
    bare.registry.configure_pool(1, 4);
    CHECK(!world_has_spawn_zone(bare));
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
    return w.registry.spawn(0, e);
}

// The control-delta formula pins [orig: calculate_capture_zone_control_delta @0x501120].
void test_control_delta_formula() {
    // 1 attacker, 3-per-team server (6 total, no small-server boost), default base 12:
    // speed = 3*12 = 36 -> delta = 65536/36 = 1820 (secure in ~36 s at 1 Hz).
    CHECK(zone_capture_control_delta(1, 3, 6, -1, 1) == 65536 / 36);
    // Small-server boost: 1v1 (2 total) -> teamSize = 1 + (6-2)/2 = 3 -> speed 36.
    CHECK(zone_capture_control_delta(1, 1, 2, -1, 1) == 65536 / 36);
    // Speed setting 1 doubles the base (24); negative presence mirrors the sign.
    CHECK(zone_capture_control_delta(-2, 3, 6, 1, 1) == -(2 * 65536) / (3 * 24));
    // A zone number shared by 2 entities halves the speed (doubles the rate).
    CHECK(zone_capture_control_delta(1, 3, 6, -1, 2) == 65536 / 18);
    // Minimum magnitude 1.
    CHECK(zone_capture_control_delta(1, 200, 200, 2, 1) >= 1);
    CHECK(zone_capture_control_delta(0, 3, 6, -1, 1) == 0);
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
    zone_capture_tick(f.w, f.w.zone_chain, ev, -1);
    CHECK(ev.control.size() == 4);            // 0x6F body per registered zone, every pass
    CHECK(ev.flips.size() == 1);              // the instant numbered flip
    if (!ev.flips.empty()) {
        CHECK(ev.flips[0].old_team == 0);
        CHECK(ev.flips[0].new_team == 1);     // neutral -> capturer directly
        CHECK(ev.flips[0].capturer_team == 1);
        CHECK(!ev.flips[0].suppressed);
    }
    CHECK(z2a->team == 1);
    CHECK(z2a->zone_control == 0);            // the new owner must SECURE it
    CHECK((f.w.zone_chain.owned_mask[1] & (1u << 2)) != 0); // masks rebuilt

    // Securing: the soldier stays; control rises by delta each pass until the latch/edge.
    int passes = 0;
    bool edged = false;
    while (passes < 200 && !edged) {
        zone_capture_tick(f.w, f.w.zone_chain, ev, -1);
        for (const auto &se : ev.secure_edges)
            if (se.zone == f.z2a && se.secured) edged = true;
        ++passes;
    }
    CHECK(edged);
    CHECK(z2a->zone_control == 0x10000);
    // 1 securer on a 1-player server: teamSize = 1 + (6-1)/2 = 3, base 12 -> speed 36,
    // HALVED by the shared zone number (two number-2 bunkers) -> delta 3640 -> ~18 s.
    CHECK(passes >= 15 && passes <= 22);

    // An ENEMY (team 2) walks in while it is secured: control must FALL first (the
    // touch gate rejects a flip at control > 0), then the zero edge (0x3C) fires,
    // then the flip neutralizes the OWNED zone (via neutral).
    Entity *s1e = f.w.registry.get(s1);
    s1e->position = {0.0f, 0.0f, 0.0f}; // the defender leaves
    const EntityHandle s2 = spawn_soldier(f.w, 2, z2a->position);
    (void)s2;
    zone_capture_tick(f.w, f.w.zone_chain, ev, -1);
    CHECK(ev.flips.empty());                  // still partially secured -> no flip yet
    CHECK(z2a->zone_control < 0x10000);
    bool zero_edge = false;
    int flip_pass = -1;
    for (int i = 0; i < 200 && flip_pass < 0; ++i) {
        zone_capture_tick(f.w, f.w.zone_chain, ev, -1);
        for (const auto &se : ev.secure_edges)
            if (se.zone == f.z2a && !se.secured) zero_edge = true;
        if (!ev.flips.empty()) flip_pass = i;
    }
    CHECK(zero_edge);
    CHECK(flip_pass >= 0);
    CHECK(z2a->team == 0);                    // owned zone neutralizes first
    // The same enemy takes the now-neutral zone on the next pass.
    zone_capture_tick(f.w, f.w.zone_chain, ev, -1);
    CHECK(!ev.flips.empty());
    CHECK(z2a->team == 2);
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
    const SpawnZoneRegistry reg = build_spawn_zone_list(f.w);
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
    test_spawn_zone_registry();
    if (failures == 0) std::printf("zone_chain_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
