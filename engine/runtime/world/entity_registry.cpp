// [orig: EntityPool_FindByNetId @ 0x4f0a20 (the pool-0-first net-id resolve); docs/net/novaworld-net-re.md]
#include <runtime/world/entity_registry.h>

#include <algorithm>
#include <cctype>

#include <base/io/strutil.h>

namespace opennova::world {
namespace {

using opennova::strutil::iequals;

PoolCapacities g_pool_capacities = kRetailPoolCapacity;

// The expansions whose pools are scaled, and by how much.
struct ExpansionPoolScale {
    const char *expansion;
    std::size_t scale;
};
constexpr ExpansionPoolScale kExpansionPoolScales[] = {
    {"revx02", 8},
};

} // namespace

PoolCapacities wire_pool_capacities(std::size_t pool0) {
    PoolCapacities out = kWirePoolCapacity;
    out[0] = std::min(pool0, std::size_t{0xFFF} + 1); // the handle's 12-bit slot (EntityHandle)
    return out;
}

PoolCapacities scaled_pool_capacities(std::size_t scale, const PoolCapacities &wire) {
    PoolCapacities out = kRetailPoolCapacity;
    for (std::size_t pool = 0; pool < out.size(); ++pool)
        out[pool] = std::min(out[pool] * scale, wire[pool]);
    return out;
}

PoolCapacities pool_capacities_for_expansion(const std::string &expansion,
                                             const PoolCapacities &wire) {
    for (const ExpansionPoolScale &row : kExpansionPoolScales)
        if (iequals(expansion, row.expansion)) return scaled_pool_capacities(row.scale, wire);
    return kRetailPoolCapacity;
}

const PoolCapacities &pool_capacities() { return g_pool_capacities; }

void set_pool_capacities(const PoolCapacities &capacities) { g_pool_capacities = capacities; }

std::size_t pool_capacity(int pool) {
    return pool >= 0 && pool < kEntityPoolCount ? g_pool_capacities[pool] : 0;
}

void EntityRegistry::configure_pool(int pool, size_t capacity) {
    if (pool < 0 || pool >= kPoolCount) return;
    Pool &p = pools_[pool];
    p.slots.assign(capacity, Entity{});
    p.used.assign(capacity, 0);
    p.destroyed.assign(capacity, 0);
    p.freed_by_destroy.assign(capacity, 0);
    p.ref_joined.assign(capacity, 0);
    p.fade_rows.clear();
    p.fade_listed.assign(capacity, 0);
    for (std::vector<EntityHandle> &list : ref_groups_)
        list.erase(std::remove_if(list.begin(), list.end(),
                           [pool](EntityHandle h) { return h.pool() == pool; }),
                list.end());
    p.live = 0;
    p.used_count = 0; // [orig: Pool_Clear @0x442060 -- used = 0 @0x4420DE..0x4420F6]
}

void EntityRegistry::ref_group_join(Pool &p, size_t slot) {
    const Entity &row = p.slots[slot];
    // [orig: Entity_SpawnFromBMSRecord - the refNum test @0x40EC29..0x40EC2B]
    if (!row.ref_group_member || row.ref_num == 0 || p.ref_joined[slot] == row.ref_num) return;
    ref_group_leave(p, slot);
    ref_groups_[row.ref_num].push_back(row.handle); // [orig: @0x40EC45]
    p.ref_joined[slot] = row.ref_num;
}

void EntityRegistry::ref_group_leave(Pool &p, size_t slot) {
    const uint8_t ref = p.ref_joined[slot];
    if (ref == 0) return;
    // The first entry naming the row comes off, the rest shift down.
    // [orig: Entity_Destroy - DynArray_RemoveById @0x43E858]
    std::vector<EntityHandle> &list = ref_groups_[ref];
    const auto it = std::find(list.begin(), list.end(), p.slots[slot].handle);
    if (it != list.end()) list.erase(it);
    p.ref_joined[slot] = 0;
}

void EntityRegistry::set_ref_group_member(EntityHandle h, bool member) {
    Entity *row = get(h);
    if (row == nullptr) return;
    Pool &p = pools_[h.pool()];
    const size_t slot = static_cast<size_t>(h.slot());
    row->ref_group_member = member;
    if (member)
        ref_group_join(p, slot);
    else
        ref_group_leave(p, slot);
}

void EntityRegistry::list_fade_row(EntityHandle h) {
    if (!h.valid() || (h.pool() != 2 && h.pool() != 3)) return;
    Pool &p = pools_[h.pool()];
    const uint32_t slot = static_cast<uint32_t>(h.slot());
    if (slot >= p.fade_listed.size() || p.fade_listed[slot]) return;
    p.fade_listed[slot] = 1;
    p.fade_rows.insert(std::lower_bound(p.fade_rows.begin(), p.fade_rows.end(), slot), slot);
}

void EntityRegistry::unlist_fade_row(EntityHandle h) {
    if (!h.valid() || (h.pool() != 2 && h.pool() != 3)) return;
    Pool &p = pools_[h.pool()];
    const uint32_t slot = static_cast<uint32_t>(h.slot());
    if (slot >= p.fade_listed.size() || !p.fade_listed[slot]) return;
    p.fade_listed[slot] = 0;
    p.fade_rows.erase(std::lower_bound(p.fade_rows.begin(), p.fade_rows.end(), slot));
}

bool EntityRegistry::fade_row_listed(EntityHandle h) const {
    if (!h.valid() || h.pool() >= kPoolCount) return false;
    const Pool &p = pools_[h.pool()];
    const size_t slot = static_cast<size_t>(h.slot());
    return slot < p.fade_listed.size() && p.fade_listed[slot] != 0;
}

size_t EntityRegistry::next_fade_row(int pool, size_t from) const {
    if (pool < 0 || pool >= kPoolCount) return SIZE_MAX;
    const std::vector<uint32_t> &rows = pools_[pool].fade_rows;
    const auto it = std::lower_bound(rows.begin(), rows.end(), from,
            [](uint32_t slot, size_t at) { return slot < at; });
    return it != rows.end() ? *it : SIZE_MAX;
}

void EntityRegistry::set_pool_used(int pool, size_t count) {
    // [orig: Pool_SetUsed @0x441F60 -- a plain store, not a raise]
    if (pool < 0 || pool >= kPoolCount) return;
    pools_[pool].used_count = count;
}

EntityHandle EntityRegistry::spawn(int pool, const Entity &seed, Reuse reuse) {
    return spawn_from(pool, 0, seed, reuse);
}

EntityHandle EntityRegistry::spawn_from(int pool, size_t first_slot, const Entity &seed,
                                        Reuse reuse) {
    if (pool < 0 || pool >= kPoolCount) return EntityHandle{};
    Pool &p = pools_[pool];
    for (size_t s = first_slot; s < p.slots.size(); ++s) {
        // Below the used count a Fresh (includeInUse-0) caller passes over a
        // destroyed row; from the count up every row is free.
        // [orig: Pool_AllocEntry @0x442230 -- the +0x219 test @0x442262..0x44226C]
        const bool skip_destroyed =
                reuse == Reuse::Fresh && s < p.used_count && p.destroyed[s];
        if (!p.used[s] && !skip_destroyed) {
            EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
            p.slots[s] = seed;
            p.slots[s].handle = h;
            p.slots[s].facial_slot = 0;
            p.slots[s].facial_checked = false;
            p.slots[s].registry_spawn_id = next_spawn_id_++;
            if (next_spawn_id_ == 0) next_spawn_id_ = 1;
            p.used[s] = 1;
            p.destroyed[s] = 0;
            p.freed_by_destroy[s] = 0;
            ref_group_join(p, s);
            if (seed.needs_destroy_fade()) list_fade_row(h);
            ++p.live;
            // An includeInUse-1 allocation takes the first free row below the
            // used count when there is one, so the count moves only when the
            // walk ran past it; an includeInUse-0 caller also passes over a
            // destroyed row (+0x219) and grows the count instead (D-NET-485).
            // [orig: Pool_AllocEntry @0x442230 -- the walk @0x442253..0x442274,
            //  the +0x219 test @0x442266, used + 1 @0x44228B]
            if (s + 1 > p.used_count) p.used_count = s + 1;
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
    p.destroyed[slot] = 0;
    p.freed_by_destroy[slot] = 0;
    ref_group_join(p, static_cast<size_t>(slot));
    if (seed.needs_destroy_fade()) list_fade_row(h);
    ++p.live;
    // A slotted load raises the count to the slot + 1, never lowers it.
    // [orig: Pool_UpdateMaxUsedFromSlotId @0x442100 -- @0x442115..0x442123;
    //  Pool_UpdateMaxUsed @0x441F40]
    if (static_cast<size_t>(slot) + 1 > p.used_count)
        p.used_count = static_cast<size_t>(slot) + 1;
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
        // [orig: Entity_Destroy - the refNum list removal @0x43E840..0x43E858]
        ref_group_leave(p, static_cast<size_t>(slot));
        unlist_fade_row(h);
        p.used[slot] = 0;
        p.destroyed[slot] = 1; // [orig: Entity_Destroy @0x43EA7F]
        p.freed_by_destroy[slot] = 1; // [orig: Entity_Destroy @0x43EA70..0x43EA92]
        --p.live;
    }
    // The slot keeps its bytes where retail zeroes the struct [orig: Entity_Destroy @0x43E810 -
    // memset(entity, 0, 0x2B4) @0x43EA70]: a caller that reads on after its callee destroyed
    // the body (a device-row player body's own think inside its callback, then the blast's
    // and the knife's Health tests, the think's decrement, the breath timer's and kill_ssn's
    // tails) reads the stale row in ours, the zeroed struct in retail. PERMANENT, a retail
    // use-after-destroy only authored data reaches (D-NET-348, the maintainer, 2026-10-21,
    // via main, Q7-34 (a)).
}

void EntityRegistry::clear_destroyed(EntityHandle h) {
    if (!h.valid()) return;
    const int pool = h.pool();
    const int slot = h.slot();
    if (pool < 0 || pool >= kPoolCount) return;
    Pool &p = pools_[pool];
    if (slot < 0 || static_cast<size_t>(slot) >= p.slots.size() || p.used[slot]) return;
    p.destroyed[slot] = 0;
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

const Entity *EntityRegistry::destroyed_row(EntityHandle h) const {
    if (!h.valid()) return nullptr;
    const int pool = h.pool();
    const int slot = h.slot();
    if (pool < 0 || pool >= kPoolCount) return nullptr;
    const Pool &p = pools_[pool];
    if (slot < 0 || static_cast<size_t>(slot) >= p.slots.size()) return nullptr;
    const size_t s = static_cast<size_t>(slot);
    return !p.used[s] && p.freed_by_destroy[s] ? &p.slots[s] : nullptr;
}

Entity *EntityRegistry::row_storage(EntityHandle h) {
    if (!h.valid()) return nullptr;
    const int pool = h.pool();
    const int slot = h.slot();
    if (pool < 0 || pool >= kPoolCount) return nullptr;
    Pool &p = pools_[pool];
    if (slot < 0 || static_cast<size_t>(slot) >= p.slots.size()) return nullptr;
    return &p.slots[static_cast<size_t>(slot)];
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
    // (1<<i)&0xF (i.e. 1..3); pool 4 is skipped. First match wins. Each pool's
    // scan stops at its used count [orig: @0x4F0A2D (pool 0), @0x4F0A68].
    auto scan = [&](int pool) -> EntityHandle {
        const Pool &p = pools_[pool];
        const size_t bound = walk_bound(p);
        for (size_t s = 0; s < bound; ++s) {
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
        const size_t bound = walk_bound(p);
        for (size_t s = 0; s < bound; ++s) {
            if (p.used[s] && p.slots[s].group_id == group) {
                out.push_back(EntityHandle::make(pool, static_cast<int>(s)));
            }
        }
    }
}

int EntityRegistry::register_area(std::string name, const Aabb &bounds, bool active,
                                  int32_t zone_id, std::optional<Aabb> script_bounds,
                                  bool flat,
                                  std::optional<std::array<int32_t, 4>> record_xy_q16,
                                  std::optional<std::array<int32_t, 2>> trigger_z_q16,
                                  std::optional<std::array<int32_t, 2>> record_z_q16) {
    const std::array<int32_t, 4> xy = record_xy_q16.value_or(std::array<int32_t, 4>{
            to_fixed(bounds.min.x), to_fixed(bounds.max.x), to_fixed(bounds.min.y),
            to_fixed(bounds.max.y)});
    const Aabb raw = script_bounds.value_or(bounds);
    const std::array<int32_t, 2> trigger_z = trigger_z_q16.value_or(
            std::array<int32_t, 2>{to_fixed(bounds.min.z), to_fixed(bounds.max.z)});
    const std::array<int32_t, 2> record_z = record_z_q16.value_or(
            std::array<int32_t, 2>{to_fixed(raw.min.z), to_fixed(raw.max.z)});
    areas_.push_back(Area{std::move(name), bounds, active, zone_id, raw, flat, xy, trigger_z,
                         record_z});
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
    group_names_.assign(std::begin(kDefaultScriptGroupNames), std::end(kDefaultScriptGroupNames));
    group_members_.assign(kDefaultScriptGroupCount, {});
}

void EntityRegistry::register_location(const LocationVolume &location) {
    locations_.push_back(location);
}

int32_t EntityRegistry::location_at(const EntityPosition &position) const {
    // Strict bounds on the words, signed compares; the last matching type-5 box.
    // [orig: WacCmd_SsnLoc @0x4F0E90 - the box walk @0x4F0EFC..0x4F0F24]
    int32_t result = 0;
    for (const LocationVolume &location : locations_) {
        bool inside = true;
        for (int i = 0; i < 3; ++i)
            inside = inside && position.word(i) > location.min_q16[i] &&
                     position.word(i) < location.max_q16[i];
        if (inside) result = location.id;
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
    for (int i = 0; i < kDefaultScriptGroupCount; ++i)
        if (iequals(name, kDefaultScriptGroupNames[i])) return i;
    return -1;
}

int EntityRegistry::script_group_index(std::string_view name) const {
    for (size_t i = 0; i < group_names_.size(); ++i)
        if (iequals(group_names_[i], name)) return static_cast<int>(i);
    return -1;
}

void EntityRegistry::set_script_group_members(int group, const std::vector<EntityHandle> &members) {
    if (group >= kDefaultScriptGroupCount && size_t(group) < group_members_.size())
        group_members_[group] = members;
}

// [orig: Server_BuildEntitySlotLists @0x4F97A0] These lists are independent
// of entity+284's BMS command group, and retain pool-slot order and dead rows.
// The walk covers the pool's high-water `used` count and skips a row whose
// +0x20 itemDef pointer is null [orig: @0x4f9809]: a populated-slot gate, since
// every spawn stores &g_ItemDefs[idx] (an unknown type resolves to row 0,
// ItemList_FindIndexByTypeId @0x49E100) and Entity_Destroy @0x43E810 zeros the
// row. The registry's live-slot walk is that gate; the type id is not tested.
void EntityRegistry::script_groups(std::vector<std::vector<EntityHandle>> &out) const {
    out = group_members_;
    for (int i = 0; i < kDefaultScriptGroupCount; ++i) out[size_t(i)].clear();
    for_each_in_pool(0, [&](const Entity &e) {
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

// The def test reads the row's ItemDef pointer, which only a free row lacks:
// every spawn links &g_ItemDefs[ItemList_FindIndexByTypeId(type)], and the
// lookup answers row 0 (retail's Null row) for a type items.def lacks. So the
// live walk is that test; has_item_def (false for a type without a row) is
// not, and a player whose type has no row still counts.
// [orig: Server_BuildEntitySlotLists @0x4f97a0 — zero @0x4f97c6, the def test
// @0x4F9809, `test eax,100h` @0x4F9815, `test bl,al` @0x4F9820, +1 @0x4f98b1;
// the player's link Entity_SpawnFromAnimSlotProperty @0x43C429, the BMS
// spawn's Entity_SpawnFromBMSRecord @0x40EC07; the miss
// ItemList_FindIndexByTypeId @0x49E131]
int32_t EntityRegistry::count_humans() const {
    int32_t humans = 0;
    for_each_in_pool(0, [&humans](const Entity &e) {
        const uint32_t flags = e.flags | e.engine_flags;
        if ((flags & kEntityFlagPlayer) != 0 && (flags & kEntityFlagCarried) == 0)
            ++humans;
    });
    return humans;
}

size_t EntityRegistry::live_count() const {
    size_t n = 0;
    for (const Pool &p : pools_) n += p.live;
    return n;
}

} // namespace opennova::world
