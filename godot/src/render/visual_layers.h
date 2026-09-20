#pragma once

#include <cstdint>

namespace godot {

// The renderer's visual-layer allocation: one contract shared by the beauty
// camera mask (render/frame_fx), the sun-shadow light masks (env/sun_shadow),
// the environment-cube capture (env/environment_cube_capture), the placed
// object layers (mission/mission_object_placer), the terrain foliage blanket
// (terrain/foliage_dispatcher), the terrain flat fallback (terrain/terrain),
// the water mirror (env/water), the weapon Inset scene camera
// (hud/hud_inset_scope) and the FirstPerson particle batch, which rides the
// viewmodel bit (particle/particle_renderer). The witness
// for the mirror's population lives with the mirror view
// (engine/runtime/environment/water_mirror.h, env #30).
namespace visual_layers {
enum : uint32_t {
	WORLD = 1u << 0,
	WATER = 1u << 10,
	// The retail environment-cube callback draws sky + sun/moon only. All 20
	// Godot visual layers are allocated, so it aliases water's bit; water
	// rejects capture-camera eyes in its shader while the admitted
	// sky/celestial meshes carry this bit in addition to WORLD.
	ENVIRONMENT_CAPTURE = WATER,
	VIEWMODEL = 1u << 11,
	FP_BODY_SHADOW_ONLY = 1u << 12,
	STATIC_SHADOW_CASTER = 1u << 13,
	DYNAMIC_SHADOW_CASTER = 1u << 14,
	TERRAIN_SHADOW_RECEIVER = 1u << 15,
	WORLD_NO_MIRROR = 1u << 16,
	// The foliage detail blanket rides its own bit (alone, inside the 20-bit
	// default mask): retail's reflection prerender hands PolyTrn a context
	// with foliage collection OFF, so the mirror draws no near-foliage patches
	// while every beauty camera admits the bit (retail: the reflection
	// context's foliage-collect field is 0 where the live beauty scene passes
	// 1 — see docs/env/env-tod-re.md #30).
	TERRAIN_FOLIAGE = 1u << 17,
	// The empty-sector flat fallback rides its own bit (alone, inside the
	// 20-bit default mask): retail's water-mirror prerender view skips empty
	// sectors whenever the mission has water, where the live beauty view
	// draws them, so the beauty camera admits the bit and the mirror mask
	// excludes it above and below water (docs/terrain/terrain-re.md,
	// "Empty-sector flat fallback", the view +100 witnesses).
	TERRAIN_FLAT_FALLBACK = 1u << 18,
	SHADOW_CASTER_MASK = STATIC_SHADOW_CASTER | DYNAMIC_SHADOW_CASTER,
	// The mirror camera's above-water mask; a below-water view adds
	// WORLD_NO_MIRROR back (retail collects unfiltered there). The render-slot
	// captures draw through SlotShadow's RenderingDevice pass and reserve no
	// visual layer.
	REFLECTION_CULL_MASK = 0xFFFFFu &
			~(WATER | VIEWMODEL | FP_BODY_SHADOW_ONLY | SHADOW_CASTER_MASK |
					WORLD_NO_MIRROR | TERRAIN_FOLIAGE | TERRAIN_FLAT_FALLBACK),
	// What a second view of the beauty scene (the weapon Inset pass) takes out
	// of the gameplay camera's mask: the first-person viewmodel, which retail
	// draws in the main frame's viewmodel-first step and never inside the
	// inset's own terrain/sky/scene pass, plus -- as for the mirror -- the
	// layer-hidden first-person body and the caster markers.
	SECOND_SCENE_VIEW_EXCLUDED = VIEWMODEL | FP_BODY_SHADOW_ONLY | SHADOW_CASTER_MASK,
};
} // namespace visual_layers

} // namespace godot
