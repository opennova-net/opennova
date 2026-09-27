#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include <base/io/fixed.h>
#include <formats/env/env.h> // Vec3

// Celestial-side render state math: sun glare, the celestial body alphas,
// glare occlusion, and the sky-dome constants + mesh builder.

namespace opennova::env {

// ---------------------------------------------------------------------------
// Sun glare [orig: Environment_ComputeSunGlareAndFogBlend @ 0x5ad610]
//           [orig: Render_SkyboxSunGlow @ 0x5acd00]

struct GlareResult {
	int glare = 0;      // 0..255 additive glare intensity
	int fog_whiten = 0; // 0..40 fog-color whitening
};

// view_dot_sun is the cosine between the view direction and the sun; values
// below 0 yield no glare. occlusion_brightness is the hysteresis state 0..255.
GlareResult compute_sun_glare(float view_dot_sun, int occlusion_brightness);

// The sun-glare screen veil + exposure stop-down pair — the exact integer
// chain of [orig: Environment_ComputeSunGlareAndFogBlend @ 0x5ad610], consumed once
// per main scene frame by [orig: Environment_ApplySunVeilAndExposureStopdown
// @ 0x5ad8b0 (ex sub_5AD8B0)]: `glare` (dot^32 x 192 x brightness/256, SunDim
// and overcast folds) is the ALPHA byte of a fullscreen WHITE quad drawn over
// the scene when > 2 ((glare << 24) + 0xFFFFFF -> Render_FullscreenDecalQuad
// @ 0x5ad94f), and `stopdown` (dot^128 x 40 x brightness/256, same folds) is
// the modulator-2 exposure stop-down input (ModulatorChain::
// set_sun_veil_stopdown). compute_sun_glare above is the earlier float
// summary of the same curve, kept for the EnvFile tooling static.
struct SunVeil {
	int glare = 0;    // 0..255 fullscreen white veil alpha byte (draw gate > 2)
	int stopdown = 0; // 0..40 modulator-2 exposure stop-down input
};

// view_dot_fixed is dot(view_forward, sun_direction) in 16.16;
// occlusion_brightness the hysteresis state 0..256; sun_dim/overcast in 16.16.
SunVeil sun_veil_from_dot(int view_dot_fixed, int occlusion_brightness,
                          int sun_dim_fixed, int overcast_blend_fixed);

// When water exists the frame's veil is the sky term plus the water-reflected
// secondary term, summed per channel with both sums clamped 192
// [orig: Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8f3..0x5ad916].
SunVeil sun_veil_combine(const SunVeil &primary, const SunVeil &secondary);

// The fullscreen white-quad draw gate: only a glare above 2 submits the quad
// [orig: @ 0x5ad928; the quad color is (glare << 24) + 0xFFFFFF @ 0x5ad931].
bool sun_veil_draws(int glare);

// Occlusion hysteresis: target = 32 * visible_rays (0..8 rays), brightness
// moves +-16 per frame toward it, clamped 0..255.
int glare_brightness_step(int current, int target);

// ---------------------------------------------------------------------------
// Celestial bodies + glare occlusion — witnessed at the ENG-2 celestial leg
// [orig: Render_CelestialBodies @ 0x5acaa0 (the LIVE sun/moon renderer,
//  was misnamed render_skybox_fog_layers); Render_SkyboxSunGlow @ 0x5acd00].
//  The old Render_SkyboxLayers @ 0x5ac230 is a caller-less dead variant (its
//  +64/+16 fixed offsets never run).

// Sun/moon bodies place at camera + direction * 64 world units, identity
// rotation, FULL camera height [orig: @ 0x5acaa0, constant flt_7C3DD0].
inline constexpr float kCelestialBodyDistance = 64.0f;

// Body alphas, 16.16 in/out like the originals:
// sun = clamp((1 - overcast) * ((0x640000 - sun_dim) / 100)), where
// sun_dim is the 0..100 (16.16) Env_SunDimPct channel (default 0, no .env
// parser writes it) [orig: @ 0x5acbc1..0x5acbfa].
int celestial_sun_alpha_fixed(int overcast_blend_fixed, int sun_dim_fixed);

// moon = clamp01((fogDistInt - 400) / 600) * (1 - overcast)  (the no-fog-
// shader path; the fog-shader path scales fogDistInt * 0.0002)
// [orig: @ 0x5acc40..0x5acccd].
int celestial_moon_alpha_fixed(float fog_distance_world, int overcast_blend_fixed,
                               bool fog_shader_path);

// ---------------------------------------------------------------------------
// Stars: retail draws none. The star 3DI loads (EffectWorld_LoadCelestialModels
// @ 0x5adc50) and its instance table regenerates per load
// [orig: Star_GenerateInstanceTable @ 0x5ac850], but the only renderer that
// reads the table has no caller in the image (no code xref, no rel32 call, no
// absolute pointer) [orig: Star_RenderField_Unused @ 0x5ad9c0], so nothing is ported.

// Glare occlusion (env #14) [orig: Render_SkyboxSunGlow @ 0x5acd9e..0x5acf7f]:
// TWO jittered rays per frame feed an 8-bit SLIDING window (>>1 per sample,
// bit 0x80 = sample visible), so the window spans the last 4 frames. The ray
// is camera -> camera + sun_dir * 1024 world units, jittered per sample from
// the frame index bits: engine-Y +-16 (bit 0) and +-8 (bit 2), height +-16
// (bit 1). Brightness steps +-16 (dead-band hold) toward
// popcount(window) * 32 * (fog_distance / 1000)  (flt_7DA0C4 = 1/65536000).
struct GlareOcclusionState {
	uint8_t window = 0;        // g_GlareOcclusionWindow @ 0x27E2E34
	int brightness = 0;        // g_GlareOcclusionBrightness @ 0x27E2E30
	uint32_t jitter_index = 0; // g_GlareJitterFrameIndex @ 0x27E5690
};

// The coarse (unjittered) glare gate ray's start-height lift
// [orig: Render_SkyboxSunGlow @ 0x5acde4 — start.z = camera_z + 1.0 +
// 0.5 * (g_GlareJitterFrameIndex & 3) world units; the two jittered fine rays
// start at the exact camera height (@ 0x5ace37)].
inline float glare_coarse_start_lift(uint32_t frame_index) {
	return 1.0f + 0.5f * static_cast<float>(frame_index & 3u);
}

// ---------------------------------------------------------------------------
// The water-reflected sun glint [orig: Environment_UpdateSunGlare @ 0x5ad130, once per
// scene pass from Terrain_RenderWorldScene @ 0x5c96c0 — the main scene's,
// then, while it renders, the weapon Inset pass's over its own camera, both
// on this one accumulator (runtime/renderer/scene_overlay.h
// kInsetOverlayOrder)]: its own 4-bit visibility window (dword_27E2E2C, >> 1
// per call, bit 3 (value 8) = visible) and +-16 brightness chase toward
// popcount * 64 (no dead-band, no fog scale — dword_27E2E28). The settled
// brightness draws the glare model mirrored below the eye (camera + sun * 128
// with the HEIGHT term negated) and, right-shifted 2, feeds the sun veil's
// secondary term, which the frame computes after both passes
// [orig: Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8dc..0x5ad916 —
// both veil sums clamp 192; its call @ 0x5cac4b follows the Inset's
// @ 0x5ca949 in Render_ProcessMainSceneFrame].
struct WaterGlintState {
	uint8_t window = 0;        // dword_27E2E2C
	int brightness = 0;        // dword_27E2E28
	uint32_t frame_index = 0;  // dword_27E5694
};

// One frame: advance the frame index, shift the window, mark visibility,
// step the brightness [orig: @ 0x5ad1cd..0x5ad356].
void water_glint_tick(WaterGlintState &state, bool visible);

// The reflected-sun point on the water plane [orig: Water_ComputeReflectedSunPoint @0x5ac040 — reflect the
// camera about the water height (view_z_jitter = 0.25 * (frame & 3), the
// caller's per-frame lift), then interpolate along sun * 2048 to the
// surface]. Mission axes (x, y ground plane, z = height), world units.
// Returns false when the geometry rejects (clip >= water height — no glint).
// The caller then jitters the point x/z by +-2 from the frame index bits
// [orig: @ 0x5ad26a..0x5ad27e] before the visibility rays.
bool water_glint_point(const Vec3 &cam_mission, const Vec3 &sun_mission,
                       float water_height, float view_z_jitter,
                       Vec3 &out_mission);

// The glint submit alpha, 16.16 [orig: @ 0x5ad395..0x5ad41c]: (dot^4 -
// 28672/65536) x brightness >> 8, the SunDim fold, >> 2; clamped [0, 1].
// No overcast fold (unlike the sky glow).
int water_glint_alpha_fixed(int view_dot_fixed, int brightness,
                            int sun_dim_fixed);

struct GlareRayJitter {
	float offset_eng_y = 0.0f; // mission Y (north; Godot -z)
	float offset_eng_z = 0.0f; // mission Z (height; Godot +y)
};

// The jitter offsets for one sample index [orig: @ 0x5ace3b..0x5ace61].
GlareRayJitter glare_ray_jitter(uint32_t jitter_index);

// One frame: advances the window with the two samples' visibility and steps
// the brightness. The embedder casts the two rays (sample indices jitter_index+1
// and jitter_index+2 BEFORE the call).
void glare_occlusion_tick(GlareOcclusionState &state, bool visible_a, bool visible_b,
                          float fog_distance_world);

// The glow submit alpha, 16.16: dot_view^4 / 2 scaled by the occlusion
// brightness (>> 8), the overcast blend and the SunDim fold
// [orig: @ 0x5acfb8..0x5ad0a9]. frame_effects_quarter applies the
// FBEFFECTS >= 3 quarter [orig: FrameFX_QualityAtLeast3 @ 0x581f60 reads the quality
// level; glow_intensity >>= 2 @ 0x5ad033..0x5ad03c] - the bloom pass
// re-adds the glare, so the highest-quality program dims the direct draw.
// The locked reimpl profile IS FBEFFECTS 3, so live callers pass true.
int glare_glow_alpha_fixed(int view_dot_fixed, int brightness, int overcast_blend_fixed,
                           int sun_dim_fixed, bool frame_effects_quarter);

// The BLOOM-SOURCE (Q3) glow alpha, 16.16: FrameFX_RenderGlowSource calls
// Render_SkyboxSunGlow(0, 0) - no occlusion test - so the Q3 draw uses the
// fog-based brightness (fog_km + 1) * 0.5 * dot_factor instead of the
// occlusion accumulator [orig: @ 0x5ad013..0x5ad027; flt_7C3280 = 1.0,
// flt_7C3B94 = 0.5, flt_7DA0C4 = 1/65536000], then the same quarter and
// overcast x SunDim fold. The glare therefore blooms even when terrain
// blocks the occlusion rays - the wide sunrise glow over a ridge.
int glare_q3_alpha_fixed(int view_dot_fixed, float fog_distance_world,
                         int overcast_blend_fixed, int sun_dim_fixed,
                         bool frame_effects_quarter);

// The 21x21 sky dome: 441 vertices / 800 triangles per pass
// [orig: Render_Skybox draw @ 0x5798dc].
inline constexpr int kSkyDomeVertices = 441;
inline constexpr int kSkyDomeTriangles = 800;

// The dome profile is a sphere cap of radius 3072 lowered so the rim
// (radius 1024 = 20 rows x 51.2) sits at y = 0: 3072^2 - 1024^2 = 2^23, so
// y(r) = sqrt(3072^2 - r^2) - sqrt(2^23) and the apex reference height is
// 3072 - sqrt(2^23) ~= 175.6906 — the "175.69" the height scale divides by
// [orig: SkyDome_BuildMesh @ 0x578ed4 — v14 = skyHeight / (3072.0 - sqrt(8388608.0))].
inline const double kSkyDomeReferenceHeight = 3072.0 - std::sqrt(8388608.0);

// The sky dome mesh [orig: SkyDome_BuildMesh @ 0x578db0] — 21 rings x 21
// columns, FVF 0x212 (XYZ|NORMAL|TEX2, stride 40). Per vertex: radius =
// row * 51.2, theta = col * pi/10, x = sin(theta)*radius, z = cos(theta)*radius,
// y = v14 * (sqrt(3072^2 - radius^2) - sqrt(2^23)) — the Y-only height scale is
// BAKED into the mesh (retail rebuilds on smoothed-height change, gated at
// [orig: Environment_ApplyFogAndAmbient @ 0x57e4f4] -> Terrain_PushSkyDomeHeightFloat
// @ 0x610920 -> SkyDome_SetHeightAndRebuild @ 0x579070; the reimpl folds the
// scale into the vertex shader instead — env #20's ratified structure, so it
// builds once at the reference height). UV1 = (x, z) * 0.003125, UV2 =
// (x, z) * 3/2048. Normal = normalize(x, y_scaled / v14^2, z) — the builder's
// anisotropic normal, NOT the vertex direction [orig: @ 0x578fbb..0x579023];
// zero-length input degenerates to (0,0,0) [orig: @ 0x578fd8]. All math runs
// in double off the binary's float32 literal seeds (51.2f, 0.31415927f,
// 9437184.0f, 0.003125f, 3/2048; 8388608.0 is a double literal — x87
// intermediates approximated as double, stored float32 like the D3D vertex
// buffer). Index winding per quad: (i, i+22, i+21), (i, i+1, i+22)
// [orig: @ 0x578e00..0x578e86].
struct SkyDomeMesh {
	std::vector<float> positions; // xyz triples, kSkyDomeVertices
	std::vector<float> normals;   // xyz triples (anisotropic dome normals)
	std::vector<float> uv1;       // uv pairs, layer 1 (1/320 world scale)
	std::vector<float> uv2;       // uv pairs, layer 2 (3/2048 world scale)
	std::vector<int32_t> indices; // 3 * kSkyDomeTriangles, witnessed winding
};

SkyDomeMesh build_sky_dome_mesh(float sky_height);

} // namespace opennova::env
