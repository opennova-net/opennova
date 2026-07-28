// [orig: EntityPool_FindByNetId @ 0x4f0a20 (the pool-0-first net-id resolve); docs/net/novaworld-net-re.md]
#include "world/entity_registry.h"

#include <algorithm>
#include <cctype>

#include <io/strutil.h>

namespace opennova::world {
namespace {

using opennova::strutil::iequals;

} // namespace

void EntityRegistry::configure_pool(int pool, size_t capacity) {
    if (pool < 0 || pool >= kPoolCount) return;
    Pool &p = pools_[pool];
    p.slots.assign(capacity, Entity{});
    p.used.assign(capacity, 0);
    p.live = 0;
}

EntityHandle EntityRegistry::spawn(int pool, const Entity &seed) {
    return spawn_from(pool, 0, seed);
}

EntityHandle EntityRegistry::spawn_from(int pool, size_t first_slot, const Entity &seed) {
    if (pool < 0 || pool >= kPoolCount) return EntityHandle{};
    Pool &p = pools_[pool];
    for (size_t s = first_slot; s < p.slots.size(); ++s) {
        if (!p.used[s]) {
            EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
            p.slots[s] = seed;
            p.slots[s].handle = h;
            p.slots[s].registry_spawn_id = next_spawn_id_++;
            if (next_spawn_id_ == 0) next_spawn_id_ = 1;
            p.used[s] = 1;
            ++p.live;
            return h;
        }
    }
    return EntityHandle{}; // pool full (faithful: spawn fails on a full fixed pool)
}

void EntityRegistry::despawn(EntityHandle h) {
    if (!h.valid()) return;
    int pool = h.pool();
    int slot = h.slot();
    if (pool < 0 || pool >= kPoolCount) return;
    Pool &p = pools_[pool];
    if (slot < 0 || static_cast<size_t>(slot) >= p.slots.size()) return;
    if (p.used[slot]) {
        p.used[slot] = 0;
        --p.live;
    }
}

Entity *EntityRegistry::get(EntityHandle h) {
    if (!h.valid()) return nullptr;
    int pool = h.pool();
    int slot = h.slot();
    if (pool < 0 || pool >= kPoolCount) return nullptr;
    Pool &p = pools_[pool];
    if (slot < 0 || static_cast<size_t>(slot) >= p.slots.size() || !p.used[slot]) {
        return nullptr;
    }
    return &p.slots[slot];
}

const Entity *EntityRegistry::get(EntityHandle h) const {
    return const_cast<EntityRegistry *>(this)->get(h);
}

void EntityRegistry::restore_from(const EntityRegistry &snapshot) {
    const uint64_t live_high_water = next_spawn_id_;
    *this = snapshot;
    if (next_spawn_id_ < live_high_water) next_spawn_id_ = live_high_water;
}

EntityHandle EntityRegistry::find_by_net_id(uint16_t net_id) const {
    // Faithful to EntityPool_FindByNetId @0x4f0a20: pool 0 first, then pools where
    // (1<<i)&0xF (i.e. 1..3); pool 4 is skipped. First match wins.
    auto scan = [&](int pool) -> EntityHandle {
        const Pool &p = pools_[pool];
        for (size_t s = 0; s < p.slots.size(); ++s) {
            if (p.used[s] && p.slots[s].net_id == net_id) {
                return EntityHandle::make(pool, static_cast<int>(s));
            }
        }
        return EntityHandle{};
    };
    EntityHandle h = scan(0);
    if (h.valid()) return h;
    for (int pool = 1; pool < kPoolCount; ++pool) {
        if (((1 << pool) & kActorPoolMask) == 0) continue;
        h = scan(pool);
        if (h.valid()) return h;
    }
    return EntityHandle{};
}

void EntityRegistry::by_group(uint8_t group, std::vector<EntityHandle> &out) const {
    out.clear();
    for (int pool = 0; pool < kPoolCount; ++pool) {
        const Pool &p = pools_[pool];
        for (size_t s = 0; s < p.slots.size(); ++s) {
            if (p.used[s] && p.slots[s].group_id == group) {
                out.push_back(EntityHandle::make(pool, static_cast<int>(s)));
            }
        }
    }
}

void EntityRegistry::by_team(uint8_t team, std::vector<EntityHandle> &out) const {
    out.clear();
    for (int pool = 0; pool < kPoolCount; ++pool) {
        const Pool &p = pools_[pool];
        for (size_t s = 0; s < p.slots.size(); ++s) {
            if (p.used[s] && p.slots[s].team == team) {
                out.push_back(EntityHandle::make(pool, static_cast<int>(s)));
            }
        }
    }
}

EntityHandle EntityRegistry::find_by_name(std::string_view name) const {
    for (int pool = 0; pool < kPoolCount; ++pool) {
        const Pool &p = pools_[pool];
        for (size_t s = 0; s < p.slots.size(); ++s) {
            if (p.used[s] && iequals(p.slots[s].name, name)) {
                return EntityHandle::make(pool, static_cast<int>(s));
            }
        }
    }
    return EntityHandle{};
}

void EntityRegistry::in_area(const Aabb &zone, std::vector<EntityHandle> &out) const {
    out.clear();
    for (int pool = 0; pool < kPoolCount; ++pool) {
        const Pool &p = pools_[pool];
        for (size_t s = 0; s < p.slots.size(); ++s) {
            if (p.used[s] && zone.contains(p.slots[s].position)) {
                out.push_back(EntityHandle::make(pool, static_cast<int>(s)));
            }
        }
    }
}

int EntityRegistry::register_area(std::string name, const Aabb &bounds, bool active,
                                  int32_t zone_id) {
    areas_.push_back(Area{std::move(name), bounds, active, zone_id});
    return static_cast<int>(areas_.size() - 1);
}

int EntityRegistry::area_index_by_zone_id(int32_t zone_id) const {
    // First record whose authored id matches [orig: the linear scan over
    // record[0] in the load-time zone-ref resolvers @0x453077/@0x453162].
    for (size_t i = 0; i < areas_.size(); ++i) {
        if (areas_[i].zone_id == zone_id) return static_cast<int>(i);
    }
    return -1;
}

int EntityRegistry::find_area(std::string_view name) const {
    for (size_t i = 0; i < areas_.size(); ++i) {
        if (iequals(areas_[i].name, name)) return static_cast<int>(i);
    }
    return -1;
}

const Area *EntityRegistry::area(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= areas_.size()) return nullptr;
    return &areas_[id];
}

int EntityRegistry::register_route(std::string name, std::vector<Vec3> markers) {
    routes_.push_back(Route{std::move(name), std::move(markers)});
    return static_cast<int>(routes_.size() - 1);
}

int EntityRegistry::find_route(std::string_view name) const {
    for (size_t i = 0; i < routes_.size(); ++i) {
        if (iequals(routes_[i].name, name)) return static_cast<int>(i);
    }
    return -1;
}

const Route *EntityRegistry::route(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= routes_.size()) return nullptr;
    return &routes_[id];
}

int EntityRegistry::intern_group(std::string_view name) {
    for (size_t i = 0; i < group_names_.size(); ++i) {
        if (iequals(group_names_[i], name)) return static_cast<int>(i);
    }
    group_names_.emplace_back(name);
    return static_cast<int>(group_names_.size() - 1);
}

size_t EntityRegistry::live_count() const {
    size_t n = 0;
    for (const Pool &p : pools_) n += p.live;
    return n;
}

} // namespace opennova::world
