#pragma once

// The particle frame's plan: which views compile which particle passes, in the
// order a frame runs them, and the render snapshot every pass compiles from.
// An embedder executes the plan's compiles in order through one
// ParticleFrameCompiler per slot (particle_frame.h); only its device state (a
// view's camera frame, projection and frustum, the GPU uploads) stays its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <runtime/environment/environment_state.h>
#include <runtime/particle/effect_scene.h>
#include <runtime/renderer/particle_atlas.h>
#include <runtime/renderer/particle_density.h>
#include <runtime/renderer/particle_frame.h>
#include <runtime/renderer/particle_quad_inputs.h>

namespace opennova::renderer {

// The compile slots: one ParticleFrameCompiler each, so every view keeps its
// own retained draw list.
enum class ParticleDrawSlot : std::uint8_t {
	WorldFarSide = 0,
	WorldCameraSide = 1,
	ReflectionFarSide = 2,
	ReflectionCameraSide = 3,
	SecondSceneFarSide = 4,
	SecondSceneCameraSide = 5,
	FirstPerson = 6,
	// The post-scene distortion pass (class-7 emitters, main view).
	Distortion = 7,
	// The weapon Inset pass's own water mirror.
	InsetMirrorFarSide = 8,
	InsetMirrorCameraSide = 9,
};
inline constexpr std::size_t kParticleDrawSlotCount = 10;

// The eye a compile runs for.
enum class ParticleFrameView : std::uint8_t {
	Main = 0,
	// The main scene's water mirror.
	Mirror = 1,
	// The weapon Inset pass (the second scene view).
	Inset = 2,
	// The weapon Inset pass's own water mirror.
	InsetMirror = 3,
};

// The frame's views as the embedder sees them this frame. Each side bit is the
// eye's water side as the embedder's one above/below-water rule decides it.
struct ParticleFrameViews {
	bool main_above_water = true;
	// A water mirror camera exists, and the frame renders its reflected scene.
	bool mirror = false;
	bool mirror_rendered = false;
	// The weapon Inset camera exists this frame, and its eye's water side.
	bool inset = false;
	bool inset_above_water = true;
	bool inset_mirror = false;
	bool inset_mirror_rendered = false;
	// The main scene's thermal byte (particle_thermal_material).
	bool thermal = false;
	ParticleFrameStrides strides{};
};

// One pass compile: the slot whose compiler runs, the eye and the view input's
// domain / water subset / stride, the material set it binds, and whether the
// snapshot's view-lit quads relight for this eye first (relight_particle_render_input).
struct ParticlePassCompile {
	ParticleDrawSlot slot = ParticleDrawSlot::WorldFarSide;
	ParticleFrameView view = ParticleFrameView::Main;
	ParticleRenderDomain domain = ParticleRenderDomain::World;
	ParticleWaterSubset water_subset = ParticleWaterSubset::All;
	bool thermal = false;
	std::uint32_t lod_divisor = 1;
	bool relight = false;
};

struct ParticleFramePlan {
	std::array<ParticlePassCompile, kParticleDrawSlotCount> compiles{};
	std::size_t compile_count = 0;
	// The slots no view compiles this frame: their submissions retire.
	std::array<ParticleDrawSlot, kParticleDrawSlotCount> retired{};
	std::size_t retired_count = 0;
};

// The frame's particle passes, in frame order:
// - the main scene's two world passes, the far side then the camera side
//   (particle_water_subset_for_side over the main eye's side; the scene's two
//   EffectWorld_RenderParticlePass calls [orig: Terrain_RenderWorldScene
//   @ 0x5c93a0, the calls @0x5c95b5 / @0x5c9690]), in the main view's stride
//   and under its thermal byte;
// - the post-scene distortion pass over the main eye: the class-7 children,
//   drawn only by FrameFX's type-0 row, which only the main scene runs
//   [orig: CParticleGroup_RenderChildren @ 0x5E5890 - the class compare
//   @0x5E58D2 and the `& 4` arm @0x5E58DB..0x5E58EE; FrameFX_DistortionPass
//   @ 0x583720 - its EffectWorld_RenderDistortionPass call @0x5838F8;
//   FrameFX_ApplyScreenEffect @ 0x584440 - the type-0 call @0x58459f; its one
//   caller Render_ProcessMainSceneFrame @ 0x5ca92e, after the main scene
//   @0x5ca8ec and ahead of the Inset @0x5ca949: the Inset and the mirror never
//   run it];
// - the first-person domain over the main eye (the scene's passes draw every
//   particle against the one viewport, particle_frame.h ParticleViewInput::frustum);
// - the main scene's water mirror pair, only on a frame that renders that
//   mirror (compiles_mirror_particles), on the main eye's side, relit for the
//   mirror's eye, in the mirror stride;
// - the weapon Inset pass's pair over its own eye's side, relit, in its stride
//   (the Inset re-runs the scene routine with its own eye);
// - the Inset's own mirror pair, likewise gated, relit, in its stride.
// The secondary views never pass the thermal byte: every scene call but the
// main scene's hands the effect world zero [orig: EffectWorld_RenderParticlePass
// @ 0x5F7274 - the thermal store]. A view pair the frame does not compile is
// retired.
ParticleFramePlan plan_particle_frame(const ParticleFrameViews &views);

// The distortion row's content gate: FrameFX's type-0 row draws the particle
// distortion pass only while some class-7 child of a visible group lives,
// ahead of the render submit's live-count and view tests, so the pass's
// emitters the submit left out count too
// [orig: EffectWorld_HasDistortionParticles @ 0x5F6640 - the group test
// @0x5e9848, the class compare @0x5e986b and its jz @0x5e9871: no live count,
// no view]. `counters` is the distortion slot's last compile.
bool particle_distortion_row_present(const ParticleFrameDebugCounters &counters);

// The catalog's per-definition visuals: each graphic layer's atlas entries by
// flipbook frame, and the ambient tint pick (particle_def_ambient_tinted).
inline constexpr int kParticleGraphicLayerCount = 4;
inline constexpr std::size_t kParticleMissingEntry = std::numeric_limits<std::size_t>::max();

struct ParticleLayerVisual {
	bool present = false;
	std::uint8_t type = 0;
	int flip_frames = 1;
	std::vector<std::size_t> frame_entries;
};

struct ParticleDefinitionVisual {
	std::array<ParticleLayerVisual, kParticleGraphicLayerCount> layers;
	bool ambient_lit = false;
};

// A Bump/Bumpadd quad's DIFFUSE depends on the view basis: the row another
// view's relight recomputes it from.
struct ParticleLitQuad {
	std::size_t particle_index = 0;
	float bump_scale = 0.0f;
	float roll = 0.0f;
	std::uint8_t alpha = 255;
};

// The frame's render snapshot (the emitters and their quad runs in the
// simulator's order) and its view-lit quads. clear() keeps capacity, so a
// steady frame allocates nothing.
struct ParticleRenderInput {
	ParticleFrameSnapshot snapshot;
	std::vector<ParticleLitQuad> lit_quads;
};

// Builds `out` from the simulator's frame for the main eye's view basis
// (row-major world-to-view). Per emitter: its domain (the group's render
// domain), its distortion class (particle_def_is_distortion_class) and its
// group's section gate; per particle: its graphic layer (a layer the
// definition lacks falls back to the first present one), the quad inputs
// (particle_quad_inputs), the pass its layer's type selects (type 7, Distort,
// draws in the distortion pass), and the atlas entry of its flipbook frame
// (no entry: the quad does not draw). The emitter's sort box takes each
// particle's base half-size. A definitions list the visuals do not match
// builds nothing.
void build_particle_render_input(const particle::ParticleFrameSnapshot &frame,
		const std::vector<ParticleDefinitionVisual> &visuals,
		const std::vector<ParticleAtlasEntry> &entries, const std::array<float, 9> &view_rows,
		const ParticleQuadTints &tints, ParticleRenderInput &out);

// Relights the view-lit quads for another eye's basis, each from its retained
// row and never from the colour it replaces.
void relight_particle_render_input(ParticleRenderInput &input,
		const std::array<float, 9> &view_rows);

// The fog and tints a particle frame runs under, from the frame's environment.
struct ParticlePassFog {
	std::array<float, 3> color{};
	float start = 0.0f;
	float end = 0.0f;
	std::int32_t type = 0;
};

struct ParticlePassEnvironment {
	// The scene's device fog: the eye's side picks the wet or the dry block
	// (EnvironmentState::build_scene_fog).
	ParticlePassFog fog{};
	// The water mirror pair's: the reflected scene runs under the dry block
	// whatever the main pass selects (EnvironmentState::build_water_mirror_fog).
	ParticlePassFog mirror_fog{};
	// The two tints the effect world refreshes every tick from the
	// environment, per-channel factors where retail byte 128 = 1.0:
	// AMBIENTCOLOR (and any blend-mode-0 graphic) draws through
	// g_EnvTerrainLightCombined; everything else through the modulator block
	// doubled and saturated at 255 [orig: Render_EmitterEffect @ 0x5f70c0
	// (world+0x3E8 / +0x3F0); CParticleEmitter_AdvanceFrame @ 0x5e6600..0x5e661c].
	ParticleQuadTints tints{};
};

// The frame's particle environment from the mission's environment state. The
// particle passes set no fog of their own: they inherit the scene's mission
// fog [orig: Terrain_RenderWorldScene @ 0x5c949f - Environment_ApplyFogAndAmbient
// ahead of the passes], and retail has no default fog block (its fog globals
// are zero-initialized data). With no environment (a preview) the passes run
// unfogged (end 0) under neutral tints.
ParticlePassEnvironment particle_pass_environment(const env::EnvironmentState *state,
		bool underwater_view);

} // namespace opennova::renderer
