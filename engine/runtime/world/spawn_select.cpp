#include "world/spawn_select.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "world/angle.h"
#include "world/collision.h"
#include "world/entity.h" // Entity
#include "world/game_type.h"
#include "world/world.h"  // World, EntityRegistry registry
#include "world/zone_chain.h"

namespace opennova::world {

namespace {

SpawnPointResult entity_pose(const Entity &entity) {
    SpawnPointResult out;
    out.found = true;
    out.position = entity.position;
    out.yaw = entity.yaw;
    out.pitch = entity.pitch;
    out.roll = entity.roll;
    return out;
}

SpawnPointResult marker_pose(const World &world, const Entity &marker) {
    SpawnPointResult out = entity_pose(marker);
    const Entity *parent = world.registry.get(marker.ground_target);
    if (parent == nullptr)
        return out;

    const int32_t parent_position[3] = {
        to_fixed(parent->position.x), to_fixed(parent->position.y),
        to_fixed(parent->position.z)};
    const CollisionMatrix parent_pose = collision_matrix_from_euler(
        bam_heading_from_mission_yaw_deg(static_cast<double>(parent->yaw)),
        bam_from_degrees_wrapped(static_cast<double>(parent->pitch)),
        bam_from_degrees_wrapped(static_cast<double>(parent->roll)),
        parent_position);
    const int32_t local[3] = {
        to_fixed(marker.position.x), to_fixed(marker.position.y),
        to_fixed(marker.position.z)};
    int32_t transformed[3] = {};
    parent_pose.transform_point(local, transformed);
    constexpr float kFromFixed = 1.0f / 65536.0f;
    out.position = {transformed[0] * kFromFixed,
                    transformed[1] * kFromFixed,
                    transformed[2] * kFromFixed};
    // Entity_TransformLocalToWorld adds heading while retaining the marker's
    // local pitch and roll. Entity stores the inverse mission-yaw convention.
    out.yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
        static_cast<double>(parent->yaw) + marker.yaw - 90.0)));
    return out;
}

std::vector<const Entity *> markers_of_type(const World &world, int32_t type) {
    std::vector<const Entity *> out;
    world.registry.for_each([&](const Entity &entity) {
        if (entity.handle.pool() == 3 && entity.item_id == type)
            out.push_back(&entity);
    });
    return out;
}

struct ScoredMarker {
    int32_t score = 0;
    SpawnPointResult pose;
};

void retail_shell_sort_descending(std::vector<ScoredMarker> &rows) {
    // Exact strict comparison and Knuth gap sequence.
    // [orig: CPairList_ShellSortByValue @0x526CF0]
    size_t gap = 1;
    while (gap <= rows.size() / 9)
        gap = 3 * gap + 1;
    do {
        for (size_t i = gap; i < rows.size(); ++i) {
            const ScoredMarker insert = rows[i];
            size_t j = i;
            while (j >= gap && rows[j - gap].score < insert.score) {
                rows[j] = rows[j - gap];
                j -= gap;
            }
            rows[j] = insert;
        }
        gap /= 3;
    } while (gap != 0);
}

SpawnPointResult best_marker_pose(World &world, int32_t marker_type,
                                  EntityHandle spawning_player) {
    const std::vector<const Entity *> markers = markers_of_type(world, marker_type);
    if (markers.empty())
        return {};

    std::vector<Vec3> avoid;
    world.registry.for_each([&](const Entity &entity) {
        if (entity.handle.pool() == 0 && entity.handle != spawning_player &&
            (entity.flags & kEntityFlagPlayer) != 0)
            avoid.push_back(entity.position);
    });

    // flt_7C19E0 is the shared float-to-int guard. The random arm is not an
    // equal-distance tie-break: it runs only when every candidate/avoid pair
    // remains at this clamp.
    // [orig: Entity_FindBestSpawnPoint @0x50CCC0]
    constexpr int64_t kDistanceClampFixed = 0x7FFF0000ll;
    bool any_below_clamp = false;
    std::vector<ScoredMarker> rows;
    rows.reserve(markers.size());
    for (const Entity *marker : markers) {
        ScoredMarker row;
        row.pose = marker_pose(world, *marker);
        int64_t nearest = kDistanceClampFixed;
        for (const Vec3 &other : avoid) {
            const long double dx =
                static_cast<long double>(to_fixed(row.pose.position.x)) -
                to_fixed(other.x);
            const long double dy =
                static_cast<long double>(to_fixed(row.pose.position.y)) -
                to_fixed(other.y);
            const long double distance = std::sqrt(dx * dx + dy * dy);
            if (distance < static_cast<long double>(nearest)) {
                nearest = static_cast<int64_t>(distance);
                any_below_clamp = true;
            }
        }
        row.score = static_cast<int32_t>(nearest >> 16);
        rows.push_back(row);
    }
    if (!avoid.empty() && !any_below_clamp) {
        // One CRT draw per candidate, in row order, from the world's owned
        // stream. [orig: Entity_FindBestSpawnPoint @0x50CEA2 — the per-entry
        // `rand() >> 8 & 0xFFFF` loop @0x50CEA2..0x50CEB9]
        for (ScoredMarker &row : rows)
            row.score = static_cast<uint16_t>(world.crt_rand.next() >> 8);
    }
    retail_shell_sort_descending(rows);
    return rows.front().pose;
}

SpawnPointResult target_pose(World &world, const Entity &target) {
    // The runtime-set model-userpoint name remains unrecovered; retain the
    // exact no-userpoint +1 z arm rather than inventing a name.
    SpawnPointResult out = entity_pose(target);
    out.position.z += 1.0f;
    if (target.zone_number == 0)
        return out;

    std::array<const Entity *, 32> nearby{};
    size_t count = 0;
    world.registry.for_each([&](const Entity &candidate) {
        if (count == nearby.size() || candidate.handle.pool() != 3 ||
            !candidate.has_item_def || candidate.item_id != 6007)
            return;
        const double dx = static_cast<double>(candidate.position.x) - target.position.x;
        const double dy = static_cast<double>(candidate.position.y) - target.position.y;
        const double radius = static_cast<double>(target.zone_radius);
        if (dx * dx + dy * dy <= radius * radius)
            nearby[count++] = &candidate;
    });
    if (count == 0)
        return out;

    const size_t choice = world.spawn_cycle_counter % (count + 1);
    ++world.spawn_cycle_counter;
    return choice == 0 ? out : marker_pose(world, *nearby[choice - 1]);
}

SpawnPointResult objective_coop_entity_pose(const World &world, uint8_t team) {
    const Entity *selected = nullptr;
    for (int pool : {1, 2}) {
        world.registry.for_each([&](const Entity &entity) {
            if (selected != nullptr || entity.handle.pool() != pool ||
                !entity.has_item_def || !entity.is_spawn_point ||
                entity.zone_number == 0 || entity.team != team)
                return;
            selected = &entity;
        });
        if (selected != nullptr)
            break;
    }
    if (selected == nullptr)
        return {};
    SpawnPointResult out = entity_pose(*selected);
    out.position.z += 1.0f; // unrecovered userpoint name: exact absent arm
    return out;
}

int32_t team_fallback_marker(uint8_t team) {
    switch (team) {
    case 1: return 6003;
    case 2: return 6004;
    case 3: return 6090;
    case 4: return 6091;
    default: return 0;
    }
}

SpawnPointResult no_pick_pose(World &world, EntityHandle spawning_player,
                              uint8_t player_slot, uint8_t team,
                              uint32_t game_type_value) {
    // CRenderState_GetFieldByIndex(team, 6) returns team field 7: the Deaths
    // counter (the accessor returns field[index+1]; event case 6 records a
    // death). The initial-start markers therefore serve until the team's first
    // death, after which the respawn chain takes over.
    // [orig: Server_PositionPlayerForSpawn @0x50D1DB;
    // CRenderState_GetFieldByIndex @0x52D7D0]
    const bool primary_allowed =
        world.match.team_stats(team)[MatchStats::kDeaths] == 0;
    if (game_type::is_waypoint_family(game_type_value)) {
        if (primary_allowed) {
            const std::vector<const Entity *> primary = markers_of_type(world, 6094);
            if (!primary.empty())
                return marker_pose(world, *primary[player_slot % primary.size()]);
        }
        const std::vector<const Entity *> fallback = markers_of_type(world, 6001);
        if (!fallback.empty())
            return marker_pose(world, *fallback[player_slot % fallback.size()]);
        return game_type::is_objective(game_type_value)
            ? objective_coop_entity_pose(world, team)
            : SpawnPointResult{};
    }

    const bool team_mode = game_type::is_team(game_type_value);
    const int32_t primary_type = team_mode && team >= 1 && team <= 4
        ? 6095 + team
        : team_mode ? 0 : 6095;
    if (primary_allowed && primary_type != 0 &&
        !markers_of_type(world, primary_type).empty()) {
        ++world.spawn_cycle_counter;
        return best_marker_pose(world, primary_type, spawning_player);
    }

    const int32_t fallback_type = team_mode ? team_fallback_marker(team) : 6002;
    ++world.spawn_cycle_counter;
    return fallback_type != 0
        ? best_marker_pose(world, fallback_type, spawning_player)
        : SpawnPointResult{};
}

} // namespace

SpawnPointResult resolve_player_spawn_pose(
    World &world, EntityHandle spawning_player, EntityHandle target,
    uint8_t player_slot, uint8_t team, uint32_t game_type_value) {
    // [orig: Server_PositionPlayerForSpawn @0x50CF60]
    if (const Entity *target_entity = world.registry.get(target))
        return target_pose(world, *target_entity);
    return no_pick_pose(world, spawning_player, player_slot, team,
                        game_type_value);
}

const Entity *resolve_spawn_target(const World &world, uint8_t requester_team,
                                   uint16_t handle) {
    // [orig: Server_ResolveSpawnTargetHandle @0x4fe110]
    const EntityHandle handle_view{handle};
    if (!handle_view.valid()) return nullptr;
    const int pool = handle_view.pool();
    if (pool != 0 && pool != 1 && pool != 2) return nullptr; // [orig: @0x4fe12f]
    EntityHandle h;
    h.packed = handle;
    const Entity *e = world.registry.get(h);
    if (e == nullptr) return nullptr;
    // def attrib 0x40000 "SpawnPoint" [orig: @0x4fe16f].
    if (!e->is_spawn_point) return nullptr;
    // Team gate: match, or the requester is teamless [orig: @0x4fe175..@0x4fe187].
    if (e->team != requester_team && requester_team != 0) return nullptr;
    return e;
}

bool world_has_spawn_zone(const World &world) {
    // SpawnZoneList membership has no alive filter. Use the canonical registry
    // builder so join, 0x0F and deploy gameplay cannot acquire different lists.
    // [orig: SpawnZoneList_GetCount @0x43B920;
    // Entity_BuildSpawnZoneList @0x43EAE0]
    return !build_spawn_zone_list(world).empty();
}

SpawnZoneRegistry build_spawn_zone_list(const World &world) {
    // [orig: Entity_BuildSpawnZoneList @0x43EAE0]. Collect pool 2 then pool 1 in slot
    // order (the original walks each pool base upward), def attrib 0x40000 only, no
    // alive filter; AABB accumulated as it goes (the original seeds its bounds at 0,
    // so a mission's positive coords always include the origin — reproduced for the
    // deploy-map zoom parity).
    SpawnZoneRegistry reg;
    struct Row {
        EntityHandle handle;
        int32_t key = 0;
        uint32_t retail_address = 0;
    };
    std::vector<Row> rows;
    auto collect_pool = [&](int pool) {
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != pool || !e.is_spawn_point) return;
            Row row;
            row.handle = e.handle;
            // key = typePriority<<16 | (unitType & 0xFF)<<8 | zone# & 0x1F
            // [orig: @0x43ec9b — ItemDef.type 1 (vehicle) -> 2, 32 -> 1, else 0]
            const int type_priority = e.item_type == 1 ? 2 : (e.item_type == 32 ? 1 : 0);
            const ItemDeathTraits *traits = world.item_death_traits.get(e.item_id);
            const uint8_t unit_type =
                    traits ? static_cast<uint8_t>(traits->unit_type) : 0;
            row.key = (type_priority << 16) | (unit_type << 8) | (e.zone_number & 0x1F);
            // Retail owns every pool in one contiguous allocation. Recreate
            // the two relevant pointer offsets so the raw-address fallback is
            // deterministic without exposing host allocator addresses.
            // [orig: EntityPool_Allocate @0x442130; compare @0x43ECC6]
            row.retail_address = pool == 1
                    ? 232420u + static_cast<uint32_t>(e.handle.slot()) * 1360u
                    : 1865416u + static_cast<uint32_t>(e.handle.slot()) * 812u;
            rows.push_back(row);
            const int32_t x = to_fixed(e.position.x);
            const int32_t y = to_fixed(e.position.y);
            if (x < reg.min_x) reg.min_x = x;
            if (y < reg.min_y) reg.min_y = y;
            if (x > reg.max_x) reg.max_x = x;
            if (y > reg.max_y) reg.max_y = y;
        });
    };
    collect_pool(2);
    collect_pool(1);
    // Ascending stable sort = the original bubble sort's behavior. Equal
    // nonzero keys keep collection order; a both-zero pair compares the exact
    // virtual address within retail's contiguous entity-pool allocation.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) {
                         if (a.key != b.key) return a.key < b.key;
                         return a.key == 0 &&
                                a.retail_address < b.retail_address;
                     });
    reg.entries.reserve(rows.size());
    for (const Row &row : rows) reg.entries.push_back(row.handle);
    return reg;
}

int spawn_zone_index_of(const SpawnZoneRegistry &registry, EntityHandle handle) {
    // [orig: SpawnZoneList_IndexOf @0x43B990 — linear scan, -1 on miss]
    for (size_t i = 0; i < registry.entries.size(); ++i)
        if (registry.entries[i].packed == handle.packed) return static_cast<int>(i);
    return -1;
}

bool team_has_available_spawn_zone(const World &world, uint8_t team) {
    // Despite the original helper's reverse-engineered name, its loop is over
    // SpawnZoneList and contains no player/alive census. +538 is zone number;
    // +540 is 16.16 control. [orig: Entity_HasAliveEntityOfTeam @0x4FC7B0]
    const SpawnZoneRegistry registry = build_spawn_zone_list(world);
    for (const EntityHandle handle : registry.entries) {
        const Entity *zone = world.registry.get(handle);
        if (zone == nullptr || zone->team != team) continue;
        if (zone->zone_number == 0 || zone->zone_control >= 0x10000)
            return true;
    }
    return false;
}

uint16_t SpawnWaveEntry::requester_countdown(EntityHandle requester) const {
    size_t position = queued.size();
    for (size_t i = 0; i < queued.size(); ++i) {
        if (queued[i] == requester) {
            position = i;
            break;
        }
    }
    // Retail writes the arithmetic into a u16 packet field; retain its low
    // word rather than applying a reimplementation-only saturation policy.
    // [orig: SpawnWaveEntry_MemberEta @0x52A2FF (countdown + index * interval);
    // SpawnWaveEntry_TailEta @0x52A66E (countdown + count * interval)]
    const int64_t eta = static_cast<int64_t>(countdown) +
            static_cast<int64_t>(position) * interval;
    return static_cast<uint16_t>(eta);
}

void SpawnWaveList::build_from_mission(const World &world,
                                       int32_t base_interval,
                                       int32_t numbered_zone_interval) {
    entries_.clear();
    // Retail scans pool 2, then pool 1, and registers every SpawnPoint ItemDef
    // regardless of alive state. A numbered zone uses its dedicated interval
    // when nonzero, otherwise it falls back to the base interval.
    // [orig: SpawnWaveList_BuildFromMission @0x52A920]
    for (const int pool : {2, 1}) {
        const size_t capacity = world.registry.pool_capacity(pool);
        for (size_t slot = 0; slot < capacity; ++slot) {
            const Entity *zone = world.registry.get(
                    EntityHandle::make(pool, static_cast<int>(slot)));
            if (zone == nullptr || !zone->is_spawn_point) continue;
            const int32_t interval =
                    zone->zone_number != 0 && numbered_zone_interval != 0
                            ? numbered_zone_interval
                            : base_interval;
            if (interval == 0) continue;
            SpawnWaveEntry entry;
            entry.zone = zone->handle;
            entry.team = zone->team;
            entry.interval = interval;
            entry.queued.reserve(8);
            entries_.push_back(std::move(entry));
        }
    }
}

bool SpawnWaveList::has_entry(EntityHandle zone) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const SpawnWaveEntry &entry) {
                           return entry.zone == zone;
                       });
}

bool SpawnWaveList::remove_player(EntityHandle player) {
    bool removed = false;
    for (SpawnWaveEntry &entry : entries_) {
        const auto old_end = entry.queued.end();
        const auto new_end = std::remove(entry.queued.begin(), old_end, player);
        if (new_end != old_end) {
            entry.queued.erase(new_end, old_end);
            removed = true;
        }
    }
    return removed;
}

bool SpawnWaveList::try_queue(const World &world, EntityHandle zone,
                              EntityHandle player) {
    const Entity *player_entity = world.registry.get(player);
    if (player_entity == nullptr) return false;
    auto target = std::find_if(entries_.begin(), entries_.end(),
                               [&](const SpawnWaveEntry &entry) {
                                   return entry.zone == zone &&
                                          entry.team == player_entity->team;
                               });
    if (target == entries_.end()) return false;
    if (std::find(target->queued.begin(), target->queued.end(), player) !=
            target->queued.end())
        return false;
    if (target->queued.size() >= 8) return false;
    // A player belongs to at most one wave group. Retail removes the pointer
    // from all other rows before appending it to the selected row.
    // [orig: SpawnWaveList_TryQueuePlayer @0x52A490]
    remove_player(player);
    target->queued.push_back(player);
    return true;
}

std::vector<SpawnWaveRelease> SpawnWaveList::tick(const World &world) {
    std::vector<SpawnWaveRelease> releases;
    for (SpawnWaveEntry &entry : entries_) {
        if (entry.countdown > 0) {
            --entry.countdown;
            const Entity *zone = world.registry.get(entry.zone);
            if (zone == nullptr || zone->zone_control < 0x10000) {
                entry.queued.clear();
                entry.countdown = 0;
            }
            continue;
        }
        if (entry.queued.empty()) continue;
        releases.push_back({entry.queued.front(), entry.zone});
        entry.queued.erase(entry.queued.begin());
        entry.countdown = entry.interval;
    }
    return releases;
}

void SpawnWaveList::reset_on_zone_team_change(const World &world,
                                              EntityHandle zone_handle) {
    const Entity *zone = world.registry.get(zone_handle);
    if (zone == nullptr) return;
    for (SpawnWaveEntry &entry : entries_) {
        if (entry.zone != zone_handle || entry.team == zone->team) continue;
        entry.queued.clear();
        entry.countdown = 0;
        entry.team = zone->team;
    }
}

const Entity *find_spawn_zone_for_team(const World &world, const ZoneChain &chain,
                                       uint8_t team, uint32_t game_type_value) {
    // [orig: find_spawn_entity_for_team @0x4fc810]
    const Entity *found = nullptr;
    if (game_type::is_objective(game_type_value)) {
        // Co-op branch: the LAST team-matching un-numbered spawn entity [orig: @0x4fc834].
        world.registry.for_each([&](const Entity &e) {
            const int pool = e.handle.pool();
            if (pool != 1 && pool != 2) return;
            if (!e.is_spawn_point) return;
            if (e.team == team && e.zone_number == 0) found = &e;
        });
        return found;
    }
    // Team branch: an owned zone that is enemy-capturable (the front line) or carries
    // the team's frontier number, fully secured. [orig: @0x4fc8c3..@0x4fc963 — the
    // walk runs over the zone registry; the frontier number comes from
    // ZoneSlotChain_FindFrontierZone]
    const uint8_t frontier = zone_chain_frontier_zone(world, chain, team);
    const uint8_t enemy = (team == 1) ? 2 : (team == 2) ? 1 : 0;
    if (enemy == 0) return nullptr; // [orig: teams other than 1/2 fall out @0x4fc8fc]
    for (const EntityHandle h : chain.zones) {
        const Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        if (e->team != team || e->zone_number == 0) continue;
        const bool enemy_front = zone_chain_is_capturable(world, chain, enemy, *e);
        const bool at_frontier = frontier != 0 && e->zone_number == frontier;
        if ((enemy_front || at_frontier) && e->zone_control >= 0x10000) return e;
    }
    return nullptr;
}

} // namespace opennova::world
