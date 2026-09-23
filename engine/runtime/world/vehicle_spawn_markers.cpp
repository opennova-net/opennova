#include "vehicle_system.h"
#include "world.h"
#include "angle.h"
#include <base/io/bam.h>
#include <algorithm>
#include <cmath>

namespace opennova::world {
namespace {
// The x87 planar norm clamps before truncation. Subtractions wrap in 32 bits.
// [orig: build_spawn_marker_budget_list @0x529D30; selector @0x529EDD]
int32_t planar_distance(const Entity &a, const Entity &b) {
	const double dx = io::bam_sub(to_fixed(a.position.x), to_fixed(b.position.x));
	const double dy = io::bam_sub(to_fixed(a.position.y), to_fixed(b.position.y));
	return static_cast<int32_t>(std::min(2147418112.0, std::sqrt(dx * dx + dy * dy)));
}
bool zone_definition(const Entity &e) {
	return e.has_item_def && (e.item_attrib & 0x60000u) == 0x60000u;
}
bool vehicle_or_untyped(const Entity &e) {
	return !e.has_item_def || e.item_type == 1;
}
} // namespace

// [orig: Game_StartMission @0x52527A..0x525301; sub_529A80 @0x529A80;
// build_spawn_marker_budget_list @0x529B40]
void VehicleSystem::build_spawn_markers() {
	spawn_markers_enabled_ = false;
	spawn_markers_.clear();
	for (auto &group : spawn_groups_)
		group.clear();
	world_.registry.for_each_in_pool(1, [&](const Entity &e) {
		if (!e.has_item_def || e.item_type != 1 || e.vehicle_spawn_team == 0)
			return;
		const int team = e.vehicle_spawn_team < 0 ? e.team : e.vehicle_spawn_team;
		if (team >= 0 && team < 5)
			spawn_groups_[team].push_back(e.handle);
	});
	int low = INT32_MAX, high = INT32_MIN, low_team = 0, high_team = 0;
	std::vector<EntityHandle> zones;
	for (int pool = 1; pool <= 2; ++pool)
		world_.registry.for_each_in_pool(pool, [&](const Entity &e) {
			if (!zone_definition(e))
				return;
			zones.push_back(e.handle); // inner nearest scan includes attrib2&4
			if ((e.item_attrib2 & 4) != 0 || e.zone_number == 0)
				return;
			if (e.zone_number < low) {
				low = e.zone_number;
				low_team = e.team;
			}
			if (e.zone_number > high) {
				high = e.zone_number;
				high_team = e.team;
			}
		});
	if (low_team == 0 || high_team == 0 || low == INT32_MAX || high == INT32_MIN)
		return;
	std::vector<EntityHandle> markers;
	world_.registry.for_each_in_pool(3, [&](const Entity &e) {
		if (e.has_item_def && (e.item_attrib2 & 4) != 0)
			markers.push_back(e.handle);
	});
	for (EntityHandle handle : markers) {
		Entity &marker = *world_.registry.get(handle);
		spawn_markers_enabled_ = true;
		if (low_team == 2)
			marker.vehicle_spawn_priority = uint8_t(high - marker.zone_number);
		else if (low_team == 1)
			marker.vehicle_spawn_priority = marker.zone_number;
		EntityHandle nearest;
		int32_t best = INT32_MAX;
		for (EntityHandle zone_handle : zones) {
			const Entity &zone = *world_.registry.get(zone_handle);
			if (zone.zone_number != marker.zone_number)
				continue;
			const int32_t distance = planar_distance(marker, zone);
			if (distance < best) {
				nearest = zone_handle;
				best = distance;
			}
		}
		spawn_markers_.push_back({ handle, nearest });
	}
}

// [orig: assign_overlay_spawn_points @0x529E60; its every-32 gate is
//  Server_TickUpdate's `test tick,1Fh` @0x51D8C4]
void VehicleSystem::tick_spawn_markers() {
	if (!spawn_markers_enabled_)
		return;
	for (int group = 0; group < 5; ++group) {
		const int direction = group == 2 ? -1 : group == 1 ? 1 : 0;
		for (const auto &pair : spawn_markers_) {
			Entity *marker = world_.registry.get(pair.marker);
			if (marker == nullptr)
				continue;
			marker->flags &= ~1u;
			marker->engine_flags &= ~1u;
			world_.registry.for_each_in_pool(1, [&](const Entity &other) {
				if (vehicle_or_untyped(other) && ((other.flags | other.engine_flags) & 6) == 0 &&
						planar_distance(other, *marker) < to_fixed(other.bound_radius)) {
					marker->flags |= 1;
					marker->engine_flags |= 1;
				}
			});
		}
		for (EntityHandle handle : spawn_groups_[group]) {
			Entity *vehicle = world_.registry.get(handle);
			if (vehicle == nullptr || ((vehicle->flags | vehicle->engine_flags) & 6) == 0)
				continue;
			vehicle->veh.respawn_waiting_for_overlay = true;
			if (vehicle->veh.stuck_ticks <= 0)
				continue;
			Entity *best = nullptr;
			for (const auto &pair : spawn_markers_) {
				const Entity *zone = world_.registry.get(pair.zone);
				Entity *marker = world_.registry.get(pair.marker);
				if (zone == nullptr || marker == nullptr || zone->zone_control < 65536 ||
						zone->team != static_cast<uint8_t>(vehicle->vehicle_spawn_team) ||
						((marker->flags | marker->engine_flags) & 1) != 0)
					continue;
				if (std::find(marker->vehicle_spawn_ids.begin(), marker->vehicle_spawn_ids.end(),
							int32_t(vehicle->item_id) + 100000) == marker->vehicle_spawn_ids.end())
					continue;
				if (best == nullptr ||
						direction * best->vehicle_spawn_priority <
								direction * marker->vehicle_spawn_priority)
					best = marker;
			}
			if (best == nullptr)
				continue;
			bool clear = true;
			world_.registry.for_each_in_pool(1, [&](const Entity &other) {
				if (other.handle == handle || !vehicle_or_untyped(other))
					return;
				// Wrecks participate here. A blocked best marker does not retry another.
				const int32_t radius =
						io::bam_add(to_fixed(other.bound_radius), to_fixed(vehicle->bound_radius));
				if (planar_distance(other, *best) < radius)
					clear = false;
			});
			if (!clear)
				continue;
			int32_t *pose = vehicle->veh.spawn_pose;
			carrier_pose_fixed(*best, pose, pose[3], pose[4], pose[5]);
			vehicle->veh.spawn_support = {};
			vehicle->veh.spawn_pose_valid = true;
			vehicle->veh.respawn_waiting_for_overlay = false;
			best->flags |= 1;
			best->engine_flags |= 1;
		}
	}
}
} // namespace opennova::world
