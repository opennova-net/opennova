// Faithful-in-outcome player spawn-point selection (net-re §5.2c). The original picks the human
// player's start pose from the mission's named START markers, NOT from any NPC's position. The
// exact selection is per-game-type and fragmented across several functions [orig:
// Server_PositionPlayerForSpawn @0x50cf60 → Entity_FindBestSpawnPoint @0x50ccc0; the 60xx start family is
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
#include <vector>

#include "world/entity.h" // Entity, EntityHandle
#include "world/geom.h"   // Vec3

namespace opennova::world {

class World;

// The 60xx player-start marker family, in selection priority (SP/DM, then coop, then team). A
// mission is authored for one mode, so typically exactly one of these is present.
// [orig: build_entity_position_list @0x509660 enumerates 6001/6002/6003/6004/6090/6091/6094-6099;
//  Server_PositionPlayerForSpawn @0x50cf60 — 6002 SP/DM, 6095 non-team, 6094 coop insertion, 6001 coop
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

// Team-mode start selection (net-re §5.61, refining §5.2c): a team gametype
// (game_type & 0x10000) resolves the TEAM's marker types — primary 6096-6099[team],
// fallback 6003/6004/6090/6091[team] — before the unified family scan. Without the
// per-team split, every AS team spawns at the FIRST family type present (both teams
// in team 1's base). [orig: Server_PositionPlayerForSpawn @0x50cf60 team switch
// @0x50d266 (6096-6099) / @0x50d320 (6003/6004/6090/6091)]
SpawnPointResult select_player_spawn_for_team(const World &world, uint8_t team,
                                              uint32_t game_type);

struct ZoneChain; // world/zone_chain.h

// Resolve a C2S 0x0E deploy pick to its target entity. Pools 0/1/2 only (pool 0 =
// mobile spawn vehicles), the ItemDef must carry attrib 0x40000 "SpawnPoint"
// (Entity::is_spawn_point), and the target's team must match the requester's — a
// TEAMLESS requester may pick anything. nullptr = invalid pick.
// [orig: Server_ResolveSpawnTargetHandle @0x4fe110]
const Entity *resolve_spawn_target(const World &world, uint8_t requester_team,
                                   uint16_t handle);

// The world offers at least one deploy-selectable spawn zone (an alive attrib-0x40000
// "SpawnPoint" entity). Gates the join-time respawn-pending flag — the deploy screen only
// holds when the mission has zones to pick [orig: Server_OnPlayerJoin @0x51a6f2
// `|= 0x10 iff SpawnZoneList_GetCount() > 0`; same count gates the 0x0F game_flags bit0
// @0x502da7; the client builds its own picker list from local BMS, pools 2+1, the same
// def gate — Entity_BuildSpawnZoneList @0x43EAE0].
bool world_has_spawn_zone(const World &world);

// The deploy/spawn-zone REGISTRY — the sorted zone list whose INDICES are the
// deploy-screen letters ('A' + index), the S2C 0x6E zoneIdx, the 0x1E zone-event
// attacker bytes, and the space the deploy screen's pick parameter (index + 1)
// resolves through. Collect order: every pool-2 then pool-1 entity whose ItemDef
// carries attrib 0x40000 "SpawnPoint" (no alive filter — the original registers
// dead zones too), accumulating the deploy-map AABB from the entity x/y as it
// goes; then bubble-sort ascending by the composite key
//   ((type==1 ? 2 : type==32 ? 1 : 0) << 16) | ((unitType & 0xFF) << 8) | (zone# & 0x1F)
// so ground zones lead and vehicles trail. Equal nonzero keys keep collect
// order (the original's bubble sort is stable there); BOTH-ZERO keys tie-break
// by entity ADDRESS in the original (allocation order across pools) — modeled
// here as collect order, a documented approximation that only reorders
// zero-key zones split across pools.
// [orig: Entity_BuildSpawnZoneList @0x43EAE0 (collect @0x43eb2e/@0x43ebad, AABB
//  @0x43eb59.., sort keys @0x43ec9b/@0x43ecb6, zero-key address tie @0x43ecc6);
//  SpawnZoneList_IndexOf @0x43B990]
struct SpawnZoneRegistry {
    std::vector<EntityHandle> entries;
    // Deploy-map AABB over the registered zones (i32 16.16 world x/y). True
    // min/max names — the original's g_WorldBoundsMax/Min globals hold these
    // SWAPPED (the "Max" global accumulates the minimum; IDB misnomer).
    int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool empty() const { return entries.empty(); }
};
SpawnZoneRegistry build_spawn_zone_list(const World &world);
// Registry index of a zone entity, -1 when absent [orig: SpawnZoneList_IndexOf @0x43B990].
int spawn_zone_index_of(const SpawnZoneRegistry &registry, EntityHandle handle);

// The 0xFFFE auto-deploy pick: the requester team's own zone that sits ON the
// frontier — enemy-capturable, or carrying the team's frontier number — with
// control fully secured (>= 0x10000). Co-op gametypes (game_type & 0x20000) take
// the last team-matching UN-numbered spawn entity instead. nullptr = no zone spawn
// (the caller falls back to the marker chain). [orig: find_spawn_entity_for_team
// @0x4fc810]
const Entity *find_spawn_zone_for_team(const World &world, const ZoneChain &chain,
                                       uint8_t team, uint32_t game_type);

// Deploy pose at a picked spawn target: the target's position with z + 1.0 and its
// yaw. Deferrals (net-re §5.61 follow-ups): the model-userpoint offset (name string
// is runtime-set in the original) and the numbered-zone round-robin over in-radius
// pool-3 type-6007 sub-spawn markers (the zone radius source entity+350 is
// unwitnessed; ASH_I5A authors no 6007 markers, so the fallback IS the retail
// behavior there). [orig: Server_PositionPlayerForSpawn @0x50cf60 pick path
// @0x50cfbe (pose copy) / @0x50d01c (z += 0x10000 when no userpoint)]
SpawnPointResult spawn_pose_for_target(const Entity &target);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_SPAWN_SELECT_H
