#pragma once

// game.cfg's `particle_density` (D-RMAT-24). The Options' PARTICLES rows edit
// the word (0 low, 1 medium, 2 high); each mission start copies it with the
// session's settings, and the copy reaches the draw two ways: the particle
// atlas pages are halved once at 1 or less (renderer/particle_atlas.h), and
// every scene pass draws only one particle in a stride of its emitter's
// particles, the stride set from the copy before the pass draws.
// [orig: Config_ParseSettingsLine @ 0x550D74 (game.cfg +0x124);
//  apply_session_settings_to_globals @ 0x551565..0x551574 (the copy's +0x18,
//  g_SessionParticleDensity @ 0x24D2058); the Accepts write
//  g_CfgParticleDensity @ 0x25507DC only (sub_55A710 @ 0x55A905..0x55A926,
//  UI_IngameOptionsDialogEventHandler @ 0x554F14..0x554F35)]

#include <cstdint>

namespace opennova::renderer {

// The config load's clamp, and the Options rows' rungs.
// [orig: Settings_ClampGraphicsOptions @ 0x54D54E..0x54D564]
inline constexpr int kParticleDensityMin = 0;
inline constexpr int kParticleDensityMax = 2;

inline constexpr int clamp_particle_density(int32_t density) {
	return density < kParticleDensityMin   ? kParticleDensityMin
	       : density > kParticleDensityMax ? kParticleDensityMax
	                                       : static_cast<int>(density);
}

// The word a fresh profile starts at: the first-launch video test writes
// quality_levels[6] = 1, raised to 2 on a CPU above 2980 MHz (lowered to 0 on a
// card without pixel shaders under 1980 MHz, which OpenNova's device never
// is), the preset offset 0. The reference machine's CPU is past the mark, and
// the Steam JO:CA install's own game.cfg carries 2.
// [orig: Renderer_ComputeQualityLevels @ 0x586242..0x58625D (caps +0x0, the
//  CPU mark from sub_587790 @ 0x5877F5), @ 0x5862F0..0x586301 (the clamp)]
inline constexpr int kParticleDensityFreshProfile = 2;

// The page halving: 0x10000 at a density of 1 or less, one
// GTexture_Downsample2x2_RGBA8 halving before the page's levels are made.
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5E82C4..0x5E82CC;
//  GTexture_DownsampleToLimits @ 0x6871DB]
inline constexpr uint32_t particle_page_density_flags(int32_t session_density) {
	return session_density <= 1 ? 0x10000u : 0u;
}

// Which scene routine a view's particles draw in. The main scene, the weapon
// Inset pass and the cinematic views set the same scale; the NVG scene sets
// half of it.
enum class ParticleScenePass : uint8_t { Main, NvgScene };

// The density scale a scene pass hands the effect world before it draws:
// (0x10000 >> (2 - density)) / 65536, so 1, 0.5 and 0.25 at density 2, 1 and
// 0; the NVG scene multiplies by 1/131072 instead, half that. The word is the
// load's clamp, so the shift stays in 0..2.
// [orig: Render_ProcessMainSceneFrame @ 0x5CA8B8..0x5CA8DE; Render_WeaponInsetScene
//  @ 0x5C9DB3..0x5C9DD9; the cinematic views @ 0x5706E5..0x57070B and
//  @ 0x5708A0..0x5708C6 (flt_7C3310 = 1/65536); NVG_RenderSceneToTarget
//  @ 0x5D088F..0x5D08B5 (flt_7DC620 = 1/131072); EffectWorld_SetParticleDensityScale
//  @ 0x5F6DB0 (g_EffectWorld +0x3F4)]
inline constexpr float particle_density_scale(int32_t session_density, ParticleScenePass pass) {
	const int density = clamp_particle_density(session_density);
	const float fixed = static_cast<float>(0x10000 >> (kParticleDensityMax - density));
	return pass == ParticleScenePass::NvgScene ? fixed * (1.0f / 131072.0f)
	                                           : fixed * (1.0f / 65536.0f);
}

// The effect world's scale before any scene pass has set it.
// [orig: CParticleManager_InitRenderDefaultsAndBaseSystem @ 0x5E8730..0x5E8737]
inline constexpr float kParticleDensityScaleInitial = 1.0f;

// The stride a billboard builder draws with under `scale`: the reciprocal,
// truncated. A particle draws only when its serial byte modulo the stride is
// 0; a stride of 1 draws every particle. The simulation never reads it.
// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5E6DA2..0x5E6E06;
//  CParticleEmitter_RenderStaticBillboards @ 0x5F4E82..0x5F4EC2;
//  CParticleEmitter_RenderTopAlignedBillboards @ 0x5F5681..0x5F56E6]
inline uint32_t particle_lod_divisor(float scale) {
	if (!(scale > 0.0f)) return 1u;
	const float stride = 1.0f / scale;
	return stride >= 1.0f ? static_cast<uint32_t>(stride) : 1u;
}

inline uint32_t particle_scene_lod_divisor(int32_t session_density, ParticleScenePass pass) {
	return particle_lod_divisor(particle_density_scale(session_density, pass));
}

// One frame's strides by view. The main view draws in the main scene's stride,
// or the NVG scene's while the world renders as that scene (the NVG arm skips
// the main scene and the Inset pass after it); the weapon Inset pass draws in
// the main scene's; the water mirror draws before any pass of its frame sets
// the scale, so in the stride the previous frame's last pass left, which is
// the main view's (the Inset pass sets the same scale after it).
// [orig: Render_ProcessMainSceneFrame @ 0x5CA504 (Render_TerrainScene ->
//  Water_ReflectionPrerender ahead of every set), @ 0x5CA6AB..0x5CA73A (the NVG
//  arm jumps past the main scene), @ 0x5CA8B8..0x5CA8DE, @ 0x5CA949 (the Inset
//  pass); Render_WeaponInsetScene @ 0x5C9DB3..0x5C9DD9; NVG_RenderSceneToTarget
//  @ 0x5D088F..0x5D08B5]
struct ParticleFrameStrides {
	uint32_t main_view = 1;
	uint32_t inset = 1;
	uint32_t mirror = 1;
	// What this frame's last pass leaves for the next frame's mirror.
	uint32_t last_pass = 1;
};

inline ParticleFrameStrides particle_frame_strides(int32_t session_density, bool main_view_nvg_scene,
		uint32_t previous_last_pass) {
	ParticleFrameStrides strides;
	strides.inset = particle_scene_lod_divisor(session_density, ParticleScenePass::Main);
	strides.main_view = main_view_nvg_scene
	                            ? particle_scene_lod_divisor(session_density, ParticleScenePass::NvgScene)
	                            : strides.inset;
	strides.mirror = previous_last_pass == 0 ? 1u : previous_last_pass;
	strides.last_pass = strides.main_view;
	return strides;
}

// Whether a particle of serial byte `serial` draws under `divisor`.
inline constexpr bool particle_lod_draws(uint8_t serial, uint32_t divisor) {
	return divisor <= 1u || (serial % divisor) == 0u;
}

} // namespace opennova::renderer
