#pragma once

namespace opennova::world {

class World;
struct Entity;
struct MedicInteraction;

// The authority's transactions the entity update raises inline, at the body's
// turn in the pool-0 walk. They live on the host session's player slots, so the
// host session arms them on its world for each server tick and the entity pass
// disarms them as it ends; a world no server session drives (a joiner, the bare
// local role) has none, and its player bodies' edges latch the death alone.
class IEntityUpdateTransactions {
public:
    virtual ~IEntityUpdateTransactions() = default;
    // The player body's death edge, after its latch: Entity_CheckAndProcessDeath
    // on a Flags 0x100 body is GameEvent_PlayerDeath.
    // [orig: Entity_UpdateInfantryPlayerBody -- `cmp is_authority` @0x4B4CE1, the
    //  Entity_CheckAndProcessDeath call @0x4B4CEA; Entity_CheckAndProcessDeath
    //  @0x51B550 -- the Flags 0x100 test @0x51B55D, GameEvent_PlayerDeath @0x51B55F]
    virtual void player_death(World &world, Entity &body) = 0;
    // The explosion queue's medic-kit arm on a same-team body: a dead target's
    // revive, a live hurt target's heal (Health = max), ahead of the pool-0 walk.
    // [orig: Projectile_ProcessExplosionQueue @0x4EADFC ->
    //  GameEvent_HandleMedicInteraction @0x4E6790 -- the is_authority tests
    //  @0x4E67D4 / @0x4E67FE, GameEvent_RevivePlayer (the call @0x4E67D8),
    //  GameEvent_HealPlayer (the call @0x4E6805)]
    virtual void medic_interaction(World &world, const MedicInteraction &interaction) = 0;
};

} // namespace opennova::world
