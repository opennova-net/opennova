#pragma once

#include <vector>

#include <novaworld/ingame_decode.h>   // EntityClass
#include <novaworld/replication_min.h> // GameEntitySnapshot
#include <world/entity.h>
#include <world/world.h>

#include "netsim/player_intent.h" // PlayerIntent

namespace opennova::netsim {

// The single deliberate bridge between the libs/world runtime entity model
// (world::Entity / EntityRegistry) and the libs/novaworld wire model
// (GameEntitySnapshot / the §5.x compact records). This is the ONLY place the two
// representations meet — keeping libs/world net-agnostic and libs/novaworld
// sim-agnostic (ADR 0009/0011).

// Map a wire type_id to its §5.10b replication class. Phase 1 uses a minimal table
// (the player infantry template vs everything-else-is-infantry); Phase 3 replaces
// it with an items.def *_function class-tag resolver [orig: ItemDef+356]. It MUST
// agree with entity_class_of for any entity that is actually replicated, so the
// host's chosen compact encoder matches the client's chosen compact decoder.
EntityClass class_for_type_id(uint16_t type_id);

// The §5.10b replication class a live World entity replicates as. EntityClass::Unknown
// means the entity has no 0x0A compact form and is not streamed (markers/buildings).
EntityClass entity_class_of(const world::Entity &e);

// Synthesize the server-owned wire snapshot of a live World entity — the input the
// §5.9 0x0A builder consumes. Position is the entity's mission-space float lifted to
// i32 16.16 (world::to_fixed). euler_z is the engine heading (entity+16, D-NET-86).
GameEntitySnapshot snapshot_of(const world::Entity &e);

// Walk the live registry into the replicated entity set. Entities with no 0x0A
// compact form (EntityClass::Unknown) are skipped.
std::vector<GameEntitySnapshot> snapshot_world(const world::World &w);

// Host-side receive-apply of a decoded C2S 0x0C extended (type-10) player uplink to a
// REMOTE PEER entity — the host-side mover [orig: NetPacket_SerializePlayerState case 4
// @0x4c2042-0x4c20a9; docs/net/novaworld-net-re.md §5.38a/§5.10]. The inverse of
// snapshot_of's two-store read at the wire
// boundary: it SNAPS the registry Entity pose (the store snapshot_of re-broadcasts) and
// mirrors the engine-frame AiEntity (live pose + heading/pitch) + stages the smooth-target
// the CLIENT interpolation consumes, marking the entity net-snapped so the motor skips it
// [orig: Entity_UpdateInfantryAI @0x4b9a03]. Per §5.38 / ADR-0012 this is ONLY ever called
// for a remote peer — the host NEVER read-applies its own player (motor-from-input). Returns
// false if the handle is unresolved, it is the local player, or the movement/spawn gate
// (Entity.flags bit1) is set.
bool apply_player_intent(world::World &world, const PlayerIntent &intent);

} // namespace opennova::netsim
