// Canonical AI body-animation slots.
//
// The infantry AI writes one of these into Entity.body_anim_slot each tick (a minimal port
// of Entity_UpdateInfantryAI @0x4b9910's anim selection); the host resolves the slot to a
// clip via the entity's .adm. These map to the AI .adm KEY namespace (anim_idle,
// anim_walk_forward, ...), which is what 3rd-person NPC .adm files use (see REVX02
// 1REG_AK1.adm etc.). This is DISTINCT from the player avatar's 252-entry off_8135F0 slot
// table (walk_forward/knife/pistol/...), which is driven by player input + weapon and is a
// separate, later concern — and from Entity.anim_slot (the retail entity+0x374
// character-model selector; entity.h). body_anim_slot == -1 means "no clip / hold rest".

#ifndef OPENNOVA_WORLD_BODY_ANIM_H
#define OPENNOVA_WORLD_BODY_ANIM_H

#include <cstdint>

namespace opennova::world {

enum BodyAnim : int32_t {
    kBodyAnimReset = 0,        // anim_reset    (bind/rest)
    kBodyAnimIdle = 1,         // anim_idle     (green idle: no alert, no target)
    kBodyAnimWalkForward = 2,  // anim_walk_forward (normal walk, no target / near goal)
    kBodyAnimJogForward = 3,   // anim_jog_forward
    kBodyAnimRunForward = 4,   // anim_run_forward  (alert, open space)
    kBodyAnimAttack = 5,       // anim_attack
    kBodyAnimReload = 6,       // anim_reload
    kBodyAnimGuard = 7,        // anim_guard
    kBodyAnimCount = 8,
};

// The AI .adm key for a body-anim slot, or "" if out of range. The host resolves this key
// against the entity's loaded .adm and falls back to anim_idle / anim_reset if absent.
inline const char *body_anim_adm_key(int32_t slot) {
    switch (slot) {
        case kBodyAnimReset: return "anim_reset";
        case kBodyAnimIdle: return "anim_idle";
        case kBodyAnimWalkForward: return "anim_walk_forward";
        case kBodyAnimJogForward: return "anim_jog_forward";
        case kBodyAnimRunForward: return "anim_run_forward";
        case kBodyAnimAttack: return "anim_attack";
        case kBodyAnimReload: return "anim_reload";
        case kBodyAnimGuard: return "anim_guard";
        default: return "";
    }
}

}  // namespace opennova::world

#endif  // OPENNOVA_WORLD_BODY_ANIM_H
