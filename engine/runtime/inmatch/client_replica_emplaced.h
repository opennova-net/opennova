#pragma once
#include <cstdint>
#include <vector>
namespace opennova::mission {
struct ItemSeatSpec;
}
namespace opennova::replication {
struct ClientState;
struct ClientEntityState;
class ClientWorldMaterializer;
}
namespace opennova::world {
class World;
struct Entity;
}
namespace opennova::inmatch {
// The replica guns' two legs. Pool1Walk: every gun's publisher state, then each occupied gun's
// channel (its ai-fn and its class update's window leg) over a remote occupant's look, in the
// pool-1 walk, ahead of every pool-0 body (a remote gunner's body chase included). AfterOwnBody:
// the own gunner's gun takes the words its World twin's channel computed in the own body's update
// (world::tick_emplaced_weapon_channel), then the carriers publish their children's words.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9 ahead of the
//  pool-0 walk @0x4C2426..0x4C2472; Entity_UpdatePool1Slot @0x4B8DD0 - the +0x1C8 call @0x4B8E3C,
//  the +0x1C4 call @0x4B8E53]
enum class ReplicaGunLeg { Pool1Walk, AfterOwnBody };
// Each decoded row's native twin is its own row by the joiner's replica rule
// (replication::replica_row_entity over `materializer`; none keeps the handle
// and the type).
void tick_replica_emplaced_channels(replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs, world::World &world, uint16_t self_handle,
		const replication::ClientWorldMaterializer *materializer, bool wire_header_world,
		ReplicaGunLeg leg);
// The carrier render callback's gun words on a joiner's present row: the
// words its ewep children published on the carrier's replica row stand for
// the brain words the tank/helo callback reads. `twin` is the carrier's
// materialized world row.
void write_present_replica_vehicle_gun(float *record, const world::World &world,
		const world::Entity &twin, const replication::ClientEntityState &carrier);
}
