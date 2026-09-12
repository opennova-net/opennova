#pragma once
#include <runtime/world/weapon_fsm.h>

namespace opennova::world {
class World;
struct Entity;
struct WeaponTableEntry;
struct FixedVec3;

// Supply owner/environment facts to the same predicate used by local, AI and host fire.
void weapon_fire_environment_inputs(const World &world, const Entity &owner,
                                    WeaponFsmInputs &inputs);
int weapon_fire_owner_status(const World &world, const Entity &owner,
                             const WeaponTableEntry *weapon, bool alternate);
int weapon_fire_origin_status(const World &world, const Entity &owner,
                              const WeaponTableEntry &weapon, const FixedVec3 &origin,
                              bool replay_client_action);
}
