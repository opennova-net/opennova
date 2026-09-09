#pragma once

#include <runtime/particle/effect_scene.h>
#include <runtime/world/script_effects.h>

namespace opennova::particle {

// Shared native consumer for WAC/BMS descriptors. The embedder supplies its
// entity-slot/owner tokens and water plane; all script lifetime rules stay here.
EffectSpawnReceipt spawn_script_effect(EffectScene &scene, const world::ScriptEffectEvent &event,
        EffectSlotToken slot, EffectOwnerToken owner, uint32_t age_ticks = 0,
        float water_height = 0.0f);

} // namespace opennova::particle
