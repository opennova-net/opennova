#pragma once
#include <cstdint>
#include <vector>
namespace opennova::mission {
struct ItemSeatSpec;
}
namespace opennova::replication {
struct ClientState;
}
namespace opennova::world {
class World;
}
namespace opennova::inmatch {
void tick_replica_emplaced_channels(replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs, world::World &world, uint16_t self_handle);
}
