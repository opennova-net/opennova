// Entity registry: fixed-capacity pools + the addressing surface both scripting
// systems use (by net id / group / team / area / name), plus named non-entity
// addressables (areas, routes/wplists, groups).
#pragma once

#include <array>
#include <cstdint>
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
};

struct LocationVolume {
    Aabb bounds;
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
    // [orig: g_pool_list @0xA892E0 — pools 0..3 are "actor" pools searched by
    // EntityPool_FindByNetId (mask &0xF); pool 4 holds static props.]
    static constexpr int kPoolCount = 5;
    static constexpr int kActorPoolMask = 0xF; // pools 0..3

    void configure_pool(int pool, size_t capacity);

    EntityHandle spawn(int pool, const Entity &seed); // kInvalid if pool full
    EntityHandle spawn_from(int pool, size_t first_slot, const Entity &seed); // first free >= first_slot
    // Allocate exactly the packed pool/slot identity supplied by the wire.
    // Unlike spawn_from, this never falls through to a later free slot. Retail
    // load handlers select Pool_GetEntryUnchecked(pool, slot) and memset that
    // row in place (pool 1 @0x432C40, pool 2 @0x433400, pool 3 @0x425C00).
    EntityHandle spawn_at(EntityHandle h, const Entity &seed);
    void despawn(EntityHandle h);
    bool despawn(EntityLifetime lifetime);
    Entity *get(EntityHandle h);
    const Entity *get(EntityHandle h) const;
    Entity *get(EntityLifetime lifetime);
    const Entity *get(EntityLifetime lifetime) const;

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
    int register_area(std::string name, const Aabb &bounds, bool active = true,
                      int32_t zone_id = -1, std::optional<Aabb> script_bounds = {});
    // The load-time id -> index resolve [orig: the @0x453000/@0x453100 scan over
    // record[0]]; -1 when no record carries the id.
    int area_index_by_zone_id(int32_t zone_id) const;
    const Area *area(int id) const;
    void clear_script_tables();
    void register_location(const Aabb &bounds, int32_t id);
    int32_t location_at(const Vec3 &position) const;

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
    // the original validates wire handles against [orig: g_pool_list[pool].capacity reads,
    // e.g. NapiNPServerMsg_HandlePlayerInfoRequest @0x514180].
    size_t pool_capacity(int pool) const {
        return (pool >= 0 && pool < kPoolCount) ? pools_[pool].slots.size() : 0;
    }

    template <class F>
    void for_each(F &&fn) const {
        for (const Pool &p : pools_) {
            for (size_t s = 0; s < p.slots.size(); ++s) {
                if (p.used[s]) fn(p.slots[s]);
            }
        }
    }

    // One pool's used slots in slot order — the retail per-pool array walk
    // (g_pool_list[pool].used entries) the per-tick systems take instead of
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
        size_t live = 0;
    };
    std::array<Pool, kPoolCount> pools_{};
    uint64_t next_spawn_id_ = 1; // zero means "identity not recorded"
    std::vector<Area> areas_;
    std::vector<LocationVolume> locations_;
    // [orig: GameMode_CreateDefaultDefs @0x4F9060]
    std::vector<std::string> group_names_ = {
        "emptygroup", "humans", "blueplayers", "redplayers", "ai", "blueai", "redai"
    };
    std::vector<std::vector<EntityHandle>> group_members_{7};
};

} // namespace opennova::world
