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

// Hidden-section mask selected by OrganicClass_HandleEvent's dismemberment
// leg: the mask starts as the hit bone's own bit (1 << bone, x86 shl count
// masking mirrored), then the 13-case switch ORs the linked-section addend on
// top — bones 1-4 are the torso/head stack, 5-8 sever the whole limb chain
// their 9-13 partner ends. Bones above 13 keep just their own bit (the
// original's default case adds nothing). Bone 0 and negatives never reach the
// switch (the caller gates on bone > 0). [orig: 1 << boneSection @0x407601;
// the case table @0x407608-0x40766f; the > 0 gate @0x4075b4]
constexpr uint32_t dismemberment_mask_for_bone(int32_t bone) {
    if (bone <= 0) return 0u;
    uint32_t mask = 1u << (bone & 31);
    switch (bone) {
        case 1: mask |= 0x1E67Cu; break;
        case 2: mask |= 0x1E678u; break;
        case 3: mask |= 0x1E670u; break;
        case 4: mask |= 0x1E668u; break;
        case 5: mask |= 0x10200u; break;
        case 6: mask |= 0x08400u; break;
        case 7: mask |= 0x20800u; break;
        case 8: mask |= 0x41000u; break;
        case 9: mask |= 0x10000u; break;
        case 10: mask |= 0x08000u; break;
        case 11: mask |= 0x20000u; break;
        case 12: mask |= 0x40000u; break;
        case 13: mask |= 0x04000u; break;
        default: break;
    }
    return mask;
}

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
