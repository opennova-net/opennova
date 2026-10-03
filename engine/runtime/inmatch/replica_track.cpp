#include <runtime/inmatch/replica_track.h>

#include <base/gameprofile/game_type.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/world.h>

#include <utility>
#include <vector>

namespace opennova::inmatch {

namespace {

// The 0x0F route: the wire's pool-3 slots (the host's team-1 filtered blue
// route, <= 128) with the host's name ids, keeping the locally promoted marker
// facts (radius, linked event, chain-back, goals) of a re-listed node.
// [orig: NapiNPClientMsg_0x00F @0x42E47F..0x42E4A3 (Pool_GetEntryUnchecked(3,
//  slot), STRWPNAME%03i name)]
void rebuild_route(world::World &world, const replication::ClientState &state) {
	world::WaypointTrack &track = world.script.waypoints;
	std::vector<world::WaypointEntry> rebuilt;
	rebuilt.reserve(state.world_state.waypoints.size());
	for (const WorldStateWaypoint &wp : state.world_state.waypoints) {
		world::WaypointEntry entry;
		for (const world::WaypointEntry &old : track.entries) {
			if (old.pool == 3 && old.node == static_cast<int32_t>(wp.slot_id)) {
				entry = old;
				break;
			}
		}
		entry.pool = 3;
		entry.node = wp.slot_id;
		entry.name_id = wp.name_id;
		// The marker's own position: the registry's pool-3 entity (a
		// full-BMS joiner promoted it), else the decoded pool-3 row (a
		// wire-header joiner streamed it), else whatever the old entry held.
		const world::EntityHandle marker_h = world::EntityHandle::make(3, wp.slot_id);
		if (const world::Entity *marker = world.registry.get(marker_h)) {
			entry.x = world::to_fixed(marker->position.x);
			entry.y = world::to_fixed(marker->position.y);
			entry.z = world::to_fixed(marker->position.z);
		} else if (const replication::ClientEntityState *row = state.find(marker_h.packed)) {
			entry.x = row->x;
			entry.y = row->y;
			entry.z = row->z;
		}
		rebuilt.push_back(entry);
	}
	track.entries = std::move(rebuilt);
}

} // namespace

void apply_replica_track(world::World &world, const replication::ClientState &state,
		uint32_t game_type, bool route_from_wire, ReplicaTrackSeen &seen) {
	if (state.world_state.revision != seen.world_state_revision) {
		seen.world_state_revision = state.world_state.revision;
		// [orig: `cmp eax, 10020h; jnz loc_42E4FD` @0x42e447..0x42e459]
		if (state.world_state.waypoints_set) {
			if (route_from_wire) rebuild_route(world, state);
		} else {
			world.script.waypoints.build_map_poi_list(world.registry); // [orig: @0x42e4fd]
		}
	}
	if (state.local_team_assigns != seen.local_team_assigns) {
		seen.local_team_assigns = state.local_team_assigns;
		// [orig: `and eax, 0FFFDFFFFh; cmp eax, 10020h` @0x431b1b..0x431b2b]
		if (!game_type::is_waypoint_family(game_type))
			world.script.waypoints.build_map_poi_list(world.registry); // [orig: @0x431b2d]
	}
}

} // namespace opennova::inmatch
