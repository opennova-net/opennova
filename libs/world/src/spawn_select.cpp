#include "world/spawn_select.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "world/entity.h" // Entity, EntityKind
#include "world/world.h"  // World, EntityRegistry registry
#include "world/zone_chain.h"

namespace opennova::world {

SpawnPointResult select_player_spawn(const World &world, const int32_t *types, size_t count) {
    // One pass: bucket every start-family marker (by type) and collect the "avoid" set (live enemy
    // organics). [orig: Entity_FindBestSpawnPoint @0x50ccc0 scores pool-3 markers against pool-0
    // entities flagged 0x100; build_entity_position_list @0x509660 enumerates the 60xx start family.]
    struct Marker {
        int32_t type;
        Vec3 pos;
        int16_t yaw;
    };
    std::vector<Marker> markers;
    std::vector<Vec3> avoid;
    // The avoid set is every live Organic — which INCLUDES already-spawned players (they are
    // Organic). This is the witnessed spread mechanism (D-NET-115): the original scores markers by
    // nearest distance to any pool-0 entity with Flags & 0x100, and that flagged set contains the
    // players already added, so the second player to spawn scores the first player's marker low and
    // a different marker wins. With multiple start markers, players spread; with exactly ONE marker,
    // every player lands on it — single-marker stacking is FAITHFUL (the original has no further
    // push-apart). [orig: Entity_FindBestSpawnPoint @0x50ccc0]
    world.registry.for_each([&](const Entity &e) {
        if (e.kind == EntityKind::Marker) {
            markers.push_back({e.item_id, e.position, e.yaw});
        } else if (e.kind == EntityKind::Organic && e.alive) {
            avoid.push_back(e.position);
        }
    });

    SpawnPointResult r;
    // Priority scan: the FIRST start-marker type with any marker wins (a mission is authored for one
    // mode, so typically exactly one type is present), then farthest-from-enemy within it. This
    // unifies Server_PositionPlayerForSpawn's per-game-type resolution over the whole family. [§5.2c D-NET-88]
    for (size_t i = 0; i < count; ++i) {
        const int32_t want = types[i];
        const Marker *best = nullptr;
        double best_score = -1.0;
        for (const Marker &m : markers) {
            if (m.type != want) continue;
            // Each candidate's score is its MIN squared 2D distance (mission x/y; z is up) to any
            // avoid entity; choose the MAX. With no enemies every score is +inf and the first is
            // kept (faithful: any start is valid). The rand() tiebreak is deferred. [orig: @0x50ccc0]
            double nearest = std::numeric_limits<double>::infinity();
            for (const Vec3 &a : avoid) {
                const double dx = static_cast<double>(m.pos.x) - static_cast<double>(a.x);
                const double dy = static_cast<double>(m.pos.y) - static_cast<double>(a.y);
                const double d2 = dx * dx + dy * dy;
                if (d2 < nearest) nearest = d2;
            }
            if (best == nullptr || nearest > best_score) {
                best_score = nearest;
                best = &m;
            }
        }
        if (best != nullptr) {
            r.found = true;
            r.position = best->pos;
            r.yaw = best->yaw;
            return r;
        }
    }
    return r; // found=false -> caller falls back, never to an NPC position
}

SpawnPointResult select_player_spawn_for_team(const World &world, uint8_t team,
                                              uint32_t game_type) {
    // Team gametype: the team's own marker types first — primary 6096-6099, fallback
    // 6003/6004/6090/6091 — then the unified family scan as the safety net.
    // [orig: Server_PositionPlayerForSpawn @0x50cf60 @0x50d266/@0x50d320]
    if ((game_type & 0x10000u) != 0 && team >= 1 && team <= 4) {
        const int32_t primary = 6095 + team; // 6096/6097/6098/6099 [orig: @0x50d266]
        const int32_t fallback =             // 6003/6004/6090/6091 [orig: @0x50d320]
                team == 1 ? 6003 : team == 2 ? 6004 : team == 3 ? 6090 : 6091;
        SpawnPointResult r = select_player_spawn(world, &primary, 1);
        if (r.found) return r;
        r = select_player_spawn(world, &fallback, 1);
        if (r.found) return r;
    }
    return select_player_spawn(world);
}

const Entity *resolve_spawn_target(const World &world, uint8_t requester_team,
                                   uint16_t handle) {
    // [orig: Server_ResolveSpawnTargetHandle @0x4fe110]
    const EntityHandle handle_view{handle};
    if (!handle_view.valid()) return nullptr;
    const int pool = handle_view.pool();
    if (pool != 0 && pool != 1 && pool != 2) return nullptr; // [orig: @0x4fe12f]
    EntityHandle h;
    h.packed = handle;
    const Entity *e = world.registry.get(h);
    if (e == nullptr) return nullptr;
    // def attrib 0x40000 "SpawnPoint" [orig: @0x4fe16f].
    if (!e->is_spawn_point) return nullptr;
    // Team gate: match, or the requester is teamless [orig: @0x4fe175..@0x4fe187].
    if (e->team != requester_team && requester_team != 0) return nullptr;
    return e;
}

bool world_has_spawn_zone(const World &world) {
    // [orig: SpawnZoneList_GetCount() > 0 — the join-time bit4 gate @0x51a6f2 and the
    //  0x0F game_flags bit0 @0x502da7. The list registers alive def-attrib-0x40000
    //  entities from pools 2+1 (Entity_BuildSpawnZoneList @0x43EAE0 is the client-side
    //  twin of the same scan).]
    bool any = false;
    world.registry.for_each([&](const Entity &e) {
        if (any) return;
        const int pool = e.handle.pool();
        if (pool != 1 && pool != 2) return;
        if (e.is_spawn_point && e.alive) any = true;
    });
    return any;
}

SpawnZoneRegistry build_spawn_zone_list(const World &world) {
    // [orig: Entity_BuildSpawnZoneList @0x43EAE0]. Collect pool 2 then pool 1 in slot
    // order (the original walks each pool base upward), def attrib 0x40000 only, no
    // alive filter; AABB accumulated as it goes (the original seeds its bounds at 0,
    // so a mission's positive coords always include the origin — reproduced for the
    // deploy-map zoom parity).
    SpawnZoneRegistry reg;
    struct Row {
        EntityHandle handle;
        int32_t key = 0;
    };
    std::vector<Row> rows;
    auto collect_pool = [&](int pool) {
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != pool || !e.is_spawn_point) return;
            Row row;
            row.handle = e.handle;
            // key = typePriority<<16 | (unitType & 0xFF)<<8 | zone# & 0x1F
            // [orig: @0x43ec9b — ItemDef.type 1 (vehicle) -> 2, 32 -> 1, else 0]
            const int type_priority = e.item_type == 1 ? 2 : (e.item_type == 32 ? 1 : 0);
            const ItemDeathTraits *traits = world.item_death_traits.get(e.item_id);
            const uint8_t unit_type =
                    traits ? static_cast<uint8_t>(traits->unit_type) : 0;
            row.key = (type_priority << 16) | (unit_type << 8) | (e.zone_number & 0x1F);
            rows.push_back(row);
            const int32_t x = to_fixed(e.position.x);
            const int32_t y = to_fixed(e.position.y);
            if (x < reg.min_x) reg.min_x = x;
            if (y < reg.min_y) reg.min_y = y;
            if (x > reg.max_x) reg.max_x = x;
            if (y > reg.max_y) reg.max_y = y;
        });
    };
    collect_pool(2);
    collect_pool(1);
    // Ascending stable sort = the original bubble sort's behavior for nonzero keys.
    // The original's BOTH-ZERO-key address tie (@0x43ecc6) is modeled as collect
    // order (see the header note).
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) { return a.key < b.key; });
    reg.entries.reserve(rows.size());
    for (const Row &row : rows) reg.entries.push_back(row.handle);
    return reg;
}

int spawn_zone_index_of(const SpawnZoneRegistry &registry, EntityHandle handle) {
    // [orig: SpawnZoneList_IndexOf @0x43B990 — linear scan, -1 on miss]
    for (size_t i = 0; i < registry.entries.size(); ++i)
        if (registry.entries[i].packed == handle.packed) return static_cast<int>(i);
    return -1;
}

const Entity *find_spawn_zone_for_team(const World &world, const ZoneChain &chain,
                                       uint8_t team, uint32_t game_type) {
    // [orig: find_spawn_entity_for_team @0x4fc810]
    const Entity *found = nullptr;
    if ((game_type & 0x20000u) != 0) {
        // Co-op branch: the LAST team-matching un-numbered spawn entity [orig: @0x4fc834].
        world.registry.for_each([&](const Entity &e) {
            const int pool = e.handle.pool();
            if (pool != 1 && pool != 2) return;
            if (!e.is_spawn_point) return;
            if (e.team == team && e.zone_number == 0) found = &e;
        });
        return found;
    }
    // Team branch: an owned zone that is enemy-capturable (the front line) or carries
    // the team's frontier number, fully secured. [orig: @0x4fc8c3..@0x4fc963 — the
    // walk runs over the zone registry; the frontier number comes from
    // ZoneSlotChain_FindFrontierZone]
    const uint8_t frontier = zone_chain_frontier_zone(world, chain, team);
    const uint8_t enemy = (team == 1) ? 2 : (team == 2) ? 1 : 0;
    if (enemy == 0) return nullptr; // [orig: teams other than 1/2 fall out @0x4fc8fc]
    for (const EntityHandle h : chain.zones) {
        const Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        if (e->team != team || e->zone_number == 0) continue;
        const bool enemy_front = zone_chain_is_capturable(world, chain, enemy, *e);
        const bool at_frontier = frontier != 0 && e->zone_number == frontier;
        if ((enemy_front || at_frontier) && e->zone_control >= 0x10000) return e;
    }
    return nullptr;
}

SpawnPointResult spawn_pose_for_target(const Entity &target) {
    // [orig: Server_PositionPlayerForSpawn @0x50cf60 pick path — pose copy @0x50cfbe,
    //  z += 1.0 when the model has no spawn userpoint @0x50d01c. The userpoint offset
    //  and the 6007 in-zone scatter are tracked §5.61 deferrals.]
    SpawnPointResult r;
    r.found = true;
    r.position = target.position;
    r.position.z += 1.0f;
    r.yaw = target.yaw;
    return r;
}

} // namespace opennova::world
