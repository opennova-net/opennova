#pragma once

#include <array>
#include <cstdint>

namespace opennova::renderer {

// THE TERRAIN LEG OF THE DYNAMIC LIGHT POOL — the projected-texture pass that
// puts a light's pool of illumination on the GROUND, as distinct from the
// object shader's per-vertex term that `light_scene` already carries.
//
// Retail re-draws the terrain block once more PER LIGHT, additively, through a
// two-stage projected-texture technique
// [orig: render_terrain_sector_batch @0x6092A0 — the `else` of
//  `if (!shadow_count || dword_319FB84)`; the per-light draw
//  render_foliage_instance @0x5AA830, whose decompiled NAME IS A MISNOMER: its
//  argument is a Light_InstanceTable slot index @0x5AA844, not a foliage
//  instance].
//
// This was the "terrain projected circles" residual recorded against D-RLIT-4.

// The terrain pass's own literal [orig: @0x7D3E68 = 0.66000003] and the 0.5
// that accompanies it on the Ambient term [orig: @0x7C3B94].
inline constexpr float kTerrainLightFactor = 0.66000003f;
inline constexpr float kTerrainAmbientHalf = 0.5f;

// WHY THE 0.5 EXISTS, which is the part that makes this look wrong if it is
// copied without its reason: the pass runs under MODULATE2X, so stage 0
// contributes a 2x of its own. The witnessed Ambient term carries
// 0.66 * 0.5 precisely BECAUSE that stage-0 2x brings it back:
// 0.66 * 0.5 * 2 == 0.66. Stage 1's 2x is left uncompensated, and the shader
// side is what applies it.
//
// So the value PUBLISHED to the shader is rgb * blend * 0.66, and the
// on-screen contribution is 2 * g * that. Dropping either the 0.5 or the
// stage-0 2x alone changes the result by a factor of two.
inline float terrain_light_published(float channel, float blend) {
	return channel * blend * kTerrainLightFactor;
}

// The Ambient term as the original composes it
// [orig: render_foliage_instance @0x5AA9AF (the 0.66 @0x7D3E68) / @0x5AAA88].
inline float terrain_light_ambient(float channel, float blend,
		float ambient_scale, float per_channel_factor) {
	return channel * blend * ambient_scale * per_channel_factor *
			kTerrainLightFactor * kTerrainAmbientHalf;
}

// THE PER-CHANNEL FACTOR is the terrain pass's OWN triple
// [orig: flt_2732DA{4,8,C} @0x5AA1EF..0x5AA245], not the ambient scale and not
// a per-tick recompute. An earlier reading conflated it with
// Env_TerrainColorRecip; it is a separate value derived from the environment's
// terrain colour, and its default 0x808080 [orig: @0x57C065] divides to
// exactly (1, 1, 1).
inline constexpr uint32_t kTerrainFactorDefaultPacked = 0x808080u;
inline constexpr float kTerrainFactorDivisor = 128.0f;

inline std::array<float, 3> terrain_per_channel_factor(uint32_t packed_rgb) {
	return {
		static_cast<float>((packed_rgb >> 16) & 0xFFu) / kTerrainFactorDivisor,
		static_cast<float>((packed_rgb >> 8) & 0xFFu) / kTerrainFactorDivisor,
		static_cast<float>(packed_rgb & 0xFFu) / kTerrainFactorDivisor,
	};
}

// The projected texture's scale: 0.4 over the light's radius
// [orig: the two projected textures at 0.4/radius].
inline constexpr float kTerrainProjectScale = 0.4f;

inline float terrain_project_scale(float radius) {
	if (radius <= 0.0f) return 0.0f;
	return kTerrainProjectScale / radius;
}

} // namespace opennova::renderer
