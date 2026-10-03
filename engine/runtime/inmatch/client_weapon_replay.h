#pragma once
#include <cstdint>
namespace opennova::replication { struct ClientState; struct ClientEntityState; }
namespace opennova::world { class World; class LocalPlayer; struct RoundSourceState; }
namespace opennova::inmatch {
class ClientRuntime;
class JoinerRole;
void sync_replica_weapon_slots(replication::ClientState &, world::World &, uint16_t self_handle);
void replica_weapon_action_source(const replication::ClientEntityState &, world::World &,
        uint8_t flags, world::RoundSourceState &);
void tick_replica_weapon_slots(replication::ClientState &, world::World &);
// The S2C 0x35 powerup weapon grants this frame folded: the one naming the
// joiner's own player lands the weapon its copy of the row names and mounts it
// (world/powerup.h powerup_weapon_grant_received).
// [orig: NapiNPClientMsg_0x035 @0x4261A0 -- the handle resolves @0x4261CF..0x426239,
//  sub_4E03D0 @0x42624C / @0x42625B]
void apply_weapon_pickups(JoinerRole &, ClientRuntime &, world::World &, world::LocalPlayer &);
}
