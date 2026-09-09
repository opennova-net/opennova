// [orig: EntityPool_FindByNetId @ 0x4f0a20 (the pool-0-first net-id resolve); docs/net/novaworld-net-re.md]
#include <runtime/world/entity_registry.h>

#include <algorithm>
#include <cctype>

#include <base/io/strutil.h>

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
            p.slots[s].facial_slot = 0;
            p.slots[s].facial_checked = false;
            p.slots[s].registry_spawn_id = next_spawn_id_++;
            if (next_spawn_id_ == 0) next_spawn_id_ = 1;
            p.used[s] = 1;
            ++p.live;
            return h;
        }
    }
    return EntityHandle{}; // pool full (faithful: spawn fails on a full fixed pool)
}

EntityHandle EntityRegistry::spawn_at(EntityHandle h, const Entity &seed) {
    if (!h.valid()) return EntityHandle{};
    const int pool = h.pool();
    const int slot = h.slot();
    if (pool < 0 || pool >= kPoolCount || slot < 0) return EntityHandle{};
    Pool &p = pools_[pool];
    if (static_cast<size_t>(slot) >= p.slots.size() || p.used[slot])
        return EntityHandle{};
    p.slots[slot] = seed;
    p.slots[slot].handle = h;
    p.slots[slot].facial_slot = 0;
    p.slots[slot].facial_checked = false;
    p.slots[slot].registry_spawn_id = next_spawn_id_++;
    if (next_spawn_id_ == 0) next_spawn_id_ = 1;
    p.used[slot] = 1;
    ++p.live;
    return h;
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

bool EntityRegistry::despawn(EntityLifetime lifetime) {
    if (get(lifetime) == nullptr) return false;
    despawn(lifetime.handle);
    return true;
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

Entity *EntityRegistry::get(EntityLifetime lifetime) {
    if (!lifetime.valid()) return nullptr;
    Entity *entity = get(lifetime.handle);
    if (entity == nullptr ||
            entity->registry_spawn_id != lifetime.registry_spawn_id)
        return nullptr;
    return entity;
}

const Entity *EntityRegistry::get(EntityLifetime lifetime) const {
    return const_cast<EntityRegistry *>(this)->get(lifetime);
}

void EntityRegistry::restore_from(const EntityRegistry &snapshot) {
    const uint64_t live_high_water = next_spawn_id_;
    *this = snapshot;
    if (next_spawn_id_ < live_high_water) next_spawn_id_ = live_high_water;
}

Entity *EntityRegistry::by_net_id(uint16_t net_id) {
    const EntityHandle h = find_by_net_id(net_id);
    return h.valid() ? get(h) : nullptr;
}

Entity *EntityRegistry::by_bms_id(int32_t bms_id) {
    EntityHandle found;
    for_each([&](const Entity &e) {
        if (!found.valid() && e.bms_id == bms_id) found = e.handle;
    });
    return found.valid() ? get(found) : nullptr;
}

EntityHandle EntityRegistry::find_by_net_id(uint16_t net_id) const {
    // [orig: EntityPool_FindByNetId @0x4f0a20] pool 0 first, then pools where
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
                                  int32_t zone_id, std::optional<Aabb> script_bounds) {
    areas_.push_back(Area{std::move(name), bounds, active, zone_id,
                         script_bounds.value_or(bounds)});
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

const Area *EntityRegistry::area(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= areas_.size()) return nullptr;
    return &areas_[id];
}

void EntityRegistry::clear_script_tables() {
    areas_.clear();
    locations_.clear();
    group_names_ = {"emptygroup", "humans", "blueplayers", "redplayers",
                    "ai", "blueai", "redai"};
    group_members_.assign(7, {});
}

void EntityRegistry::register_location(const Aabb &bounds, int32_t id) {
    locations_.push_back({bounds, id});
}

int32_t EntityRegistry::location_at(const Vec3 &position) const {
    // [orig: WacCmd_SsnLoc @0x4F0E90] Strict bounds, last matching type-5 box.
    int32_t result = 0;
    for (const LocationVolume &location : locations_) {
        const Aabb &b = location.bounds;
        if (position.x > b.min.x && position.x < b.max.x &&
                position.y > b.min.y && position.y < b.max.y &&
                position.z > b.min.z && position.z < b.max.z)
            result = location.id;
    }
    return result;
}

int EntityRegistry::intern_group(std::string_view name) {
    for (size_t i = 0; i < group_names_.size(); ++i) {
        if (iequals(group_names_[i], name)) return static_cast<int>(i);
    }
    group_names_.emplace_back(name);
    group_members_.emplace_back();
    return static_cast<int>(group_names_.size() - 1);
}

int EntityRegistry::default_script_group_index(std::string_view name) {
    static constexpr const char *names[] = {
        "emptygroup", "humans", "blueplayers", "redplayers", "ai", "blueai", "redai"
    };
    for (int i = 0; i < 7; ++i) if (iequals(name, names[i])) return i;
    return -1;
}

int EntityRegistry::script_group_index(std::string_view name) const {
    for (size_t i = 0; i < group_names_.size(); ++i)
        if (iequals(group_names_[i], name)) return static_cast<int>(i);
    return -1;
}

void EntityRegistry::set_script_group_members(int group, const std::vector<EntityHandle> &members) {
    if (group >= 7 && size_t(group) < group_members_.size()) group_members_[group] = members;
}

// [orig: Server_BuildEntitySlotLists @0x4F97A0] These lists are independent
// of entity+284's BMS command group, and retain pool-slot order and dead rows.
void EntityRegistry::script_groups(std::vector<std::vector<EntityHandle>> &out) const {
    out = group_members_;
    for (size_t i = 0; i < 7; ++i) out[i].clear();
    for_each_in_pool(0, [&](const Entity &e) {
        if (e.item_id == 0) return;
        const uint32_t flags = e.flags | e.engine_flags;
        if ((flags & kEntityFlagPlayer) != 0) {
            if ((flags & 1u) != 0) return;
            out[1].push_back(e.handle);
            if (e.team == 1 || e.team == 2) out[1 + e.team].push_back(e.handle);
        } else {
            out[4].push_back(e.handle);
            if ((flags & 1u) == 0 && (e.team == 1 || e.team == 2))
                out[4 + e.team].push_back(e.handle);
        }
    });
}

size_t EntityRegistry::live_count() const {
    size_t n = 0;
    for (const Pool &p : pools_) n += p.live;
    return n;
}

} // namespace opennova::world
