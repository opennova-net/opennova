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
// [orig: render_terrain_sector_batch @0x6092A0 — the per-light loop
//  @0x609870..0x6098BC: each handle passes
//  LightInstance_IsAliveAndLightsTerrain @0x609880 (a live handle whose flags
//  lack 0x400), then the render-mode dword_319FBD4 & 0x100 selects
//  foliage_setup_render_matrices @0x6098A5 or the normal
//  Light_SetupTerrainProjectedPass @0x6098B4 (ex `render_foliage_instance`:
//  its argument is a Light_InstanceTable slot index @0x5AA857, not a foliage
//  instance)].
//
// STAGED, NOT WIRED: this was the "terrain projected circles" residual
// recorded against D-RLIT-4; the terrain shader still carries no point-light
// input.

// The flag that keeps a light off the terrain pass (light_scene.h's "terrain
// disabled (flag 1024)") [orig: LightInstance_IsAliveAndLightsTerrain @0x609880].
inline constexpr uint32_t kLightFlagNoTerrain = 0x400u;

// The terrain pass's own literal [orig: flt_7D3E68 = 0.66000003 @0x5AA9C8 /
// @0x5AA9E9 / @0x5AAA01] and the 0.5 that accompanies it on the Ambient
// constants [orig: dbl_7C3618 (0.5) @0x5AAA9B..0x5AAAB3].
inline constexpr float kTerrainLightFactor = 0.66000003f;
inline constexpr float kTerrainAmbientHalf = 0.5f;

// WHY THE 0.5 EXISTS, which is the part that makes this look wrong if it is
// copied without its reason: the pass's blend doubles stage 0 (MODULATE2X —
// the stage state is set by the shader the pass applies, GfxShader_ApplyPassChecked
// @0x5AAAFE/@0x5AAB16, not by this function). The witnessed Ambient term
// carries 0.66 * 0.5 precisely BECAUSE that stage-0 2x brings it back:
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
// [orig: Light_SetupTerrainProjectedPass @0x5AA9C8..0x5AAA01 — record float
//  +44/+48/+52 (rgb) * float +56 (blend) * EffectWorld_AmbientScale{R,G,B} *
//  flt_2732DA{C,8,4} * 0.66, then * RgbGen_EvaluateColor when the record
//  carries a gen block @0x5AAA05..0x5AAA5F, then * 0.5 into constants 4..6
//  @0x5AAA9B..0x5AAAB3].
inline float terrain_light_ambient(float channel, float blend,
		float ambient_scale, float per_channel_factor) {
	return channel * blend * ambient_scale * per_channel_factor *
			kTerrainLightFactor * kTerrainAmbientHalf;
}

// THE PER-CHANNEL FACTOR is the environment's packed terrain colour
// `Env_TerrainColorRecip` unpacked to floats ONCE per tick: red is byte 2 into
// flt_2732DAC, green byte 1 into flt_2732DA8, blue byte 0 into flt_2732DA4,
// each times flt_7C3DD4 = 1/128
// [orig: EffectWorld_TickInstancesAndLightScale @0x5AA1EF..0x5AA23F]. It is
// not the ambient scale, and the pass reads the three floats rather than
// re-unpacking. The environment default 0x808080 [orig: Environment_InitDefaults
// @0x57C050..0x57C065 — the same immediate seeds Env_CloudColorTarget and
// Env_LightningColor] divides to exactly (1, 1, 1); if it did not, every
// terrain light would be tinted by default. It is a factor, not a clamp: a
// white terrain colour exceeds unity.
inline constexpr uint32_t kTerrainFactorDefaultPacked = 0x808080u;
inline constexpr float kTerrainFactorDivisor = 128.0f;

inline std::array<float, 3> terrain_per_channel_factor(uint32_t packed_rgb) {
	return {
		static_cast<float>((packed_rgb >> 16) & 0xFFu) / kTerrainFactorDivisor,
		static_cast<float>((packed_rgb >> 8) & 0xFFu) / kTerrainFactorDivisor,
		static_cast<float>(packed_rgb & 0xFFu) / kTerrainFactorDivisor,
	};
}

// THE PROJECTED TEXTURE's SCALE. The normal pass projects with
// `32768.0 / record[+0x28]` [orig: Light_SetupTerrainProjectedPass
//  @0x5AA864..0x5AA873], and +0x28 is the record's Q16 range — the same dword
// Light_GetPointLightParams divides into pos.w = 65536 / range @0x5A91C8..0x5A91D4
// — so the scale is 0.5 / radius: the texture's 0..1 span covers exactly one
// diameter, centred by the +0.5 translation @0x5AA900/@0x5AA915. The alternate
// pass under render-mode bit 0x100 scales by 26214.4 / range instead, i.e.
// 0.4 / radius [orig: foliage_setup_render_matrices @0x6098A5]. A bigger light
// casts a WIDER pool, not a brighter one.
inline constexpr float kTerrainProjectScale = 0.5f;      // 32768 / 65536
inline constexpr float kTerrainProjectScaleAlt = 0.4f;   // 26214.4 / 65536

inline float terrain_project_scale(float radius, bool alt_pass = false) {
	if (radius <= 0.0f) return 0.0f;
	return (alt_pass ? kTerrainProjectScaleAlt : kTerrainProjectScale) / radius;
}

} // namespace opennova::renderer
