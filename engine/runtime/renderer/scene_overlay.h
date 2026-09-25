#pragma once

// The post-particle overlay tail of the retail scene frame, as pure data: the
// draws Terrain_RenderSceneWithReflection issues after particle pass B, in
// their witnessed order, and the typed draw list an embedding renderer
// executes after its own pass-B composite and before the frame effects (the
// bloom and the screen quads). No Godot dependencies.
//
// [orig: Terrain_RenderSceneWithReflection @ 0x5c93a0 — particle pass B
// @ 0x5c9690, then sub_5C63B0 (the NVG laser beams) @ 0x5c9695,
// render_weather_trail_particles @ 0x5c96a6, EffectWorld_RenderLightCoronas(1)
// @ 0x5c96ad, update_sun_glare (the water glint; only while the water height
// is nonzero, @ 0x5c96b5) @ 0x5c96c0, the underwater murk quad
// Terrain_DrawScissorRect @ 0x5c96f5, and the sun glare
// render_skybox_sun_glow(1, 1) @ 0x5c9714; the frame effects follow in
// Render_ProcessMainSceneFrame (FrameFX_QualityAtLeast3 @ 0x5caa7b ->
// Render_DispatchShadowByType @ 0x5caa97, the bloom)].

#include <runtime/environment/precipitation.h>
#include <runtime/renderer/light_scene.h>
#include <runtime/renderer/precipitation_frame.h>

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
};

// The main scene's tail, in draw order. The scope's aperture view runs the
// same scene routine [orig: terrain_scene_render @ 0x5d08c3].
inline constexpr std::array<SceneOverlaySlot, 6> kSceneOverlayOrder = {
	SceneOverlaySlot::NvgLaserBeams,  // @ 0x5c9695
	SceneOverlaySlot::Precipitation,  // @ 0x5c96a6
	SceneOverlaySlot::LightCoronas,   // @ 0x5c96ad
	SceneOverlaySlot::WaterGlint,     // @ 0x5c96c0
	SceneOverlaySlot::UnderwaterMurk, // @ 0x5c96f5
	SceneOverlaySlot::SunGlare,       // @ 0x5c9714
};

// The water mirror's reflected scene draws only the coronas after its two
// particle passes and its tracer pass [orig: Water_RenderReflectedWorldScene
// @ 0x5c8510 — EffectWorld_RenderLightCoronas(1) @ 0x5c85fd]; the
// precipitation drawer's one caller is the main scene core
// [orig: render_weather_trail_particles <- @ 0x5c96a6]. render_main_scene
// then closes the mirror target: at water detail >= 2 the fullscreen dim
// (DESTCOLOR / ZERO under 0xFF404040 [orig: render_main_scene @ 0x5c1727
// gate, @ 0x5c1856..0x5c189e]), and at FrameFX quality >= 3 the far depth
// band around the sun/moon redraw and the glow [orig: render_main_scene
// @ 0x5c18c6 FrameFX_QualityAtLeast3 test, @ 0x5c18f4 Render_SetViewportFarDepth,
// @ 0x5c18fb render_celestial_bodies(0), @ 0x5c1904 render_skybox_sun_glow(0, 0)].
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
// maps to (x, z, -y). Colours are gamma-domain.
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
// (Env_TerrainLightCombined | 0xFF000000), the pass state of the drawer
// [orig: render_weather_trail_particles @ 0x5dee10 — mode word 0x651, pass
// flags 0x10500000: z-write off, z-test on, no fog].
void append_precipitation_overlay(const PrecipitationDrawFrame &precipitation,
		uint32_t texture, SceneOverlayFrame &out);

// The corona billboards: one camera-facing quad per segment, the
// premultiplied colour as its diffuse, depth-tested, no depth write
// [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40 — fog+blend mode 2
// @ 0x5aafb6].
void append_corona_overlay(const std::vector<LightCoronaQuad> &quads, uint32_t texture,
		SceneOverlayFrame &out);

// The full-viewport murk quad: Env_WaterColorLit under the alpha byte
// 0x80 - ftol(murk x -96), source-over, ZFUNC ALWAYS (the scene passes the
// alternate pass state 0x300000) [orig: Terrain_RenderSceneWithReflection
// @ 0x5c96d3..0x5c96f5 -> Terrain_DrawScissorRect @ 0x5c38e0, pass flags
// @ 0x5c39bd..0x5c39c6].
void append_underwater_murk_overlay(const float rgb[3], uint8_t alpha_byte, float water_height,
		SceneOverlayFrame &out);

// One SELFLUM surface of a sky body (the glare, the water glint, the mirror's
// redrawn discs): a world-space triangle list, sat(SelfLumColor x light
// scale) per channel as its diffuse and the device fog visibility as its
// alpha. The main tail's glare and glint submit with 0x110, ZFUNC ALWAYS
// [orig: render_skybox_sun_glow @ 0x5ad0f7; update_sun_glare @ 0x5ad470];
// the mirror's redraws submit 0x100 inside the far band (`depth` FarBand)
// [orig: render_skybox_sun_glow @ 0x5ad10c; render_celestial_bodies submits
// 0x100 @ 0x5acc1c / @ 0x5accdd]. The light scale is the unpacked modulator
// block the draw runs under: Render_LightScaleR/G/B (byte / 64 each,
// Render_UnpackModulatorToLightScale @ 0x58db30), the effect's
// ColorSrcGlobalGain [orig: apply_shader_parameters @ 0x58e05d].
void append_self_lum_overlay(SceneOverlaySlot slot, const float *positions, const float *uvs,
		std::size_t vertex_count, const float self_lum_rgb[3], const float light_scale_rgb[3],
		float fog_visibility, uint32_t texture, SceneOverlayFrame &out,
		SceneOverlayDepth depth = SceneOverlayDepth::Always);

// The mirror's dim: one viewport quad whose flat diffuse (`factor` on every
// channel, the 0xFF404040 vertex colour) multiplies the finished mirror
// target, DESTCOLOR / ZERO, ZFUNC ALWAYS [orig: render_main_scene
// @ 0x5c1856..0x5c189e].
void append_mirror_dim_overlay(float factor, SceneOverlayFrame &out);

// The light scale the glare draws under, every channel: the scene forces the
// modulator block to 0xFF404040 (0x40 / 64 = 1.0) around the glow and
// restores Env_ModulatorBlock after it [orig: Render_UnpackModulatorToLightScale
// @ 0x5c9702 and @ 0x5c9722]. The glint, drawn before that bracket
// (@ 0x5c96c0), runs under the frame's own modulator.
inline constexpr float kSunGlareLightScale = 1.0f;

} // namespace opennova::renderer
