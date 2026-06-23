// The host's own-player spawn — a faithful subset of the §5.2b host player-spawn machine
// (docs/net/novaworld-net-re.md §5.2b/§5.38), in libs/world so the netsim/Phase-2 listen
// server can spawn the player without a libs/mission dependency. The player is an
// authoritative pool-0 World entity (ADR 0012), indistinguishable from any other simulated
// entity, driven by the SAME infantry motor as an NPC but ordered from input, not AI think.
#ifndef OPENNOVA_WORLD_PLAYER_SPAWN_H
#define OPENNOVA_WORLD_PLAYER_SPAWN_H

#include <cstdint>

#include "world/entity.h" // EntityHandle, Vec3

namespace opennova::world {

class World;

// The player infantry template id [orig: net-re §5.2b — type_id 0x14B9].
inline constexpr int32_t kPlayerInfantryTypeId = 0x14B9;

// Spawn parameters for the host's own player. `yaw` is the mission yaw in degrees (the same
// convention as a BMS heading). `net_id` is the SSN the caller assigns (default a reserved
// high value unlikely to collide with mission entities).
struct PlayerSpawn {
    Vec3 position;
    int16_t yaw = 0;
    uint8_t team = 0;
    uint16_t net_id = 0xFFF0;
    int16_t health = 100;
};

// Faithful §5.2b sequence: (1) alloc a pool-0 player-infantry (0x14B9) entity; (2/3)
// item-template health init; (4) place Position/Yaw/Team; (5) entity_reset_to_spawn_state
// (clear the entity+36 movement gate). Then mount the infantry motor as the LOCAL PLAYER
// (inf.active + inf.is_local_player; input-ordered, no AI route) and publish
// World::cached.local_player. Returns the spawn handle, or an invalid handle if pool 0 is
// full or the World has no AiSystem wired. [orig: net-re §5.2b/§5.38; ADR 0012]
EntityHandle spawn_player(World &world, const PlayerSpawn &spawn);

// Spawn a REMOTE PEER's player entity on the host (a joiner). The SAME faithful §5.2b sequence
// as spawn_player, EXCEPT it is NOT the host's own player: inf.is_local_player stays false and
// World::cached.local_player is NOT republished (the host keeps its own player as the local
// one). The peer is a full pool-0 0x14B9 entity the host SNAPs from the joiner's C2S 0x0C
// uplinks (netsim EntityWireBridge::apply_player_intent) and the motor skips once the entity is
// net-snapped. Pass a distinct net_id per joiner (the default 0xFFF0 is the host's own player).
// [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> player_ServerAdd @0x51cbc0 registers a
// joined player's entity without assigning g_local_player_entity; net-re §5.2a/§5.2b.]
EntityHandle spawn_remote_player(World &world, const PlayerSpawn &spawn);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_SPAWN_H
