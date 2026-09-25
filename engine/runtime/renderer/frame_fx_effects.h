#pragma once

// FrameFX: retail's post-scene frame-effect system, as the portable planner
// the device executes. After the scene (and before the HUD) the main scene
// frame dispatches, in this order, the distortion pass (type 0), the death
// blur (type 4) or the damage blur (type 1), the bloom (type 2), then the
// thermal (8) and monitor (9) views; the first-person NVG view replaces the
// whole chain. Every pass is one DrawPass descriptor (the 52-byte block
// render_scar_decal_batch walks): a source texture, a target, a pixel stage
// with its blend, and one of four tap-geometry builders. The planner turns
// one frame's facts into those rows; the device (godot/src/render/frame_fx)
// runs them in order and evaluates the pixel stages this header also states
// as CPU references.
// [orig: Render_ProcessMainSceneFrame @0x5ca8f6..0x5caad5 (the dispatch);
//  Render_DispatchShadowByType @0x584440 (types 0-5) and sub_5845B0
//  @0x5845b0 (types 8/9) -- both are the FrameFX dispatchers despite their
//  names; render_scar_decal_batch @0x582ab0 (DrawPass);
//  CFrameFX_CreatePixelShaders @0x5821d0 (the pixel stages and blends)]

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace opennova::renderer {

// The FBEFFECTS level every gate reads (dword_24D2068). The reimpl runs the
// locked highest profile, FBEFFECTS 3 (runtime/menu/options_policy.h).
// [orig: FrameFX_QualityAtLeast3 @0x581f60]
inline constexpr int kLockedFrameEffectsLevel = 3;

// The two square work targets ([fx+4] / [fx+8], 256 x 256).
// [orig: create_frame_effect_render_targets @0x583cf7..0x583d04]
inline constexpr int kFrameFxWorkSide = 256;

// The capture target [fx+0] per axis: the highest power of two not above
// (backbuffer - 1), found by clearing the lowest set bit until one remains.
// A 1920 x 1080 frame captures at 1024 x 1024, 1024 x 768 at 512 x 512.
// [orig: create_frame_effect_render_targets @0x583c7f..0x583c97]
int frame_fx_capture_side(int backbuffer_side);

// DrawPass's degree-to-radian factor (the float the binary stores, not pi/180)
// and the U scale of every builder's `reduced` flag (+0x11).
// [orig: render_scar_decal_batch @0x582beb (flt_7D838C);
//  build_scar_decal_quad_vertices @0x581307 / scar_build_quad_vertices
//  @0x5815da / build_scar_decal_vertices_extended @0x581d2a (flt_7C3DC8)]
inline constexpr float kFrameFxDegreesToRadians = 0.017453279f;
inline constexpr float kFrameFxReducedU = 0.75f;

// The DrawPass sources and targets. Frame is the frame colour (retail's
// render target 0, the backbuffer); Scanlines is the 64 x 64 "ffscan"
// texture, a source only.
enum class FrameFxBuffer : std::uint8_t {
	Frame = 0,
	Capture = 1,
	WorkA = 2,
	WorkB = 3,
	Scanlines = 4,
};

// The pixel stages with the blend each state object carries (the ps.1.1
// text lives at the data address named per row).
// [orig: CFrameFX_CreatePixelShaders @0x5821d0 -- each state's store:
//  LumaAverage [fx+10h] ps 0x7D8230, blend off @0x58234c;
//  AverageBlend [fx+14h] ps 0x7D8170, SRCALPHA/INVSRCALPHA @0x582373;
//  AverageAdd [fx+20h] ps 0x7D8170, SRCALPHA/ONE @0x58251c;
//  WeightedAdd [fx+24h] ps 0x7D7F00, ONE/ONE @0x58253b;
//  FanAverage [fx+28h] ps 0x7D7E28, SRCALPHA/INVSRCALPHA @0x5825f1;
//  Thermal [fx+2Ch] ps 0x7D7C48, blend off @0x582840;
//  Scanline [fx+30h] fixed function MODULATE2X(TEXTURE, DIFFUSE),
//  DESTCOLOR/SRCCOLOR @0x582888]
enum class FrameFxStage : std::uint8_t {
	LumaAverage = 0,
	AverageBlend = 1,
	AverageAdd = 2,
	WeightedAdd = 3,
	FanAverage = 4,
	Thermal = 5,
	Scanline = 6,
};

// The four tap-geometry builders, by DrawPass type.
// [orig: render_scar_decal_batch @0x582be8..0x582d3e]
enum class FrameFxTaps : std::uint8_t {
	// build_scar_decal_quad_vertices @0x581300: four taps at the radius about
	// the texel, (+s,+c), (+c,-s), (-s,-c), (-c,+s) for (s, c) = radius x
	// (sin a, cos a), the U parts scaled by the reduced factor.
	Rotated = 1,
	// scar_build_quad_vertices @0x5815d0: four taps along (s, c) at 0.5, 2.5,
	// 4.5 and 6.5 radii.
	Weighted = 2,
	// build_scar_decal_vertices_extended @0x581d20: a six-vertex fan about the
	// rect centre (see frame_fx_fan_*).
	RadialFan = 3,
	// build_tiled_decal_quad_vertices @0x5819c0: a 256 x 64-pixel tiling of the
	// source with a per-draw texel offset.
	Tiled = 4,
};

// One DrawPass descriptor. The offsets are the 52-byte block's.
// [orig: render_scar_decal_batch @0x582ab0]
struct FrameFxPass {
	FrameFxBuffer source = FrameFxBuffer::Capture;  // +0x00
	FrameFxBuffer target = FrameFxBuffer::Frame;    // +0x0C (0 = the backbuffer)
	FrameFxStage stage = FrameFxStage::LumaAverage; // +0x14
	FrameFxTaps taps = FrameFxTaps::Rotated;        // +0x18
	bool viewport_rect = false; // +0x10: the viewport rect, else the 256-square work rect
	bool reduced_u = false;     // +0x11
	float base = 0.0f;          // +0x08: the UV offset every vertex carries
	// Replaces `base` with half a texel of the capture's smaller side
	// (0.5 / [fx+98h]); the thermal view's only [orig: sub_584390 @0x5843b0..0x5843d3].
	bool base_from_capture = false;
	float radius = 0.0f;               // +0x28
	std::int32_t angle_degrees = 0;    // +0x1C
	std::int32_t angle_step_degrees = 0; // +0x20
	std::int32_t count = 1;            // +0x24
	float constant_alpha = 0.0f;       // pixel constant c0.a (SetPixelShaderConstantF)
	std::uint32_t vertex_color = 0;    // +0x2C (the Tiled builder)
	std::int32_t tile_offset_x = 0;    // Tiled: rand() & 0xFF
	std::int32_t tile_offset_y = 0;    // Tiled: rand() & 0x3E
};

// The two distortion draw sets the type-0 row executes with texture slot 2
// bound: the effect world's distortion particles on WorkA, then the tracer
// pool's distortion ribbons on WorkB.
// [orig: render_projected_shadow @0x5838f8 (EffectWorld_DrawParticles(4)
//  through the misnamed CNapiSession_SetViewMatrix, slot 2 = [fx+4]),
//  @0x583928 (CEffectEmitterPool_RenderDistortionPass, slot 2 = [fx+8])]
enum class FrameFxDistortionSet : std::uint8_t {
	Particles = 0,
	TracerRibbons = 1,
};

enum class FrameFxStepKind : std::uint8_t {
	// IDirect3DDevice9::StretchRect(render target 0 -> [fx+0], LINEAR).
	// [orig: FrameFX_CaptureBackBufferAndSubmitDecal @0x583dc0 (@0x583ebb)]
	CaptureFrame = 0,
	Pass = 1,
	Distortion = 2,
};

struct FrameFxStep {
	FrameFxStepKind kind = FrameFxStepKind::Pass;
	FrameFxPass pass;                                  // Pass
	FrameFxDistortionSet set = FrameFxDistortionSet::Particles; // Distortion
	FrameFxBuffer screen_texture = FrameFxBuffer::WorkA;         // Distortion: slot 2
};

// The frame's view facts the dispatch reads.
// [orig: Render_ProcessMainSceneFrame @0x5ca8f6..0x5caad5]
struct FrameFxViewInputs {
	bool in_session = false;          // g_napi_np_ctx.is_in_session
	bool local_dead = false;          // local player Flags & 2 ([ent+24h] & 2)
	std::int32_t red_word = 0;        // g_screenFlashRedDamage, the raw word
	int camera_mode = 0;              // g_camera_mode
	// tick - g_camera_lerp_start_tick (the death stamp), as the int32 retail
	// subtracts.
	std::int32_t death_elapsed_ticks = 0;
	bool thermal_view = false;        // the frame's latch: CanFire && flags2 & 4
	bool monitor_view = false;        // the frame's latch: CanFire && flags2 & 8
	bool nvg_active = false;          // g_NVGActive
	bool death_screen_active = false; // g_death_screen_active
	bool binoculars_view_active = false; // g_binocularsViewActive
	// The frame's Scoped selector byte: CanFire, a Scoped non-Inset def, not a
	// vehicle-attack seat, never on the death screen. [orig: @0x5ca2be..0x5ca304]
	bool scoped_selector = false;
	// The frame's Sighted selector byte under the same gates.
	// [orig: @0x5ca2cc..0x5ca2d5, cleared @0x5ca2ff..0x5ca304]
	bool sighted_selector = false;
};

struct FrameFxFrameInputs {
	FrameFxViewInputs view;
	int frame_effects_level = kLockedFrameEffectsLevel;
	// CEffectEmitterPool_HasDistortionChannels @0x5db7f0 ||
	// CNapiSession_HasActiveDataTransfer @0x5f6640 (the effect world's
	// distortion-particle test): the type-0 row's content gate.
	bool distortion_present = false;
	// GetTickCount(), the type-0 jitter's clock (any millisecond clock: only
	// its low bits reach the angle).
	std::uint32_t clock_ms = 0;
};

// The first-person NVG view. `scene` renders the 512-square NVG scene and
// accumulates its glow; `composite` replaces the frame with the tint + glow
// composite and skips every other FrameFX row; `lens` (the Scoped arm, off
// the death screen and the binoculars) draws that composite through the
// scoped lens instead (renderer/nvg_scope_lens.h) over a scene rendered with
// the square scoped frustum; `sighted` (the Sighted arm, after the Scoped
// one) renders the scene at 80 / zoom without the viewmodel and draws the
// SIGHTS card INTO it, laid out over the 512 square, so the card is tinted
// and glows with the scene and no card draws over the composite;
// `clear_glow` clears the persistent glow target to green on the frame
// g_NVGActive changed.
// [orig: Render_ProcessMainSceneFrame @0x5ca516..0x5ca5cd (the scene arms --
//  the Scoped one sub_5D2990 @0x5ca575, the Sighted one
//  Math_BuildScaledFixedPointToFloatMatrix @0x5ca591 -- and the dword_29D6BA4
//  latch), @0x5ca6ab..0x5ca73a (the composite, the Scoped arm's sub_5D2B10
//  @0x5ca71a, the Sighted arm's j_render_fullscreen_overlay @0x5ca72b, and the
//  jump past every FrameFX dispatch to @0x5cab1d); the card into the scene
//  terrain_scene_render @0x5d08cb..0x5d0952]
struct FrameFxNvgPlan {
	bool scene = false;
	bool composite = false;
	bool lens = false;
	bool sighted = false;
	bool clear_glow = false;
};

// The frame's NVG arms without the toggle latch (plan_frame_fx adds
// `clear_glow`).
FrameFxNvgPlan frame_fx_nvg_view(const FrameFxViewInputs &view);

// NVG.tga and its gain scale draw at the end of the full-screen composite,
// never on the death screen, and never under the lens (whose arm skips that
// composite). [orig: render_fullscreen_overlay @0x5d1077..0x5d1080
//  (sub_5CFF70 unless g_death_screen_active)]
bool frame_fx_nvg_mask_visible(const FrameFxViewInputs &view);

struct FrameFxFramePlan {
	FrameFxNvgPlan nvg;
	std::vector<FrameFxStep> before_bloom; // type 0, then type 4 or type 1
	bool bloom = false;                    // type 2 (frame_fx_bloom_passes)
	std::vector<FrameFxStep> after_bloom;  // type 8, then type 9
};

// The planner's one piece of cross-frame state: last frame's g_NVGActive
// (dword_29D6BA4), written every frame. [orig: @0x5ca5cd]
struct FrameFxPlannerState {
	bool nvg_active_last = false;
};

// One CRT rand() draw (0..0x7FFF). The live caller passes the render stream's
// crt::crt_rand15; tests pass a script.
using FrameFxRand = std::function<std::uint16_t()>;

// The frame's plan. Draws the type-0 jitter and each Tiled pass's offsets
// from `rand` in retail order.
FrameFxFramePlan plan_frame_fx(const FrameFxFrameInputs &in, FrameFxPlannerState &state,
		const FrameFxRand &rand);

// The bloom's rows after the Q3 altbuffer capture: the 256A downsample, the
// two weighted pairs and the half-strength composite.
// [orig: FrameFX_CaptureRenderTarget @0x584165..0x5841c0; sub_5841D0 @0x584248..0x58437a]
std::vector<FrameFxPass> frame_fx_bloom_passes();

// --- CPU references of the pixel stages ---------------------------------------

using FrameFxRgb = std::array<float, 3>;

// LumaAverage's alpha: the average's (0.20, 0.30, 0.10) luma, squared
// (ps text 0x7D8230).
inline constexpr FrameFxRgb kFrameFxLumaWeights = {0.20f, 0.30f, 0.10f};
// WeightedAdd's four tap weights at 0.5 / 2.5 / 4.5 / 6.5 radii (ps text 0x7D7F00).
inline constexpr std::array<float, 4> kFrameFxWeightedTapWeights = {0.50f, 0.46f, 0.35f, 0.19f};
inline constexpr std::array<float, 4> kFrameFxWeightedTapSteps = {0.5f, 2.5f, 4.5f, 6.5f};
// The fan's four tap distances. [orig: build_scar_decal_vertices_extended @0x581d20]
inline constexpr std::array<float, 4> kFrameFxFanTapSteps = {0.5f, 1.5f, 2.5f, 3.5f};

// Thermal: L = dot(1 - average, (0.30, 0.60, 0.10)); out = (L^2, L + 0.05, L^2)
// (ps text 0x7D7C48: c0 and c2; c1 is unused).
inline constexpr FrameFxRgb kFrameFxThermalLuma = {0.30f, 0.60f, 0.10f};
inline constexpr float kFrameFxThermalGreenBias = 0.05f;
FrameFxRgb frame_fx_thermal_color(const FrameFxRgb &average);

// The radial fan at rect-normalised (s, t): its interpolated alpha
// m = 2 max(|s - 1/2|, |t - 1/2|) and tap k's UV
// (s, t) + m base + steps[k] radius (2 u (1/2 - s), 2 (1/2 - t)), u the
// reduced U scale. The centre keeps the source, the border takes the taps.
float frame_fx_fan_alpha(float s, float t);
std::array<float, 2> frame_fx_fan_tap(float s, float t, int k, float radius, float base,
		bool reduced_u);

// The "ffscan" 64 x 64 scanline texture: even rows 0x60, odd rows
// 0x80 + ((rand() >> 2) % 24) per texel, replicated into all four channels;
// 2048 draws in row-major order. The Tiled pass maps it at 256 x 64 pixels
// per repeat under MODULATE2X with diffuse 0x808080 and DESTCOLOR/SRCCOLOR:
// out = 2 sat(2 texel x 128/255) x dst.
// [orig: CFrameFX_CreatePixelShaders @0x58289f..0x582912; sub_583A60 @0x583a60]
inline constexpr int kFrameFxScanlineSide = 64;
inline constexpr int kFrameFxScanlineTileWidth = 256;
inline constexpr int kFrameFxScanlineTileHeight = 64;
inline constexpr std::uint32_t kFrameFxScanlineDiffuse = 0x808080u;
std::vector<std::uint8_t> frame_fx_scanline_texels(const FrameFxRand &rand);

// --- the NVG view -------------------------------------------------------------

// The NVG targets: the scene renders into a 512-square target and the glow
// accumulates in a persistent 256-square one.
// [orig: sub_5CF400 @0x5cf422..0x5cf42c (0x200 x 0x200 on pixel-shader
//  hardware); init_water_reflection_render_targets @0x5cf665..0x5cf690 (the
//  glow at half that)]
inline constexpr int kNvgSceneSide = 512;
inline constexpr int kNvgGlowSide = 256;

// Tint: d = dot(scene, (0.5, 1.0, 0.5)); out = d (0.2, 0.9, 0.2) + (0, 0.2, 0),
// replacing the frame. [orig: init_view_effect_shaders_and_textures @0x5cfab6
// (ps text 0x7DC3D8, state dword_2BDFAB8)]
inline constexpr FrameFxRgb kNvgTintDot = {0.50f, 1.00f, 0.50f};
inline constexpr FrameFxRgb kNvgTintScale = {0.20f, 0.90f, 0.20f};
inline constexpr FrameFxRgb kNvgTintBias = {0.00f, 0.20f, 0.00f};
FrameFxRgb nvg_tint_color(const FrameFxRgb &scene);

// Glow source: g = sat(dot(sum of the four taps squared, (0.30, 0.60, 0.10))
// - 0.15); out = g (0.2, 0.6, 0.2) at alpha 0.16, blended ONE/INVSRCALPHA
// into the persistent target twice a frame: diagonal taps at +-1.5/512, then
// axial taps at +-3/512. The composite adds it MODULATE2X by 0x808080.
// [orig: init_view_effect_shaders_and_textures @0x5cfb60 (ps text 0x7DC228,
//  state dword_2BDFABC); render_water_caustic_overlay @0x5d032e..0x5d0453;
//  render_fullscreen_overlay @0x5d0f90..0x5d1059]
inline constexpr FrameFxRgb kNvgGlowLuma = {0.30f, 0.60f, 0.10f};
inline constexpr float kNvgGlowThreshold = 0.15f;
inline constexpr FrameFxRgb kNvgGlowScale = {0.20f, 0.60f, 0.20f};
inline constexpr float kNvgGlowAlpha = 0.16f;
inline constexpr float kNvgGlowTapStep = 1.5f;
inline constexpr std::uint32_t kNvgGlowClearColor = 0x0000FF00u; // ARGB: green, alpha 0
FrameFxRgb nvg_glow_source(const std::array<FrameFxRgb, 4> &taps);

} // namespace opennova::renderer
