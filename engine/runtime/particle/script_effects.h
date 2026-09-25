#pragma once

#include <runtime/particle/effect_scene.h>
#include <runtime/world/script_effects.h>

namespace opennova::particle {

// Shared native consumer for WAC/BMS descriptors. The embedder supplies its
// entity-slot/owner tokens and water plane; all script lifetime rules stay here.
// `gate` is the descriptor spawn's section stamp: every script handler tags
// its group with the entity it spawns at (fx2ssn, fx2tgt, fxrain's local
// player, the marker action) [orig: WacScript_SpawnEffectAtSsnEntity
// @ 0x4F23A0, WacScript_SpawnEffectAtTargetMarker @ 0x4F7FD0, WacCmd_FxRain
// @ 0x4EE3E0, EventAction_SpawnParticleEffect @ 0x4540E0 — descriptor +0x0C].
EffectSpawnReceipt spawn_script_effect(EffectScene &scene, const world::ScriptEffectEvent &event,
        EffectSlotToken slot, EffectOwnerToken owner, uint32_t age_ticks = 0,
        float water_height = 0.0f, const EffectSectionGate &gate = {});

} // namespace opennova::particle
