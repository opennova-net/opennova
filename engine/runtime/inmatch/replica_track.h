#pragma once

// THE REPLICA'S TRACK LEGS: what the two receive handlers that rebuild the
// client's waypoint / point-of-interest list (g_WaypointList) put onto the
// world's track. The S2C 0x0F world-state load rebuilds the route from its
// records in a waypoint gametype and builds the map POI list in any other;
// the S2C 0x50 team assign of the local player rebuilds the POI list outside
// the waypoint gametypes. A joiner takes the 0x0F route from the wire; the
// authority's own client keeps the route its mission promotion built from the
// same channel the 0x0F writer serializes.
// [orig: NapiNPClientMsg_0x00F @0x42e447..0x42e4fd; NapiNPClientMsg_TeamAssign
//  @0x431b10..0x431b2d; Entity_BuildMapPoiLists @0x42de40]

#include <cstdint>

namespace opennova::world {
class World;
}
namespace opennova::replication {
struct ClientState;
}

namespace opennova::inmatch {

// What the legs last consumed of the replica's counters.
struct ReplicaTrackSeen {
	uint32_t world_state_revision = 0;
	uint32_t local_team_assigns = 0;
};

// Each 0x0F and each local 0x50 the replica folded since `seen`, in that
// order, onto world.script.waypoints; `game_type` is the client's game type
// the 0x50 leg tests.
void apply_replica_track(world::World &world, const replication::ClientState &state,
		uint32_t game_type, bool route_from_wire, ReplicaTrackSeen &seen);

} // namespace opennova::inmatch
