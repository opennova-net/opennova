#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

// Celestial-side render state math: sun glare, the celestial body alphas,
// the star field, glare occlusion, and the sky-dome constants + mesh
// builder. Split from the retired env_render.h umbrella (W3-7).

namespace opennova::env {

// ---------------------------------------------------------------------------
// Sun glare [orig: compute_sun_glare_and_fog_blend @ 0x5ad610]
//           [orig: render_skybox_sun_glow @ 0x5acd00]

struct GlareResult {
	int glare = 0;      // 0..255 additive glare intensity
	int fog_whiten = 0; // 0..40 fog-color whitening
};

// view_dot_sun is the cosine between the view direction and the sun; values
// below 0 yield no glare. occlusion_brightness is the hysteresis state 0..255.
GlareResult compute_sun_glare(float view_dot_sun, int occlusion_brightness);

// Occlusion hysteresis: target = 32 * visible_rays (0..8 rays), brightness
// moves +-16 per frame toward it, clamped 0..255.
int glare_brightness_step(int current, int target);

// ---------------------------------------------------------------------------
// Celestial bodies + glare occlusion — witnessed at the ENG-2 celestial leg
// [orig: render_celestial_bodies @ 0x5acaa0 (the LIVE sun/moon renderer,
//  was misnamed render_skybox_fog_layers); render_skybox_sun_glow @ 0x5acd00;
//  render_star_field @ 0x5ad9c0]. The old render_skybox_layers @ 0x5ac230 is
//  a caller-less dead variant (its +64/+16 fixed offsets never run).

// Sun/moon bodies place at camera + direction * 64 world units, identity
// rotation, FULL camera height [orig: @ 0x5acaa0, constant flt_7C3DD0].
inline constexpr float kCelestialBodyDistance = 64.0f;

// Body alphas, 16.16 in/out like the originals:
// sun = clamp((1 - overcast) * ((0x640000 - sun_dim + 1) / 100)), where
// sun_dim is the 0..100 (16.16) Env_SunDimPct channel (default 0, no .env
// parser writes it) [orig: @ 0x5acbc1..0x5acbfa].
int celestial_sun_alpha_fixed(int overcast_blend_fixed, int sun_dim_fixed);

// moon = clamp01((fogDistInt - 400) / 600) * (1 - overcast)  (the no-fog-
// shader path; the fog-shader path scales fogDistInt * 0.0002)
// [orig: @ 0x5acc40..0x5acccd].
int celestial_moon_alpha_fixed(float fog_distance_world, int overcast_blend_fixed,
                               bool fog_shader_path);

// ---------------------------------------------------------------------------
// Star field (env #33) [orig: Star_GenerateInstanceTable @ 0x5ac850 (ex kong
// misnomer init_weather_particles); render_star_field @ 0x5ad9c0]. 256
// camera-anchored billboard instances regenerated per celestial load; per
// render tick each visible star's brightness accumulator EMA-chases its
// twinkle band. Engine axes throughout (x, y ground plane, z up), 16.16.

inline constexpr int kStarInstanceCount = 256;

// The star/weather PRNG: state = rol4(state + rol11(state), 4) ^ 1, low 16
// bits returned [orig: inlined at both sites; the dead standalone step is
// Star_TwinklePrngNext_unused @ 0x5ac010, state Star_TwinklePrng @ 0x840B38].
// Like the water noise field, the retail table content depends on the shared
// state's call history at load — a deterministic reimpl documents its seed.
uint32_t star_prng_next(uint32_t &state);

struct StarInstance {
	int32_t offset_fp[3] = { 0, 0, 0 };  // camera-relative offset, 16.16
	int32_t billboard_param = 12288;     // 12288..13311 (4096-fixed scale)
	int32_t twinkle_add = 1;             // 1..255, clamped to 255 - mask
	int32_t twinkle_mask = 31;           // 31 >> (r & 3): {31, 15, 7, 3}
	int32_t brightness = 0;              // runtime accumulator (BSS-zero at generate)
	int32_t dir_fp[3] = { 0, 0, 0 };     // normalize(offset >> 8), 16.16
};

// Fills out[0..255] with the witnessed per-star generation [orig: @ 0x5ac850]:
// offX/offY = (r - 0x8000) << 9; offZ = ((r + 0x20000) << 6) -
// ((|offX| + |offY|) >> 3) (the dome shaping); billboard = (r & 0x3FF) +
// 12288; add = r & 0xFF (0 -> 1, <= 255 - mask); mask = 31 >> (r & 3);
// brightness untouched; dir = normalize(off >> 8) via the 2^32/len + 0x8000
// rounding divide.
void generate_star_instances(StarInstance *out, uint32_t &prng_state);

// One render-tick twinkle update: brightness = (brightness + add +
// (r16 & mask)) >> 1; returns the new brightness [orig: @ 0x5adb45].
int32_t star_twinkle_tick(StarInstance &star, uint32_t &prng_state);

// The near-light cull: HIDDEN when dot(light_dir_fixed, star_dir) > 64225
// (16.16 ~0.98) [orig: @ 0x5adac4]. light_dir is the direct near-unit active
// light direction in engine axes.
bool star_visible_fixed(const StarInstance &star, const int32_t light_dir_fp[3]);

// Glare occlusion (env #14) [orig: render_skybox_sun_glow @ 0x5acd9e..0x5acf7f]:
// TWO jittered rays per frame feed an 8-bit SLIDING window (>>1 per sample,
// bit 0x80 = sample visible), so the window spans the last 4 frames. The ray
// is camera -> camera + sun_dir * 1024 world units, jittered per sample from
// the frame index bits: engine-Y +-16 (bit 0) and +-8 (bit 2), height +-16
// (bit 1). Brightness steps +-16 (dead-band hold) toward
// popcount(window) * 32 * (fog_distance / 1000)  (flt_7DA0C4 = 1/65536000).
struct GlareOcclusionState {
	uint8_t window = 0;        // Glare_OcclusionWindow @ 0x27E2E34
	int brightness = 0;        // Glare_OcclusionBrightness @ 0x27E2E30
	uint32_t jitter_index = 0; // Glare_JitterFrameIndex @ 0x27E5690
};

struct GlareRayJitter {
	float offset_eng_y = 0.0f; // engine Y axis (render/Godot -x)
	float offset_eng_z = 0.0f; // engine Z (height, render/Godot +y)
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
// [orig: @ 0x5acfb8..0x5ad0a9].
int glare_glow_alpha_fixed(int view_dot_fixed, int brightness, int overcast_blend_fixed,
                           int sun_dim_fixed);

// ---------------------------------------------------------------------------
// Celestial + sky constants
// [orig: render_skybox @ 0x57960e] sun/moon dome position = camera + dir * 2000
// [orig: render_skybox_layers @ 0x5ac230] layer offsets +64/+16, alpha 0x2000,
// additive submit flag 0x110; glare/star models load under mode 0x300000.
// NOTE (celestial-leg re-grill 2026-07-06): 2000 is the DOME VS clip-space
// proximity reference distance ONLY [orig: c14 upload @ 0x57960e] — the
// bodies place at kCelestialBodyDistance (64). The +16/+64 offsets and the
// 0x2000 alpha belong to the caller-less dead variant @ 0x5ac230.
inline constexpr float kCelestialDomeDistance = 2000.0f;
inline constexpr float kCelestialLayerAlpha = float(0x2000) / 65536.0f; // dead variant

// The 21x21 sky dome: 441 vertices / 800 triangles per pass
// [orig: render_skybox draw @ 0x5798dc].
inline constexpr int kSkyDomeVertices = 441;
inline constexpr int kSkyDomeTriangles = 800;

// The dome profile is a sphere cap of radius 3072 lowered so the rim
// (radius 1024 = 20 rows x 51.2) sits at y = 0: 3072^2 - 1024^2 = 2^23, so
// y(r) = sqrt(3072^2 - r^2) - sqrt(2^23) and the apex reference height is
// 3072 - sqrt(2^23) ~= 175.6906 — the "175.69" the height scale divides by
// [orig: build_sky_dome_mesh @ 0x578ed4 — v14 = skyHeight / (3072.0 - sqrt(8388608.0))].
inline const double kSkyDomeReferenceHeight = 3072.0 - std::sqrt(8388608.0);

// The sky dome mesh [orig: build_sky_dome_mesh @ 0x578db0] — 21 rings x 21
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
