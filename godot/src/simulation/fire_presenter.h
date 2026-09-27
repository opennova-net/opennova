#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include <runtime/renderer/scene_overlay.h>
#include <runtime/renderer/tracer_frame.h>
#include <runtime/world/nvg_laser.h>

#include "resource_index/resource_root.h"
#include "simulation/present_event_records.h"
#include "simulation/present_stats.h"

namespace godot {

class EffectLightDirector;
class EffectWorld;
class EntityPresenter;
class MissionAudio;
class MissionEnvironment;
class Simulation;
struct SceneOverlaySubmission;

// One NVG laser candidate (Simulation::nvg_laser_sources): a decoded person
// row, its seat attach bone, its held weapon definition (entity+0x298) and
// whether it is the local player, plus that definition's action point (its
// +0x2D4 userpoint, 1-based on its third-person model).
struct NvgLaserSource {
	int handle = -1;
	opennova::world::NvgLaserGate gate;
	int launch_userpoint = 0;
};

// The frame the beams draw for: the eye (Godot space), the projection's _11,
// the millisecond clock, the scene fog the beams fold, and the local view's
// g_NVGActive and g_camera_mode.
struct NvgLaserView {
	Transform3D eye;
	float projection_x_scale = 1.0f;
	std::uint32_t tick_ms = 0;
	opennova::renderer::SceneOverlayFog fog;
	bool nvg_active = false;
	int camera_mode = 0;
	// The weapon Inset pass's walk: the persons that view draws (its own
	// collect's verdicts), in its own overlay slot.
	bool inset_view = false;
};

// THE viewing-client fire-presentation pass (the former fire_present_pass.gd,
// ADR 0043 d9), an owned member of EntityPresenter: presents the sim's
// authoritative host rounds or decoded visual-only joiner rounds —
// AI/remote-player fire sound, muzzle effect, and in-flight tracers. The
// local player's own predicted fire keeps its action-slot presentation
// (PlayerWeaponEffects._fire_action_effects) and is self-filtered here,
// exactly like the entity presenter's wire walk filters the local avatar.
// A joiner re-runs decoded S2C tag-2 rounds through the same visual RoundSim,
// so it must drain this queue too [orig: remote tag-2 receive ->
// RoundData_SpawnRound; net-re §5.60].
//
// [orig: WeaponSlot_FireAndSpawnEffects @ 0x53F440 — on the firing host every AI
// anim-event fire plays the ammo-def 'ai_launch' sound set (+64) through
// Sound_PlayWithDistanceAttenuation @ 0x528E40 and spawns the 'ai_launcheffect'
// muzzle effect (+68) through CEffectWorld_SpawnEmitterAtPosition @ 0x5F6DF0 at the
// fire origin along the fire direction; remote clients re-fire the same ring records
// (net-re §5.60). Our sim records each spawn (RoundSim.fired) and this pass drains it
// once per logic tick — the same records, one presentation per shot.]
//
// Sound distance model [orig: Sound_PlayWithDistanceAttenuation @ 0x528E40]: a shot
// heard from >= 30 u arrives LATE by the witnessed propagation delay
// (62 * dist / 330) >> 2 ticks. The gate and the pending-sound slot pool
// [orig: Sound_TickPendingSlots @ 0x529310] live in the SIM on the logic clock
// (world/fire_sound.h, S12a): the runtime stamps the camera listener each frame,
// the sim gates at fire time and counts down per logic tick, and this pass just
// plays whatever drain_fire_sounds() returns — including the adm-arm action-row
// sounds, which retail plays immediately at the shooter with no delay. The set's
// max range still culls at PLAY time in our bank vs fire time in retail (the
// tracked D-AI-8 delta). The MF_Light muzzle glow leg (+36 ->
// Entity_UpdateMuzzleGlowEffect @ 0x56C960) routes to the EffectLightDirector's
// light pool per presented fire (on_muzzle_fire).
//
// Tracers [orig: RoundData_SpawnRound @ 0x4EC0D0]: every tracer_rate-th round per
// shooter (forcetracer 0x8000 = every round) is visible in flight — a channel in the
// tracer/trail emitter pool [orig: g_TracerEmitterPool @ 0x2BF5270], styled by the
// ammo 'tracer_type <friendly> [enemy]' id selected against the LOCAL player's team
// at spawn (@ 0x4ec740; shooter == local player counts friendly — our sim runs the
// same select, world/tracer_trails.h). The sim owns the point rings (one pre-move
// point per 62 Hz tick, drain after death); the witnessed style tables and the
// ribbon build [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0] live in
// engine/runtime/renderer/tracer_frame.{h,cpp} (pinned by the renderer_tracer_frame
// ctest); this pass compiles the trail rows through it against the render camera
// and uploads one surface per run of same-material channel draws, in pool order,
// on the ladder rung of the frame's tracer slot (tracer_rung: the far-side slot
// while the eye is below the water). The three materials are the witnessed ones
// (godot/shaders/tracer_ribbon_{stock,smoke,nvg}.gdshader), each fogged per its
// style's +0 word. The round's item graphic (frndlyTrcrID/foeTrcrID +
// TRACER_SCALE/TRACER_WIDTH nodes) is D-AI-12d; the light_move glow (round+0x1B4)
// rides the light pool (Simulation::fill_round_glows ->
// EffectLightDirector::sync_round_glows).
// The SP host shows tracers unconditionally (the MP NoTracers rules bit,
// dword_24D1E34 & 1, is a net seam wired via RoundSim.no_tracers_rule).
class FirePresenter {
public:
	// The SndProf.def body slots that refire every body tick (chute flap /
	// freefall); the exclusive key folds the refires into one voice (D-SND-10).
	static constexpr int SLOT_CHUTE_FLAP = 43;
	static constexpr int SLOT_FREEFALL = 44;

	explicit FirePresenter(EntityPresenter *p_owner);

	// `container` hosts the tracer MeshInstance3D (no container: no ribbon
	// geometry). `audio` (nullable) gates the sound legs, `fx` (nullable) the
	// effect legs, `lights` (nullable) the MF_Light muzzle glow. The muzzle
	// anchor for retail's adm-arm fire effect is the owner's
	// muzzle_world_for: the entity presenter owns the per-handle held-weapon
	// node the effect spawns at; a shooter with no wire body resolves to a
	// non-finite anchor and the pass keeps the wire position. The ribbons
	// face the owner viewport's current camera (no camera, no ribbons);
	// `resource_root` supplies the smoke texture and `environment` (nullable)
	// the eye's water side.
	void setup(Simulation *p_sim, Node3D *p_container, MissionAudio *p_audio,
			EffectWorld *p_fx, EffectLightDirector *p_lights,
			const Ref<ResourceRoot> &p_resource_root, MissionEnvironment *p_environment);
	void teardown();

	// Once per present (beside the other passes), after the sim advanced. The
	// pending-sound countdown consumes logic ticks inside the sim now
	// (world/fire_sound.h) — this pass only drains and plays. Each drain forwards
	// into a public data leg over the engine rows (the present_snapshot
	// precedent): EntityPresenter's bound legs feed the same rows from tests
	// without a live sim.
	void present();
	void present_slot_sounds(const std::vector<opennova::world::SoundSlotEvent> &p_events);
	void present_sound_emitters(const std::vector<opennova::world::SoundEmitterEvent> &p_events);
	void present_fires(const std::vector<opennova::world::FirePresentationRow> &p_events);
	void present_fire_sounds(const std::vector<opennova::world::ReadyFireSound> &p_sounds);
	void draw_tracer_rows(const PackedFloat32Array &p_rows);

	// The NVG laser beams into the overlay tail's NvgLaserBeams slot: every
	// source through the engine gate, its action point read off the owner's
	// drawn third-person weapon (the gun draw 5 places with the same matrix
	// the beam's transform builds), the ray clip from the sim (unclipped
	// without one), the tracer NVG style's ribbon and the pool+0x3008 combine
	// (world/nvg_laser.h and renderer/scene_overlay.h carry the witnesses).
	// Returns the beams drawn.
	int append_nvg_laser_beams(const std::vector<NvgLaserSource> &p_sources,
			const NvgLaserView &p_view, SceneOverlaySubmission &r_submission);

	// Load-time pipeline warm: emit one zero-area surface on each ribbon
	// material at `position` (must be in frustum so the surfaces actually draw)
	// so their pipelines compile behind the loading screen instead of as a
	// ~40 ms draw stall on the first tracer. The next present's clear_surfaces
	// drops the warm surfaces.
	void warm_pipelines(const Vector3 &p_position);

	// Typed diagnostic counters (ADR 0017: cross-object contracts are typed
	// records) — probes assert the presentation legs actually ran.
	Ref<FirePresentStats> get_stats() const;
	// The ribbon geometry — the ADR 0018 read seam for tests asserting the rebuilt
	// tracer surfaces (surface count, vertex layout, material) without private
	// reach-ins.
	Ref<ArrayMesh> ribbon_mesh() const { return mesh_; }

private:
	Simulation *sim() const;
	MissionAudio *audio() const;
	EffectWorld *fx() const;
	EffectLightDirector *lights() const;
	MissionEnvironment *environment() const;
	void free_mesh_instance();
	Ref<ShaderMaterial> ribbon_material(opennova::renderer::TracerShader p_shader,
			bool p_fog_black);
	Ref<Texture2D> smoke_texture();
	void emit_surface(const opennova::renderer::TracerRibbonFrame &p_frame,
			std::size_t p_first_draw, std::size_t p_end_draw, int p_rung);

	EntityPresenter *owner_ = nullptr;
	ObjectID sim_id_;    // drain + trail source (null in data-driven tests)
	ObjectID audio_id_;  // MissionAudio (or null)
	ObjectID fx_id_;     // EffectWorld (or null)
	ObjectID lights_id_; // EffectLightDirector: the MF_Light muzzle glow route (or null)
	ObjectID environment_id_; // MissionEnvironment: the eye's water side (or null)
	Ref<ResourceRoot> resource_root_;
	Ref<ArrayMesh> mesh_;
	ObjectID mesh_instance_id_;
	// One material per (normal-pass shader, fog-black) pair, created on first use.
	std::array<Ref<ShaderMaterial>, 6> materials_;
	// smoktest.pcx, the pool+0x3000 texture
	// [orig: CEffectEmitterPool_CreateShaders @ 0x5DC8F0 (the store @ 0x5dc926)].
	Ref<Texture2D> smoke_texture_;
	bool smoke_texture_loaded_ = false;
	opennova::renderer::TracerRibbonFrame frame_;
	opennova::renderer::TracerRibbonFrame distortion_frame_;
	opennova::renderer::TracerRibbonFrame laser_frame_; // one beam's ribbon, reused
	std::vector<opennova::renderer::TracerChannelInput> channels_;
	int64_t stat_fires_ = 0;
	int64_t stat_sounds_ = 0;
	int64_t stat_effects_ = 0;
	int64_t stat_tracer_peak_ = 0;
};

} // namespace godot
