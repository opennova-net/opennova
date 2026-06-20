#include "world/player_spawn.h"

#include "world/ai.h"           // AiSystem, AiEntity
#include "world/entity_spawn.h" // entity_reset_to_spawn_state
#include "world/geom.h"         // to_fixed
#include "world/infantry.h"     // anim_state
#include "world/world.h"        // World, registry, cached

namespace opennova::world {

namespace {
// [orig: degrees -> BAM32 = 2^32/360 = 11930464; same const as ai.cpp/promote.cpp]
constexpr int64_t kBamPerDegree = 11930464;
} // namespace

EntityHandle spawn_player(World &world, const PlayerSpawn &spawn) {
    // §5.2b steps 1-4: a pool-0 player-infantry entity (type 0x14B9), item-template health,
    // placed pose. kind=Organic so entity_wire_bridge::entity_class_of resolves it as Player.
    Entity seed;
    seed.net_id = spawn.net_id;
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

    // Mount the infantry motor as the local player — same motor as an NPC organic, but
    // ordered from input and never AI-think/routed. [orig: net-re §5.38; ADR 0012]
    if (world.ai == nullptr) return EntityHandle{};
    const int idx = world.ai->attach(h);
    AiEntity &ae = *world.ai->at(idx);
    ae.pos[0] = to_fixed(spawn.position.x);
    ae.pos[1] = to_fixed(spawn.position.y);
    ae.pos[2] = to_fixed(spawn.position.z);
    // Mission yaw (deg) -> engine heading (BAM32), the canonical (90 - yaw) convention.
    ae.heading = static_cast<int32_t>((90 - spawn.yaw) * kBamPerDegree);
    ae.team = spawn.team;
    ae.net_id = spawn.net_id;
    ae.health = spawn.health;
    ae.inf.active = true;
    ae.inf.is_local_player = true;
    ae.inf.body_heading = ae.heading;
    ae.inf.target_heading = ae.heading;
    ae.inf.max_health = spawn.health;
    ae.inf.anim_state = anim_state::kIdle;

    // Publish the local-player handle — the net anchor + present resolve it. [ADR 0012]
    world.cached.local_player = h;
    return h;
}

} // namespace opennova::world
