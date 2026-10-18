#pragma once
#include <cstdint>
namespace opennova::replication { struct ClientState; struct ClientEntityState; }
namespace opennova::world { class World; class LocalPlayer; struct RoundSourceState; }
namespace opennova::mission { class MissionKernel; }
namespace opennova::inmatch {
class ClientRuntime;
class JoinerRole;
void sync_replica_weapon_slots(replication::ClientState &, world::World &, uint16_t self_handle);
void replica_weapon_action_source(const replication::ClientEntityState &, world::World &,
        uint8_t flags, world::RoundSourceState &);
void tick_replica_weapon_slots(replication::ClientState &, world::World &);
// A decoded shooter's Flags 0x800000 for the round its round event spawns: a
// person row's resolver-kept mirror, a pool-1 row's BB walk recomputed at the
// event (D-COL-13), another row's received word.
bool replica_shooter_indoors(mission::MissionKernel &, const replication::ClientEntityState &);
// The S2C 0x35 powerup weapon grants this frame folded: the one naming the
// joiner's own player lands the weapon its copy of the row names and mounts it
// (world/powerup.h powerup_weapon_grant_received).
// [orig: NapiNPClientMsg_0x035 @0x4261A0 -- the handle resolves @0x4261CF..0x426239,
//  Player_MountPickedUpWeapon @0x42624C / @0x42625B]
void apply_weapon_pickups(JoinerRole &, ClientRuntime &, world::World &, world::LocalPlayer &);
}
