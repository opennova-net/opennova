// Retail player spawn selection (net-re §5.2c/§5.61). One operation owns both
// picked deploy targets and the no-pick game-type marker chain; NPC placement
// remains the separate Entity_SpawnFromBMSRecord path.
// [orig: Server_PositionPlayerForSpawn @0x50CF60;
// Entity_FindBestSpawnPoint @0x50CCC0]
//
// Lives in engine/runtime/world (no engine/runtime/mission dependency, like player_spawn.h); scans the world
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

struct SpawnPointResult {
    bool found = false; // a start marker of some family type existed
    Vec3 position{};    // mission space (Z-up), copied from the chosen marker
    int16_t yaw = 0;    // mission yaw (degrees); spawn_player applies the (90 - yaw) heading
    int16_t pitch = 0;
    int16_t roll = 0;
};

// The marker ids admitted to the retail player-start registry. Keep the
// classification private behind a predicate; callers must not infer a spawn
// priority from this unordered family.
// [orig: build_entity_position_list @0x509660]
constexpr bool is_player_spawn_marker_type(int32_t item_id) {
    switch (item_id) {
    case 6001: case 6002: case 6003: case 6004:
    case 6090: case 6091: case 6094: case 6095:
    case 6096: case 6097: case 6098: case 6099:
        return true;
    default:
        return false;
    }
}

// Resolve one complete spawn pose. A valid target selects the picked-zone path
// (including numbered-zone 6007 scatter). Without one, the retail mode chain is
// exact: 6095→6002 solo; 6096..6099→6003/6004/6090/6091 team; and
// 6094→6001→numbered entity Co-op. `player_slot` is Co-op's direct-marker
// rotation input; `spawning_player` is excluded from the Flags&0x100 avoidance
// set used by non-Co-op marker scoring. The mission-global cycle advances at
// the same non-Co-op/scatter sites as retail.
// [orig: Server_PositionPlayerForSpawn @0x50CF60;
// Entity_FindBestSpawnPoint @0x50CCC0; CRenderState_GetFieldByIndex
// @0x52D7D0 field 6]
SpawnPointResult resolve_player_spawn_pose(
    World &world, EntityHandle spawning_player, EntityHandle target,
    uint8_t player_slot, uint8_t team, uint32_t game_type);

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
// @0x502da7; the client builds its own picker list from the exact S2C 0x10/0x0D
// pool-2 + pool-1 rows, with the same def gate — Entity_BuildSpawnZoneList @0x43EAE0].
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

// One retail spawn-wave group. The original stores eight player pointers,
// queued_count, the zone pointer, interval/countdown/pre-delay, and a cached
// team in one 56-byte row. Handles make the same ownership explicit without
// leaking allocator addresses into the portable world model.
// [orig: g_spawn_wave_list @0x24E0E48; SpawnWaveList_AppendEntry @0x52AB60]
struct SpawnWaveEntry {
    EntityHandle zone;
    uint8_t team = 0;
    int32_t interval = 0;
    int32_t countdown = 0;
    int32_t pre_delay = 0;
    std::vector<EntityHandle> queued;

    // Countdown shown to this requester. Members see their position in the
    // queue; a nonmember sees the tail ETA.
    // [orig: SpawnWaveList_GetEntryInfo @0x52A700]
    uint16_t requester_countdown(EntityHandle requester) const;
};

struct SpawnWaveRelease {
    EntityHandle player;
    EntityHandle zone;
};

// Spawn selection and its timed release list are one domain module: the host
// asks this object whether a valid deploy pick queues, and consumes releases
// from its 1 Hz tick. It has no transport dependency; S2C 0x6E is a projection
// of entries(). [orig: SpawnWaveList_* @0x52A330..0x52AB60]
class SpawnWaveList {
public:
    void clear() { entries_.clear(); }
    void build_from_mission(const World &world, int32_t base_interval,
                            int32_t numbered_zone_interval);
    bool has_entry(EntityHandle zone) const;
    bool try_queue(const World &world, EntityHandle zone, EntityHandle player);
    bool remove_player(EntityHandle player);
    std::vector<SpawnWaveRelease> tick(const World &world);
    void reset_on_zone_team_change(const World &world, EntityHandle zone);

    const std::vector<SpawnWaveEntry> &entries() const { return entries_; }

private:
    std::vector<SpawnWaveEntry> entries_;
};

// The C2S 0x2C deploy-pick sentinels [orig: Input_HandleActionBinding case 12
// @0x49b0c5-0x49b17b - param 0 -> 0xFFFF (no pick), 65534 -> 0xFFFE (the
// auto-team zone pick); host decode Server_ProcessClientRequestRespawn
// @0x519AF0, net-re paragraph 5.61].
inline constexpr uint16_t kDeployPickNone = 0xFFFF;
inline constexpr uint16_t kDeployPickAutoTeam = 0xFFFE;

// The 0xFFFE auto-deploy pick: the requester team's own zone that sits ON the
// frontier — enemy-capturable, or carrying the team's frontier number — with
// control fully secured (>= 0x10000). Co-op gametypes (game_type & 0x20000) take
// the last team-matching UN-numbered spawn entity instead. nullptr = no zone spawn
// (the caller falls back to the marker chain). [orig: find_spawn_entity_for_team
// @0x4fc810]
const Entity *find_spawn_zone_for_team(const World &world, const ZoneChain &chain,
                                       uint8_t team, uint32_t game_type_value);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_SPAWN_SELECT_H
