#include "world/spawn_select.h"

#include <limits>
#include <vector>

#include "world/entity.h" // Entity, EntityKind
#include "world/world.h"  // World, EntityRegistry registry

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
    // unifies CMap_SetupSpawnCamera's per-game-type resolution over the whole family. [§5.2c D-NET-88]
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

} // namespace opennova::world
