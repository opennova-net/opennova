// Retail player spawn selection (net-re §5.2c/§5.61). One operation owns both
// picked deploy targets and the no-pick game-type marker chain; NPC placement
// remains the separate Entity_SpawnFromBMSRecord path.
// [orig: Server_PositionPlayerForSpawn @0x50CF60;
// Entity_FindBestSpawnPoint @0x50CCC0]
//
// Lives in engine/runtime/world (no engine/runtime/mission dependency, like player_spawn.h); scans the world
// registry's promoted markers by item_id (== the raw BMS type_id, make_seed promote.cpp:78),
// equivalent to the original's items.def-index match (ItemList_FindIndexByTypeId is injective).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <runtime/world/entity.h> // Entity, EntityHandle
#include <runtime/world/geom.h>   // Vec3

namespace opennova::world {

class World;

struct SpawnPointResult {
    bool found = false; // a start marker of some family type existed
    Vec3 position{};    // mission space (Z-up), copied from the chosen marker
    int16_t yaw = 0;    // the whole-degree mission yaw mirror (Entity::yaw)
    int16_t pitch = 0;
    int16_t roll = 0;
    // The heading word (+0x10) the placement copies onto the player verbatim:
    // the chosen entity's own Yaw (a start marker's placement angle, low 16
    // bits zero; a vehicle's live motor heading), plus its parent's Yaw for a
    // parented marker. The motor heading and the local look yaw start from it
    // (D-NET-376). [orig: Entity_FindBestSpawnPoint @0x50CED7..0x50CF38,
    //  look yaw @0x50CF4D; Server_PositionPlayerForSpawn @0x50CFD6 /
    //  @0x50D16E / @0x50D3F7 / @0x50D51B; Entity_TransformLocalToWorld
    //  @0x43BE7E]
    int32_t heading_bam = 0;
    // The Co-op direct-marker arm's two survivor latches. The chosen 6094/6001
    // marker's parachute bit (0x20) is copied onto the player, and a marker
    // whose team byte is 2 additionally arms the queued mount (0x200) with
    // `carrier` = the marker's parent when it has one, else the MARKER itself,
    // destined for entity+0x16C/+0x180. The body update's 0x200 toggle performs
    // the actual seat attach; nothing here flips the mounted state. Zero
    // outside that arm (the scored non-Co-op markers and the picked-zone path
    // carry neither). [orig: Server_PositionPlayerForSpawn @0x50D424..0x50D45A]
    uint32_t flags_or = 0;
    EntityHandle carrier;
};

// The player-slot bytes the no-pick arm reads beside the assigned team: the
// spectator latch (slot+100567, the addEvent+108 byte Server_PlayerAdd stores
// @0x51CD83) and the restore team (slot+100568). A latched slot is POSITIONED
// with a substitute team while its assigned team stays 0.
// [orig: Server_PositionPlayerForSpawn @0x50D17C..0x50D1C6]
struct SpawnSlotState {
    bool spectator = false;
    uint8_t restore_team = 0;
};

// Install the resolved latches on the positioned player: OR the flags, and
// when a carrier was armed write it to mount_target (+0x16C) and
// mount_toggle_fallback (+0x180) exactly as the marker arm does. The pose
// itself is the caller's copy (join builds a PlayerSpawn, deploy/boot write the
// entity directly). [orig: Server_PositionPlayerForSpawn @0x50D42A, @0x50D44D..0x50D45A]
void apply_spawn_point_latches(Entity &player, const SpawnPointResult &sel);


// The start markers the no-pick arm reads for a player positioned as `team` under `game_type`, by
// their items.def type (the raw BMS type_id): the primary, served while the team has no death, and
// the fallback after it. The waypoint family (Co-op) reads 6094 then 6001, the team modes 6095 + team
// then 6003/6004/6090/6091, every other mode 6095 then 6002; a team mode's team outside 1..4 reads
// none (0). What the editor's Play from here moves in a staged mission (editor/run/play_start.h).
// [orig: Server_PositionPlayerForSpawn @0x50CF60]
struct StartMarkerTypes {
    int32_t primary = 0;
    int32_t fallback = 0;
};
StartMarkerTypes start_marker_types(uint32_t game_type, uint8_t team);

// Resolve one complete spawn pose. A valid target selects the picked-zone path
// (including numbered-zone 6007 scatter). Without one, the retail mode chain is
// exact: 6095→6002 solo; 6096..6099→6003/6004/6090/6091 team; and
// 6094→6001→numbered entity Co-op. `player_slot` is Co-op's direct-marker
// rotation input; `spawning_player` is excluded from the Flags&0x100 avoidance
// set used by non-Co-op marker scoring and supplies the dead bit the spectator
// team substitution reads. `slot` carries the spectator latch/restore team; a
// latched slot positions as team 1 in Co-op, in team modes as the nonzero
// restore team of a dead-flagged entity or else 2 - (logic_tick & 1), and
// unchanged elsewhere. The mission-global cycle advances at the same
// non-Co-op/scatter sites as retail.
// [orig: Server_PositionPlayerForSpawn @0x50CF60 (spectator arm @0x50D17C..0x50D1C6);
// Entity_FindBestSpawnPoint @0x50CCC0; CPlayerStats_GetFieldPlusOne (ex CRenderState_GetFieldByIndex)
// @0x52D7D0 field 6]
SpawnPointResult resolve_player_spawn_pose(
    World &world, EntityHandle spawning_player, EntityHandle target,
    uint8_t player_slot, uint8_t team, uint32_t game_type,
    const SpawnSlotState &slot = {});

struct ZoneChain; // world/zone_chain.h



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
// by entity ADDRESS. Retail's single contiguous pool allocation fixes pool 1
// at +232420 (stride 1360) and pool 2 at +1865416 (stride 812), so this order
// is reproduced without depending on the reimplementation allocator.
// [orig: EntityPool_Allocate @0x442130; Entity_BuildSpawnZoneList @0x43EAE0
//  (collect @0x43eb2e/@0x43ebad, AABB @0x43eb59.., sort keys
//  @0x43ec9b/@0x43ecb6, zero-key address tie @0x43ecc6);
//  SpawnZoneList_IndexOf @0x43B990]
struct SpawnZoneRegistry {
    std::vector<EntityHandle> entries;
    // Deploy-map AABB over the registered zones (i32 16.16 world x/y). True
    // min/max names — the original's g_WorldBoundsMax/Min globals hold these
    // SWAPPED (the "Max" global accumulates the minimum; IDB misnomer).
    int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool empty() const { return entries.empty(); }
};
// Registry index of a zone entity, -1 when absent [orig: SpawnZoneList_IndexOf @0x43B990].
int spawn_zone_index_of(const SpawnZoneRegistry &registry, EntityHandle handle);


// One retail spawn-wave group. The original stores eight player pointers,
// queued_count, the zone pointer, interval/countdown, a second timer word at
// +48 and a cached team in one 56-byte row. Handles make the same ownership
// explicit without leaking allocator addresses into the portable world model.
// The +48 word is not carried: every store to it is zero (the mission build
// @0x52A9BB/@0x52AA85, the control-loss flush @0x52A372, the team-flip reset
// @0x52A5E1), so its tick decrement @0x52A339 never runs and its two ETA
// reads (@0x52A2FF, @0x52A66E) add nothing.
// [orig: g_SpawnWaveList @0x24E0E48; SpawnWaveList_AppendEntry @0x52AB60;
// SpawnWaveList_TickEntry @0x52A330]
struct SpawnWaveEntry {
    EntityHandle zone;
    uint8_t team = 0;
    int32_t interval = 0;
    int32_t countdown = 0;
    std::vector<EntityHandle> queued;

    // Countdown shown to this requester. Members see the countdown plus their
    // position in the queue times the interval; a nonmember sees the tail ETA
    // (countdown plus the whole queue).
    // [orig: SpawnWaveList_GetEntryInfo @0x52A700 -> SpawnWaveEntry_MemberEta
    // @0x52A2E0 / SpawnWaveEntry_TailEta @0x52A610]
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


} // namespace opennova::world
