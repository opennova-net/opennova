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
// Whether the S2C 0x49 echo of an authority reload refills `addressed` (a playing
// authority only; the definition carries the witness).
bool reload_echo_refills(const World &world, const Entity &addressed);
// Queue the walk's C2S 0x25 for `addressed` (an EWeap's param 0xFFFF, else the combo).
void queue_walk_reload(World &world, const Entity &addressed, const WeaponTableEntry &weapon);
}
