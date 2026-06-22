// Faithful-in-outcome player spawn-point selection (net-re §5.2c). The original picks the human
// player's start pose from the mission's named START markers, NOT from any NPC's position. The
// exact selection is per-game-type and fragmented across several functions [orig:
// CMap_SetupSpawnCamera @0x50cf60 → Entity_FindBestSpawnPoint @0x50ccc0; the 60xx start family is
// enumerated by build_entity_position_list @0x509660; a real SP mission (00TRa) can ship only a
// 6001 marker that the strict 6002 SP path never reaches]. We UNIFY that machinery to a priority
// scan over the start-marker family: the first present type wins, farthest-from-enemy within it —
// a tracked simplification (§5.2c, D-NET-88) that finds the authored start for any mission mode.
// NPCs take their authored BMS positions on a separate path [orig: Entity_SpawnFromBMSRecord
// @0x40e9f0] — so the player never inherits an NPC's spot.
//
// Lives in libs/world (no libs/mission dependency, like player_spawn.h); scans the world
// registry's promoted markers by item_id (== the raw BMS type_id, make_seed promote.cpp:78),
// equivalent to the original's items.def-index match (ItemList_FindIndexByTypeId is injective).
#ifndef OPENNOVA_WORLD_SPAWN_SELECT_H
#define OPENNOVA_WORLD_SPAWN_SELECT_H

#include <cstddef>
#include <cstdint>

#include "world/geom.h" // Vec3

namespace opennova::world {

class World;

// The 60xx player-start marker family, in selection priority (SP/DM, then coop, then team). A
// mission is authored for one mode, so typically exactly one of these is present.
// [orig: build_entity_position_list @0x509660 enumerates 6001/6002/6003/6004/6090/6091/6094-6099;
//  CMap_SetupSpawnCamera @0x50cf60 — 6002 SP/DM, 6095 non-team, 6094 coop insertion, 6001 coop
//  fallback (+ the dedicated 6001 reader @0x41f25e), 6096-6099 team, 6003/6004/6090/6091 TDM teams.]
inline constexpr int32_t kSpawnMarkerStartTypes[] = {
    6002, 6095, 6094, 6001, 6096, 6097, 6098, 6099, 6003, 6004, 6090, 6091,
};
inline constexpr size_t kSpawnMarkerStartTypeCount =
    sizeof(kSpawnMarkerStartTypes) / sizeof(kSpawnMarkerStartTypes[0]);

struct SpawnPointResult {
    bool found = false; // a start marker of some family type existed
    Vec3 position{};    // mission space (Z-up), copied from the chosen marker
    int16_t yaw = 0;    // mission yaw (degrees); spawn_player applies the (90 - yaw) heading
};

// Select the player-start: scan `types` (priority order); the FIRST type with any promoted marker
// (EntityKind::Marker, item_id == type) wins, returning the one FARTHEST (mission 2D) from any live
// enemy organic. [orig: Entity_FindBestSpawnPoint @0x50ccc0 — min 2D distance to a pool-0 "avoid"
// entity (flags & 0x100), pick the max.] The avoid set is approximated by live organic soldiers (a
// tracked divergence; the faithful flags&0x100 set is unmodeled — §5.2c). At select time the player
// has not spawned, so every organic is an NPC. Returns found=false when no family marker exists, so
// the caller can pick a safe fallback — never an NPC position.
SpawnPointResult select_player_spawn(const World &world, const int32_t *types, size_t count);

// Convenience overload: scan the default start-marker family in priority order.
inline SpawnPointResult select_player_spawn(const World &world) {
    return select_player_spawn(world, kSpawnMarkerStartTypes, kSpawnMarkerStartTypeCount);
}

} // namespace opennova::world

#endif // OPENNOVA_WORLD_SPAWN_SELECT_H
