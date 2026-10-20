// Entity registry: fixed-capacity pools + the addressing surface both scripting
// systems use (by net id / group / team / area / name), plus named non-entity
// addressables (areas, routes/wplists, groups).
#pragma once

#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <runtime/world/entity.h>
#include <runtime/world/geom.h>

namespace opennova::world {

struct Area {
    std::string name;
    Aabb bounds;
    // The area trigger's Active flag (bms flags bit0). The player-AWOL probe
    // counts only active zones [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40
    // walks records with flags bit0 set].
    bool active = true;
    // The authored zone id (record dword @0). Trigger/action zone refs carry
    // THIS id in the file and are rewritten to the array index at load
    // [orig: the load-time resolvers @0x453000/@0x453100 match record[0]].
    int32_t zone_id = -1;
    Aabb script_bounds; // raw authored bounds; WAC area3D ignores constrain-Z
    // The record's box is flat on x or y, compared as its 16.16 words
    // (mission::zone_box_flat): no zone parameter resolves to it [orig: the
    // load-time resolvers' box tests @0x453093/@0x45317e].
    bool flat = false;
    // The record's own X/Y words as authored (x_min, x_max, y_min, y_max, not
    // normalized), which the player-AWOL probe compares the position words
    // against [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40 - @0x439D69..0x439D85].
    std::array<int32_t, 4> record_xy_q16{};
    // The trigger tests' Z words: the record's z_min / z_max when it constrains
    // Z, else 0xC0000000 / 0x40000000 [orig: Entity_IsBmsRefInTriggerBounds
    // @0x43E510 - `test byte [eax+1Ch], 2` @0x43E531, the defaults
    // @0x43E553..0x43E55B]; and the record's raw Z words the WAC 3D test reads
    // [orig: WacCmd_SsnArea3D @0x4F0F60 - @0x4F1000..0x4F100E].
    std::array<int32_t, 2> trigger_z_q16{};
    std::array<int32_t, 2> record_z_q16{};

    // The position words against the record's X/Y words, signed inclusive
    // (D-NET-507). [orig: WacCmd_SsnArea @0x4F1020 - @0x4F10A6..0x4F10BD]
    bool xy_contains(const EntityPosition &p) const {
        return p.x.q16 >= record_xy_q16[0] && p.x.q16 <= record_xy_q16[1] &&
               p.y.q16 >= record_xy_q16[2] && p.y.q16 <= record_xy_q16[3];
    }
    // The trigger tests' box: the X/Y words and the trigger Z words, signed
    // inclusive. [orig: Entity_IsBmsRefInTriggerBounds @0x43E510 - the compares
    //  @0x43E596..0x43E5C1; EventTrigger_AnySatchelInArea @0x547160 -
    //  @0x5471F3..0x547215]
    bool trigger_contains(const EntityPosition &p) const {
        return xy_contains(p) && p.z.q16 >= trigger_z_q16[0] && p.z.q16 <= trigger_z_q16[1];
    }
};

// The seven default script groups, in their fixed slots 0..6: the names the
// mode's default definitions create [orig: GameMode_CreateDefaultDefs @0x4F9060,
// the strcpys @0x4F9087..0x4F91DE]; their members are rebuilt from pool 0
// (EntityRegistry::script_groups) [orig: Server_BuildEntitySlotLists @0x4F97A0].
// A mission's own WAC groups intern after them.
inline constexpr int kDefaultScriptGroupCount = 7;
inline constexpr const char *kDefaultScriptGroupNames[kDefaultScriptGroupCount] = {
    "emptygroup", "humans", "blueplayers", "redplayers", "ai", "blueai", "redai"
};

// A Location box (BMS bounding box type 5), each axis normalized to min / max
// words [orig: Mission_LoadBMSFile @0x40FCC3].
struct LocationVolume {
    int32_t min_q16[3] = {0, 0, 0};
    int32_t max_q16[3] = {0, 0, 0};
    int32_t id = 0;
};

// Stable identity for one allocation lifetime of a packed pool/slot handle.
// Handles are intentionally reused; the registry spawn serial prevents an old
// owner from reading or despawning a later entity that occupies the same slot.
struct EntityLifetime {
    EntityHandle handle{};
    uint64_t registry_spawn_id = 0;

    bool valid() const { return handle.valid() && registry_spawn_id != 0; }
};

class EntityRegistry {
public:
    // [orig: g_PoolList @0xA892E0 — pools 0..3 are "actor" pools searched by
    // EntityPool_FindByNetId (mask &0xF); pool 4 holds static props.]
    static constexpr int kPoolCount = 5;
    static constexpr int kActorPoolMask = 0xF; // pools 0..3

    void configure_pool(int pool, size_t capacity);

    // Pool_AllocEntry's includeInUse argument: 0 passes over a destroyed row (D-NET-485).
    enum class Reuse : uint8_t { Destroyed, Fresh };
    EntityHandle spawn(int pool, const Entity &seed, Reuse reuse = Reuse::Destroyed); // kInvalid if pool full
    EntityHandle spawn_from(int pool, size_t first_slot, const Entity &seed,
                            Reuse reuse = Reuse::Destroyed); // first free >= first_slot
    // Allocate exactly the packed pool/slot identity supplied by the wire.
    // Unlike spawn_from, this never falls through to a later free slot. Retail
    // load handlers select Pool_GetEntryUnchecked(pool, slot) and memset that
    // row in place (pool 1 @0x432C40, pool 2 @0x433400, pool 3 @0x425C00).
    EntityHandle spawn_at(EntityHandle h, const Entity &seed);
    void despawn(EntityHandle h);
    bool despawn(EntityLifetime lifetime);
    // Clear a free row's +0x219 (destroyed) byte, so an includeInUse-0 caller
    // may take it again (D-NET-485). A live row is left alone.
    // [orig: HeliLift_UpdateSlotState @0x451730 - `mov [reg+219h], bl` after
    //  each clone's Entity_Destroy]
    void clear_destroyed(EntityHandle h);
    Entity *get(EntityHandle h);
    const Entity *get(EntityHandle h) const;
    Entity *get(EntityLifetime lifetime);
    const Entity *get(EntityLifetime lifetime) const;
    // Row h's storage, live or not (null past the pool's capacity): the entity
    // an AI slot belongs to. Stable until configure_pool or restore_from
    // replaces the pool.
    Entity *row_storage(EntityHandle h);

    // Restore authored/runtime-visible registry state without rewinding the
    // host-only lifetime serial. Collision and skeletal caches use that serial
    // to distinguish entities that reuse the same packed pool/slot handle.
    void restore_from(const EntityRegistry &snapshot);

    // Faithful to EntityPool_FindByNetId @0x4f0a20: scans pool 0 first, then pools
    // 1..3 (mask &0xF), first match wins; returns (pool<<12)|slot, else invalid.
    EntityHandle find_by_net_id(uint16_t net_id) const;
    // The row itself, or null: the net_id scan above, and the file id
    // (bms::Entity::id) the mission drives key placed entities by.
    Entity *by_net_id(uint16_t net_id);
    Entity *by_bms_id(int32_t bms_id);

    void by_group(uint8_t group, std::vector<EntityHandle> &out) const;

    // Named, first-class non-entity addressables.
    // Returns the area INDEX (the id space zone-resolved refs use). zone_id is the
    // authored record id [orig: zone record dword @0].
    // `record_xy_q16`: the record's X/Y words; absent, the bounds' words.
    // `trigger_z_q16` / `record_z_q16`: the trigger Z words and the raw record Z
    // words; absent, the bounds' / script bounds' words.
    int register_area(std::string name, const Aabb &bounds, bool active = true,
                      int32_t zone_id = -1, std::optional<Aabb> script_bounds = {},
                      bool flat = false,
                      std::optional<std::array<int32_t, 4>> record_xy_q16 = {},
                      std::optional<std::array<int32_t, 2>> trigger_z_q16 = {},
                      std::optional<std::array<int32_t, 2>> record_z_q16 = {});
    // The load-time id -> index resolve [orig: the @0x453000/@0x453100 scan over
    // record[0]]; -1 when no record carries the id.
    int area_index_by_zone_id(int32_t zone_id) const;
    const Area *area(int id) const;
    void clear_script_tables();
    void register_location(const LocationVolume &location);
    int32_t location_at(const EntityPosition &position) const;

    int intern_group(std::string_view name); // WAC named group, separate from BMS command groups
    static int default_script_group_index(std::string_view name);
    int script_group_index(std::string_view name) const;
    void set_script_group_members(int group, const std::vector<EntityHandle> &members);
    void script_groups(std::vector<std::vector<EntityHandle>> &out) const;
    // The WAC 'humans' count the same walk rebuilds: every live pool-0 row
    // with the Player bit that is not hidden (a player waiting to deploy is).
    // Retail's item-def test is its allocated-row test, so a player whose type
    // has no items.def row counts. [orig: Server_BuildEntitySlotLists @0x4f97a0]
    int32_t count_humans() const;

    size_t live_count() const;
    // Monotonic spawn serial: differs whenever any entity has spawned since a
    // caller last sampled it (the cheap "rebuild your handle index" edge).
    uint64_t spawn_serial() const { return next_spawn_id_; }

    // Configured slot capacity of `pool` (0 for an unconfigured/invalid pool) — the bound
    // the original validates wire handles against [orig: g_PoolList[pool].capacity reads,
    // e.g. NapiNPServerMsg_HandlePlayerInfoRequest @0x514180].
    size_t pool_capacity(int pool) const {
        return (pool >= 0 && pool < kPoolCount) ? pools_[pool].slots.size() : 0;
    }

    // The pool's used count, its allocation high-water mark: the bound the
    // original's pool walks stop at, freed rows below it included [orig:
    // Pool_GetUsedCount @0x441F80]. configure_pool clears it [orig: Pool_Clear
    // @0x442060] and the mission load sets it per pool [orig: Pool_SetUsed
    // @0x441F60]. A spawn raises it past its slot. spawn / spawn_from stand for
    // an includeInUse-1 allocation, which takes the lowest free row (its
    // ItemTypeIndex 0) below the count, else the row at the count, raising it
    // [orig: Pool_AllocEntry @0x442230 -- the +0x1C test @0x44225C, the raise
    // @0x44228B; the player body's push 1 @0x43C3DF, the class callbacks'
    // clones (sections, dismemberment) in Entity_CloneFromTemplateByType
    // @0x4398BD / @0x439939 / @0x439962, the deployable's
    // Entity_CloneFromTemplateToPool1 @0x439840].
    // An includeInUse-0 caller (Reuse::Fresh) also passes over a destroyed row
    // (+0x219, which Entity_Destroy sets and the next occupant's write clears)
    // and grows the count (D-NET-485). spawn_at stands for a
    // slotted load [orig: Pool_UpdateMaxUsed @0x441F40,
    // Pool_UpdateMaxUsedFromSlotId @0x442100]. A despawn never lowers it.
    size_t pool_used(int pool) const {
        return (pool >= 0 && pool < kPoolCount) ? pools_[pool].used_count : 0;
    }
    void set_pool_used(int pool, size_t count);

    template <class F>
    void for_each(F &&fn) const {
        for (const Pool &p : pools_) {
            for (size_t s = 0; s < p.slots.size(); ++s) {
                if (p.used[s]) fn(p.slots[s]);
            }
        }
    }

    // One pool's used slots in slot order — the retail per-pool array walk
    // (g_PoolList[pool].used entries) the per-tick systems take instead of
    // the whole registry [orig: Entity_UpdateAllEntities @0x4c2100 walks pool
    // 1 @0x4c2140.., pool 0 @0x4c244c..; Entity_BuildProximityLists_Pool01
    // @0x4b9340 walks pools 1 then 0].
    template <class F>
    void for_each_in_pool(int pool, F &&fn) const {
        if (pool < 0 || pool >= kPoolCount) return;
        const Pool &p = pools_[pool];
        for (size_t s = 0; s < p.slots.size(); ++s) {
            if (p.used[s]) fn(p.slots[s]);
        }
    }

private:
    struct Pool {
        std::vector<Entity> slots;
        std::vector<char> used; // char (not bool) for stable addressing
        // entity+0x219: set by a despawn [orig: Entity_Destroy - `mov byte ptr
        // [esi+219h], 1` @0x43EA7F], cleared by the next occupant's spawn.
        std::vector<char> destroyed;
        size_t live = 0;
        size_t used_count = 0; // g_PoolList[pool].used, the high-water mark (pool_used)
    };
    std::array<Pool, kPoolCount> pools_{};
    uint64_t next_spawn_id_ = 1; // zero means "identity not recorded"
    std::vector<Area> areas_;
    std::vector<LocationVolume> locations_;
    std::vector<std::string> group_names_{std::begin(kDefaultScriptGroupNames),
                                          std::end(kDefaultScriptGroupNames)};
    std::vector<std::vector<EntityHandle>> group_members_{kDefaultScriptGroupCount};
};

} // namespace opennova::world
