// Spawn-state init for runtime entities — the field-init half of the host player-spawn
// machine (docs/net/novaworld-net-re.md §5.2b). Kept separate from promotion
// (libs/mission) so the netsim/Phase-2 in-process listen-server player spawn can reuse it
// without pulling in a mission dependency (libs/world stays Godot- and mission-agnostic).
#ifndef OPENNOVA_WORLD_ENTITY_SPAWN_H
#define OPENNOVA_WORLD_ENTITY_SPAWN_H

#include "world/entity.h"

namespace opennova::world {

// Faithful subset of [orig: Entity_ResetToSpawnState @ 0x4B9610] (net-re §5.2b step 5).
// Backs up the current Position into the spawn-point fields and clears the entity+36
// bit-1 movement gate — the gate Player_BuildTag0CInputBody @0x42A550 checks before the
// C2S 0x0C uplink (§5.6). This is the in-process spawn signal, not a wire message.
//
// DEFERRED (cited, not yet modeled in the portable world): the original also splats Yaw
// across the heading-field family, zeroes velocities / AI-target refs, detaches from any
// vehicle, walks pools 0/1 removing cross-references to this entity, and rebuilds the
// proximity lists. Those need the vehicle/AI/pool-cross-ref machinery the portable world
// does not model yet; Phase 2 (the moving player) needs only the gate clear + position
// backup. Revisit when death/respawn (Phase 3) and the vehicle layer land.
void entity_reset_to_spawn_state(Entity &e);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ENTITY_SPAWN_H
