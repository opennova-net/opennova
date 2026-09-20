// Spawn-state init for runtime entities — the field-init half of the host player-spawn
// machine (docs/net/novaworld-net-re.md §5.2b). Kept separate from promotion
// (engine/runtime/mission) so the runtime/replication/Phase-2 in-process listen-server player spawn can reuse it
// without pulling in a mission dependency (engine/runtime/world stays Godot- and mission-agnostic).
#pragma once

#include <runtime/world/entity.h>

namespace opennova::world {

class World;
class AiSystem;

// [orig: Entity_ResetToSpawnState @0x4B9610] Entity-only seeding is for a
// fresh row before its motor is attached. Live rows use the World overload:
// it also resets the motor, clears references/mounts and refreshes collision.
void entity_reset_to_spawn_state(Entity &e);
void entity_reset_to_spawn_state(World &world, Entity &e);
void entity_reset_to_spawn_state(World &world, AiSystem &ai, Entity &e);

// Fresh NPC initialization after its ADM, definition and collision bindings.
// [orig: Entity_InitOrganicAI @0x4BFCC0; warmup @0x4B8B20]
void initialize_organic_ai(World &world, Entity &e);

enum class NpcCorpseStep { Kept, Respawned, Removed };
bool npc_respawn_unhide(World &world, const AiSystem &ai, Entity &e);
NpcCorpseStep step_npc_corpse(World &world, AiSystem &ai, Entity &e);

} // namespace opennova::world
