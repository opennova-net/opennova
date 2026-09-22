#pragma once
#include <cstdint>
#include <vector>
namespace opennova::mission {
struct ItemSeatSpec;
}
namespace opennova::replication {
struct ClientState;
struct ClientEntityState;
}
namespace opennova::world {
class World;
struct Entity;
}
namespace opennova::inmatch {
void tick_replica_emplaced_channels(replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs, world::World &world, uint16_t self_handle);
// The carrier render callback's gun words on a joiner's present row: the
// words its ewep children published on the carrier's replica row stand for
// the brain words the tank/helo callback reads. `twin` is the carrier's
// materialized world row.
void write_present_replica_vehicle_gun(float *record, const world::World &world,
		const world::Entity &twin, const replication::ClientEntityState &carrier);
}
