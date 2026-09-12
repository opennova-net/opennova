#pragma once
#include <cstdint>
namespace opennova::replication { struct ClientState; struct ClientEntityState; }
namespace opennova::world { class World; struct RoundSourceState; }
namespace opennova::inmatch {
void sync_replica_weapon_slots(replication::ClientState &, world::World &, uint16_t self_handle);
void replica_weapon_action_source(const replication::ClientEntityState &, world::World &,
        uint8_t flags, world::RoundSourceState &);
void tick_replica_weapon_slots(replication::ClientState &, world::World &);
}
