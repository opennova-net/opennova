// Entity registry: fixed-capacity pools + the addressing surface both scripting
// systems use (by net id / group / team / area / name), plus named non-entity
// addressables (areas, routes/wplists, groups).
#ifndef OPENNOVA_WORLD_ENTITY_REGISTRY_H
#define OPENNOVA_WORLD_ENTITY_REGISTRY_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "world/entity.h"
#include "world/geom.h"

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
};

struct Route {
    std::string name;
    std::vector<Vec3> markers;
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
    void despawn(EntityHandle h);
    Entity *get(EntityHandle h);
    const Entity *get(EntityHandle h) const;

    // Restore authored/runtime-visible registry state without rewinding the
    // host-only lifetime serial. Collision and skeletal caches use that serial
    // to distinguish entities that reuse the same packed pool/slot handle.
    void restore_from(const EntityRegistry &snapshot);

    // Faithful to EntityPool_FindByNetId @0x4f0a20: scans pool 0 first, then pools
    // 1..3 (mask &0xF), first match wins; returns (pool<<12)|slot, else invalid.
    EntityHandle find_by_net_id(uint16_t net_id) const;

    void by_group(uint8_t group, std::vector<EntityHandle> &out) const;
    void by_team(uint8_t team, std::vector<EntityHandle> &out) const;
    EntityHandle find_by_name(std::string_view name) const;
    void in_area(const Aabb &zone, std::vector<EntityHandle> &out) const;

    // Named, first-class non-entity addressables.
    // Returns the area INDEX (the id space zone-resolved refs use). zone_id is the
    // authored record id [orig: zone record dword @0].
    int register_area(std::string name, const Aabb &bounds, bool active = true,
                      int32_t zone_id = -1);
    // The load-time id -> index resolve [orig: the @0x453000/@0x453100 scan over
    // record[0]]; -1 when no record carries the id.
    int area_index_by_zone_id(int32_t zone_id) const;
    int find_area(std::string_view name) const;              // -1 if absent
    const Area *area(int id) const;

    int register_route(std::string name, std::vector<Vec3> markers); // returns route id
    int find_route(std::string_view name) const;
    const Route *route(int id) const;

    int intern_group(std::string_view name); // stable id for a named group

    size_t live_count() const;

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

private:
    struct Pool {
        std::vector<Entity> slots;
        std::vector<char> used; // char (not bool) for stable addressing
        size_t live = 0;
    };
    std::array<Pool, kPoolCount> pools_{};
    uint64_t next_spawn_id_ = 1; // zero means "identity not recorded"
    std::vector<Area> areas_;
    std::vector<Route> routes_;
    std::vector<std::string> group_names_;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ENTITY_REGISTRY_H
