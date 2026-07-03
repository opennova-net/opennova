// Advance & Secure zone-slot chain (net-re §5.61): the frontier rule, ownership masks,
// the owned-zone (0x0F) mask, the control latch, the 0xFFFE auto-deploy pick, and the
// 0x0E pick resolve — pinned against the ASH_I5A authored shape (four type-1359 zone
// objects: zone 1 team 1, zone 2 x2 neutral, zone 3 team 2; 6003/6004 base markers).
// [orig: ZoneSlotChain_* @0x4A2350..0x4A2DE0; Server_ResolveSpawnTargetHandle @0x4fe110;
//  find_spawn_entity_for_team @0x4fc810]
#include "world/entity.h"
#include "world/spawn_select.h"
#include "world/world.h"
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
    if (failures == 0) std::printf("zone_chain_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
