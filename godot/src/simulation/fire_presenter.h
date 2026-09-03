#pragma once

#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <vector>

#include "simulation/present_event_records.h"
#include "simulation/present_stats.h"

namespace godot {

class EffectLightDirector;
class EffectWorld;
class EntityPresenter;
class MissionAudio;
class Simulation;

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
// camera-facing ribbon build [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0] live in
// engine/runtime/renderer/tracer_frame.{h,cpp} (pinned by the renderer_tracer_frame
// ctest); this pass compiles the trail rows through it and uploads the per-family
// strips. Additive styles (std/rapid/
// sniper/df1/NVG) draw fog-to-black additive [orig: SetFogAndBlendMode mode 2 —
// ONE:ONE, alpha unused; our disable_fog stand-in = D-AI-12c]; smoke styles
// (rocket/at4/grenade) draw alpha-blended with scene fog [orig: mode 0]. The 4-wide
// wave-animated smoke/sniper cross-section and the distortion pass (style +0x828)
// are tracked in docs/world/world-wac-ai-re.md §24.6 (D-AI-12a/b); the round's item
// graphic (frndlyTrcrID/foeTrcrID + TRACER_SCALE/TRACER_WIDTH nodes) is
// D-AI-12d; the light_move glow (round+0x1B4) rides the light pool now
// (Simulation.get_round_glow_rows -> EffectLightDirector.sync_round_glows).
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
	// non-finite anchor and the pass keeps the wire position. The ribbon
	// camera is the owner's listener position.
	void setup(Simulation *p_sim, Node3D *p_container, MissionAudio *p_audio,
			EffectWorld *p_fx, EffectLightDirector *p_lights);
	void teardown();

	// Once per present (beside the other passes), after the sim advanced. The
	// pending-sound countdown consumes logic ticks inside the sim now
	// (world/fire_sound.h) — this pass only drains and plays. Each drain forwards
	// into a public data leg (the present_snapshot precedent): tests feed the same
	// rows without a live sim.
	void present();
	void present_slot_sounds(const TypedArray<SlotSoundRow> &p_events);
	void present_sound_emitters(const TypedArray<SoundEmitterRow> &p_events);
	void present_fires(const TypedArray<FirePresentationEvent> &p_events);
	void present_fire_sounds(const TypedArray<FireSoundRow> &p_sounds);
	void draw_tracer_rows(const PackedFloat32Array &p_rows);

	// Load-time pipeline warm: emit one invisible (alpha-0) strip on each ribbon
	// material at `position` (must be in frustum so the strips actually draw) so
	// their pipelines compile behind the loading screen instead of as a ~40 ms
	// draw stall on the first tracer. The next present's clear_surfaces drops the
	// warm strips.
	void warm_pipelines(const Vector3 &p_position);

	// Typed diagnostic counters (ADR 0017: cross-object contracts are typed
	// records) — probes assert the presentation legs actually ran.
	Ref<FirePresentStats> get_stats() const;
	// The ribbon geometry surface — the ADR 0018 read seam for tests asserting the
	// rebuilt tracer strips (surface count, vertex layout) without private reach-ins.
	Ref<ImmediateMesh> ribbon_mesh() const { return mesh_; }

private:
	Simulation *sim() const;
	MissionAudio *audio() const;
	EffectWorld *fx() const;
	EffectLightDirector *lights() const;
	void free_mesh_instance();
	void emit_strip(const std::vector<float> &p_run, const Ref<StandardMaterial3D> &p_material);

	EntityPresenter *owner_ = nullptr;
	ObjectID sim_id_;    // drain + trail source (null in data-driven tests)
	ObjectID audio_id_;  // MissionAudio (or null)
	ObjectID fx_id_;     // EffectWorld (or null)
	ObjectID lights_id_; // EffectLightDirector: the MF_Light muzzle glow route (or null)
	Ref<ImmediateMesh> mesh_;
	ObjectID mesh_instance_id_;
	Ref<StandardMaterial3D> mat_additive_; // std/rapid/sniper/df1/NVG [orig: fog-black additive]
	Ref<StandardMaterial3D> mat_alpha_;    // rocket/at4/grenade smoke [orig: alpha + scene fog]
	int64_t stat_fires_ = 0;
	int64_t stat_sounds_ = 0;
	int64_t stat_effects_ = 0;
	int64_t stat_tracer_peak_ = 0;
};

} // namespace godot
