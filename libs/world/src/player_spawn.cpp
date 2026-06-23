#include "world/player_spawn.h"

#include "world/angle.h"
#include "world/ai.h"           // AiSystem, AiEntity
#include "world/entity_spawn.h" // entity_reset_to_spawn_state
#include "world/geom.h"         // to_fixed
#include "world/infantry.h"     // anim_state
#include "world/world.h"        // World, registry, cached

namespace opennova::world {

namespace {

// The shared faithful §5.2b spawn: alloc a pool-0 0x14B9 entity, init health, place the pose,
// clear the movement gate, and mount the infantry motor. `is_local` selects the host's OWN
// player (input-ordered, publishes World::cached.local_player) vs a REMOTE peer (a joiner's
// entity the host snaps from the wire — never the local player). [orig: net-re §5.2b/§5.38]
EntityHandle spawn_player_entity(World &world, const PlayerSpawn &spawn, bool is_local) {
    // §5.2b steps 1-4: a pool-0 player-infantry entity (type 0x14B9), item-template health,
    // placed pose. kind=Organic so entity_wire_bridge::entity_class_of resolves it as Player.
    Entity seed;
    seed.net_id = spawn.net_id;
    // Key the player by its net_id as the bms_id too, so the host can resolve a player avatar
    // node for it through the present pass (the player has no BMS placement of its own). A high
    // net_id (0xFFF0) won't collide with the small mission bms ids. [net-re §5.38; ADR 0012]
    seed.bms_id = static_cast<int32_t>(spawn.net_id);
    seed.kind = EntityKind::Organic;
    seed.item_id = kPlayerInfantryTypeId;
    seed.position = spawn.position;
    seed.yaw = spawn.yaw;
    seed.team = spawn.team;
    seed.health = spawn.health; // [orig: Entity_InitFromItemDef @0x49e550 — healthMax -> Health]
    seed.alive = true;
    seed.flags = 2u;            // the movement gate starts SET; the spawn reset clears it

    const EntityHandle h = world.registry.spawn(0, seed);
    if (!h.valid()) return h;
    Entity *ent = world.registry.get(h);

    // §5.2b step 5: clear the entity+36 bit-1 movement gate + back up the spawn position.
    // [orig: Entity_ResetToSpawnState @0x4B9610]
    entity_reset_to_spawn_state(*ent);

    // Mount the infantry motor — same motor as an NPC organic. The host's own player is ordered
    // from input and never AI-think/routed; a remote peer is snapped from the wire and the motor
    // skips it once net-snapped (inf.is_local_player stays false). [orig: net-re §5.38; ADR 0012]
    if (world.ai == nullptr) return EntityHandle{};
    const int idx = world.ai->attach(h);
    AiEntity &ae = *world.ai->at(idx);
    ae.pos[0] = to_fixed(spawn.position.x);
    ae.pos[1] = to_fixed(spawn.position.y);
    ae.pos[2] = to_fixed(spawn.position.z);
    // Mission yaw (deg) -> engine heading (BAM32), the canonical (90 - yaw) convention.
    ae.heading = bam_heading_from_mission_yaw_deg(spawn.yaw);
    ae.team = spawn.team;
    ae.net_id = spawn.net_id;
    ae.health = spawn.health;
    ae.inf.active = true;
    ae.inf.is_local_player = is_local;
    ae.inf.body_heading = ae.heading;
    ae.inf.target_heading = ae.heading;
    ae.inf.max_health = spawn.health;
    ae.inf.anim_state = anim_state::kIdle;

    // Publish the local-player handle ONLY for the host's own player — the net anchor + present
    // resolve it. A remote peer never becomes the local player. [ADR 0012]
    if (is_local) world.cached.local_player = h;
    return h;
}

} // namespace

EntityHandle spawn_player(World &world, const PlayerSpawn &spawn) {
    return spawn_player_entity(world, spawn, /*is_local=*/true);
}

EntityHandle spawn_remote_player(World &world, const PlayerSpawn &spawn) {
    return spawn_player_entity(world, spawn, /*is_local=*/false);
}

} // namespace opennova::world
