#pragma once

// The post-particle overlay tail of the retail scene frame, as pure data: the
// draws Terrain_RenderWorldScene issues after particle pass B, in
// their witnessed order, and the typed draw list an embedding renderer
// executes after its own pass-B composite and before the frame effects (the
// bloom and the screen quads). No Godot dependencies.
//
// [orig: Terrain_RenderWorldScene @ 0x5c93a0 — particle pass B
// @ 0x5c9690, then Render_NVGLaserBeamsForVisiblePersons (the NVG laser beams) @ 0x5c9695,
// Render_WeatherTrailParticles @ 0x5c96a6, EffectWorld_RenderLightCoronas(1)
// @ 0x5c96ad, Environment_UpdateSunGlare (the water glint; only while the water height
// is nonzero, @ 0x5c96b5) @ 0x5c96c0, the underwater murk quad
// Render_DrawViewportColorQuad @ 0x5c96f5, and the sun glare
// Render_SkyboxSunGlow(1, 1) @ 0x5c9714; the frame effects follow in
// Render_ProcessMainSceneFrame (FrameFX_QualityAtLeast3 @ 0x5caa7b ->
// FrameFX_ApplyScreenEffect @ 0x5caa97, the bloom)].

#include <runtime/environment/precipitation.h>
#include <runtime/renderer/light_scene.h>
#include <runtime/renderer/precipitation_frame.h>
#include <runtime/renderer/texture_filter.h>
#include <runtime/renderer/tracer_frame.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

enum class SceneOverlaySlot : uint8_t {
	NvgLaserBeams = 0,
	Precipitation = 1,
	LightCoronas = 2,
	WaterGlint = 3,
	UnderwaterMurk = 4,
	SunGlare = 5,
	// The water mirror's closing draws (kMirrorOverlayOrder).
	MirrorDim = 6,
	MirrorCelestialBodies = 7,
	MirrorSunGlow = 8,
	// The weapon Inset pass's own draws of the tail (kInsetOverlayOrder).
	InsetLightCoronas = 9,
	InsetNvgLaserBeams = 10,
	InsetPrecipitation = 11,
	InsetWaterGlint = 12,
};

// The main scene's tail, in draw order. The scope's aperture view runs the
// same scene routine [orig: NVG_RenderSceneToTarget @ 0x5d08c3].
inline constexpr std::array<SceneOverlaySlot, 6> kSceneOverlayOrder = {
	SceneOverlaySlot::NvgLaserBeams,  // @ 0x5c9695
	SceneOverlaySlot::Precipitation,  // @ 0x5c96a6
	SceneOverlaySlot::LightCoronas,   // @ 0x5c96ad
	SceneOverlaySlot::WaterGlint,     // @ 0x5c96c0
	SceneOverlaySlot::UnderwaterMurk, // @ 0x5c96f5
	SceneOverlaySlot::SunGlare,       // @ 0x5c9714
};

// The weapon Inset pass's tail. Its scene core is the same routine
// [orig: Render_WeaponInsetScene @ 0x5c9740 -> Terrain_RenderWorldScene
// (view, 0, 0, 0) @ 0x5c9de9], so it draws the main tail in the main order,
// every draw its own over the Inset's camera, which the Inset composes and
// sets inside the pass right before its scene [orig: Camera_ComputeThirdPersonView
// @ 0x5c9841, Render_SetViewAndProjectionMatrices @ 0x5c997f]:
// - the NVG laser beams of the visible-person list the Inset's own collect
//   rebuilt [orig: @ 0x5c9695; Terrain_CollectVisibleEntities zeroes the
//   list @ 0x5c916b, Terrain_CollectVisibleEntitiesForTerrain appends
//   @ 0x5c8eb1..0x5c8ef9];
// - the precipitation drawer again at the Inset camera: its camera memory
//   (the velocity term) and the fall accumulator it zeroes are one state
//   across both passes' calls [orig: @ 0x5c96a6 -> Render_WeatherTrailParticles
//   @ 0x5dee65 (the pool update), @ 0x5dee74..0x5deed8 (the camera memory),
//   @ 0x5deede..0x5deef4 (the fall read and its zeroing)];
// - its own corona walk: EffectWorld_RenderLightCoronas(1) @ 0x5c96ad at the
//   corona phase the Inset's own effect-world prologue advanced (it runs once
//   per scene pass) [orig: EffectWorld_BeginScenePass @ 0x5a9f70 `add dword_2732DA0, 1`,
//   called @ 0x5ca69c by Render_ProcessMainSceneFrame and @ 0x5c9a5d by the
//   Inset], gating owned coronas on the section masks the Inset's own
//   collect wrote [orig: Terrain_CollectVisibleEntities @ 0x5c94f0;
//   Terrain_IsBuildingSectionBitSet @ 0x5c6960];
// - the water glint's leg again at the Inset camera, behind the same
//   water-height test: its frame counter, visibility window and brightness
//   are one accumulator across both passes' calls, and the glint model
//   draws at the Inset eye with the Inset's view dot [orig: @ 0x5c96b5,
//   @ 0x5c96c0 -> Environment_UpdateSunGlare @ 0x5ad1c8..0x5ad356 (the
//   accumulator), @ 0x5ad1ba..0x5ad213 (the placement), @ 0x5ad384..0x5ad41c
//   (the view dot)].
// Its second argument is zero, and the sun glare is gated on it, so the
// Inset draws no glare [orig: @ 0x5c96cd, the test @ 0x5c970a..0x5c970e skips
// Render_SkyboxSunGlow @ 0x5c9714]. The murk quad is the main tail's
// batch: each view gates it on its own eye (scene_overlay_view_draws).
inline constexpr std::array<SceneOverlaySlot, 5> kInsetOverlayOrder = {
	SceneOverlaySlot::InsetNvgLaserBeams,
	SceneOverlaySlot::InsetPrecipitation,
	SceneOverlaySlot::InsetLightCoronas,
	SceneOverlaySlot::InsetWaterGlint,
	SceneOverlaySlot::UnderwaterMurk,
};

// The water mirror's reflected scene draws only the coronas after its two
// particle passes and its tracer pass [orig: Water_RenderReflectedWorldScene
// @ 0x5c8510 — EffectWorld_RenderLightCoronas(1) @ 0x5c85fd]; the
// precipitation drawer's one caller is the main scene core
// [orig: Render_WeatherTrailParticles <- @ 0x5c96a6]. Render_MainScene
// then closes the mirror target: at water detail >= 2 the fullscreen dim
// (DESTCOLOR / ZERO under 0xFF404040 [orig: Render_MainScene @ 0x5c1727
// gate, @ 0x5c1856..0x5c189e]), and at FrameFX quality >= 3 the far depth
// band around the sun/moon redraw and the glow [orig: Render_MainScene
// @ 0x5c18c6 FrameFX_QualityAtLeast3 test, @ 0x5c18f4 Render_SetViewportFarDepth,
// @ 0x5c18fb Render_CelestialBodies(0), @ 0x5c1904 Render_SkyboxSunGlow(0, 0)].
// The locked profile runs both.
inline constexpr std::array<SceneOverlaySlot, 4> kMirrorOverlayOrder = {
	SceneOverlaySlot::LightCoronas,
	SceneOverlaySlot::MirrorDim,
	SceneOverlaySlot::MirrorCelestialBodies,
	SceneOverlaySlot::MirrorSunGlow,
};

// The fixed-function combine a batch draws with.
enum class SceneOverlayShading : uint8_t {
	// color MODULATE2X(texture, diffuse) saturating, alpha MODULATE;
	// SRCALPHA / INVSRCALPHA (the precipitation mode word 0x651).
	Modulate2xBlend = 0,
	// texture x diffuse added ONE / ONE; the diffuse carries the fade and the
	// fog-to-black fold already (the corona).
	AdditiveModulate = 1,
	// The FF_ST_AD_LUM SELFLUM specialization: tex.rgb x 2 x the diffuse
	// (= sat(SelfLumColor x gain)) saturating, fogged to black by the diffuse
	// alpha, added ONE / ONE (the glare model's materials).
	SelfLumAdditive = 2,
	// The untextured diffuse, SRCALPHA / INVSRCALPHA (the murk quad).
	FlatBlend = 3,
	// The untextured diffuse multiplied into the target, DESTCOLOR / ZERO
	// (the mirror dim).
	DimMultiply = 4,
	// The tracer pool's NVG laser material (pool+0x3008): one texture on both
	// stages, the second stage on the second coordinate set; colour = DIFFUSE,
	// alpha = DIFFUSE.a x (1 - T0.a) x (1 - T1.a), SRCALPHA / ONE; the
	// texture wraps. The diffuse carries the fog fold already.
	// [orig: CEffectEmitterPool_CreateShaders @ 0x5dc8f0 (the pool's
	//  0x3008 shader); the render-state layout RenderState_ApplyToDevice
	//  @ 0x681920]
	NvgLaser = 5,
};

enum class SceneOverlayDepth : uint8_t {
	TestNoWrite = 0, // ZFUNC LESSEQUAL, z-write off
	Always = 1,      // ZFUNC ALWAYS (submit flag 0x10, pass flag 0x200000)
	// ZFUNC LESSEQUAL inside the viewport depth band
	// [kQ3FarBandMinZ, kQ3FarBandMaxZ] (Render_SetViewportFarDepth), z-write off.
	FarBand = 2,
};

enum class SceneOverlayGeometry : uint8_t {
	World = 0,     // world-space positions (render frame) under the view projection
	Billboard = 1, // position = the centre, corner = the offset along the view's right/up
	Screen = 2,    // position.xy in normalized device coordinates (a viewport quad)
};

inline constexpr uint32_t kSceneOverlayNoTexture = 0xFFFFFFFFu;

// One vertex; positions are in the RENDER (Godot) frame: mission (x, y, z)
// maps to (x, z, -y). Colours are gamma-domain. `corner` is the billboard
// corner offset for Billboard geometry and the second texture coordinate set
// for World geometry (the ribbon's TEX2 vertex).
struct SceneOverlayVertex {
	float position[3] = {0.0f, 0.0f, 0.0f};
	float uv[2] = {0.0f, 0.0f};
	float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float corner[2] = {0.0f, 0.0f};
};

// One draw: a triangle list over the frame's vertices.
struct SceneOverlayBatch {
	SceneOverlaySlot slot = SceneOverlaySlot::Precipitation;
	SceneOverlayShading shading = SceneOverlayShading::Modulate2xBlend;
	SceneOverlayDepth depth = SceneOverlayDepth::TestNoWrite;
	SceneOverlayGeometry geometry = SceneOverlayGeometry::World;
	uint32_t texture = kSceneOverlayNoTexture; // the embedder's texture table index
	// The family its texture samples as (texture_filter.h stage_sampler): the
	// drops, the coronas and the NVG laser's smoke are fixed-function stages,
	// bilinear with point mips at every texfilter level; a SELFLUM surface is
	// a model stage through its effect. The untextured draws keep the default.
	TextureStage stage = TextureStage::ObjectStage;
	uint32_t first_vertex = 0;
	uint32_t vertex_count = 0;
	// The murk quad's side test, per view: drawn only while the view's eye is
	// at or below the water height (the scissor's cmp/jg @ 0x5c96ca..0x5c96d1
	// includes equality).
	bool eye_at_or_below_water_gate = false;
	float water_height = 0.0f;
};

// One frame's overlay geometry, in submission order.
struct SceneOverlayFrame {
	std::vector<SceneOverlayVertex> vertices;
	std::vector<SceneOverlayBatch> batches;

	void clear() {
		vertices.clear();
		batches.clear();
	}
};

// The draw list of one view: the frame's batches of the slots in `order`,
// slot by slot in that order, submission order within a slot.
void compile_scene_overlay(const SceneOverlayFrame &frame, const SceneOverlaySlot *order,
		std::size_t order_count, std::vector<SceneOverlayBatch> &out);

// Whether a view whose eye sits at `eye_height` draws this batch (the murk
// gate; every other batch draws in every view that admits its slot).
bool scene_overlay_view_draws(const SceneOverlayBatch &batch, float eye_height);

// The precipitation streaks: one triangle per drop in world space (the
// identity world matrix @ 0x5def50), the frame's one diffuse
// (g_EnvTerrainLightCombined | 0xFF000000), the pass state of the drawer
// [orig: Render_WeatherTrailParticles @ 0x5dee10 — mode word 0x651, pass
// flags 0x10500000: z-write off, z-test on, no fog]. `slot` names the view's
// call: the main scene's (Precipitation) or the weapon Inset pass's own
// (InsetPrecipitation).
void append_precipitation_overlay(const PrecipitationDrawFrame &precipitation,
		uint32_t texture, SceneOverlayFrame &out,
		SceneOverlaySlot slot = SceneOverlaySlot::Precipitation);

// The corona billboards: one camera-facing quad per segment, the
// premultiplied colour as its diffuse, depth-tested, no depth write
// [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40 — fog+blend mode 2
// @ 0x5aafb6]. `slot` names the walk: the main scene's (LightCoronas, which
// the water mirror draws too) or the weapon Inset pass's own
// (InsetLightCoronas).
void append_corona_overlay(const std::vector<LightCoronaQuad> &quads, uint32_t texture,
		SceneOverlayFrame &out, SceneOverlaySlot slot = SceneOverlaySlot::LightCoronas);

// The full-viewport murk quad: g_EnvWaterColorLit under the alpha byte
// 0x80 - ftol(murk x -96), source-over, ZFUNC ALWAYS (the scene passes the
// alternate pass state 0x300000) [orig: Terrain_RenderWorldScene
// @ 0x5c96d3..0x5c96f5 -> Render_DrawViewportColorQuad @ 0x5c38e0, pass flags
// @ 0x5c39bd..0x5c39c6].
void append_underwater_murk_overlay(const float rgb[3], uint8_t alpha_byte, float water_height,
		SceneOverlayFrame &out);

// One SELFLUM surface of a sky body (the glare, the water glint, the mirror's
// redrawn discs): a world-space triangle list, sat(SelfLumColor x light
// scale) per channel as its diffuse and the device fog visibility as its
// alpha. The main tail's glare and glint submit with 0x110, ZFUNC ALWAYS
// [orig: Render_SkyboxSunGlow @ 0x5ad0f7; Environment_UpdateSunGlare @ 0x5ad470];
// the mirror's redraws submit 0x100 inside the far band (`depth` FarBand)
// [orig: Render_SkyboxSunGlow @ 0x5ad10c; Render_CelestialBodies submits
// 0x100 @ 0x5acc1c / @ 0x5accdd]. The light scale is the unpacked modulator
// block the draw runs under: g_RenderLightScaleR/G/B (byte / 64 each,
// Render_UnpackModulatorToLightScale @ 0x58db30), the effect's
// ColorSrcGlobalGain [orig: Material_ApplyShaderParameters @ 0x58e05d].
void append_self_lum_overlay(SceneOverlaySlot slot, const float *positions, const float *uvs,
		std::size_t vertex_count, const float self_lum_rgb[3], const float light_scale_rgb[3],
		float fog_visibility, uint32_t texture, SceneOverlayFrame &out,
		SceneOverlayDepth depth = SceneOverlayDepth::Always);

// The device fog one overlay draw folds into its diffuse: the eye and look
// direction (Godot frame), the pass's resolved start/end/type
// (renderer/device_fog.h), and the colour a non-black fog mode fogs toward.
struct SceneOverlayFog {
	float eye[3] = {0.0f, 0.0f, 0.0f};
	float forward[3] = {0.0f, 0.0f, -1.0f};
	float start = 0.0f;
	float end = 0.0f;
	int type = 0;
	bool enabled = false;
	float color[3] = {0.0f, 0.0f, 0.0f};
};

// The NVG laser beams: every ribbon draw of `ribbons` (built by
// append_tracer_beam in the Godot frame) as one world-space triangle list in
// the NvgLaserBeams slot, the NvgLaser combine, depth-tested without writes;
// each vertex's diffuse is fogged at its own distance (exponential fog by the
// eye depth, the linear modes by the radial distance) toward black for the
// style's black-fog word, else toward the scene colour.
// [orig: Render_NVGLaserBeamsForVisiblePersons @ 0x5c63b0 from Terrain_RenderWorldScene
//  @ 0x5c9695 -> Entity_RenderNVGLaserBeam @ 0x5c6090 ->
//  Render_DrawTrailOrBeamSegments @ 0x5dcb80 (the ribbon pass flags
//  0x10520000: fog on, z-write off, z-test on)]. `slot` names the view's
// walk: the main scene's (NvgLaserBeams) or the weapon Inset pass's own
// (InsetNvgLaserBeams).
void append_nvg_laser_overlay(const TracerRibbonFrame &ribbons, uint32_t texture,
		const SceneOverlayFog &fog, SceneOverlayFrame &out,
		SceneOverlaySlot slot = SceneOverlaySlot::NvgLaserBeams);

// The mirror's dim: one viewport quad whose flat diffuse (`factor` on every
// channel, the 0xFF404040 vertex colour) multiplies the finished mirror
// target, DESTCOLOR / ZERO, ZFUNC ALWAYS [orig: Render_MainScene
// @ 0x5c1856..0x5c189e].
void append_mirror_dim_overlay(float factor, SceneOverlayFrame &out);

// The light scale the glare draws under, every channel: the scene forces the
// modulator block to 0xFF404040 (0x40 / 64 = 1.0) around the glow and
// restores g_EnvModulatorBlock after it [orig: Render_UnpackModulatorToLightScale
// @ 0x5c9702 and @ 0x5c9722]. The glint, drawn before that bracket
// (@ 0x5c96c0), runs under the frame's own modulator.
inline constexpr float kSunGlareLightScale = 1.0f;

} // namespace opennova::renderer
