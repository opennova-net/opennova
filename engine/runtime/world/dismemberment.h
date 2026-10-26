#pragma once
// The dismemberment cut: the person class callback's authority leg that clones a
// lethally hit NPC's severed sections into a pool-0 corpse piece.
// [orig: OrganicClass_HandleEvent @0x407598..0x4076C9 -> Entity_CloneFromTemplateByType
//  @0x4398A0]

#include <cstdint>

namespace opennova::world {

struct Entity;
struct FixedVec3;
struct World;

// The cut of `bone` (dismemberment_mask_for_bone) on a body this lethal hit killed
// (`was_alive`): an NPC in pool 0, no player, no piece already, no NoDismember attrib,
// health at or below zero and at most half its maximum. The piece carries
// `death_anim_state` and the round's horizontal kick; the victim hides the cut sections
// once the piece and its motor exist (D-AI-9).
void try_spawn_dismemberment_piece(World &world, Entity &victim,
                                   const FixedVec3 &round_velocity_q16,
                                   int32_t bone, int32_t death_anim_state,
                                   bool was_alive);

} // namespace opennova::world
