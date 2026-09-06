#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <functional>

#include "devtools/frame_stats.h"
#include "mission/mission_data.h"
#include "mission/mission_object_placer.h"
#include "mission/mission_perf_counters.h"
#include "mission/mission_setup_options.h"
#include "object/entity_index.h"
#include "object/entity_ref.h"
#include "object/item_database.h"
#include "object/object_model.h"
#include "resource_index/resource_root.h"
#include "simulation/entity_presenter.h"
#include "simulation/inmatch_session_values.h"
#include "simulation/player_aim_overlay.h"
#include "simulation/present_stats.h"
#include "simulation/simulation.h"
#include "simulation/tick_sink.h"

namespace godot {

class EffectLightDirector;
class EffectWorld;
class ItemEffectDirector;
class MissionAudio;
class MissionEnvironment;

// The per-mission subtree (ADR 0043 decision 9): the Godot presentation owner
// for one native inmatch::Session. It owns the Simulation (a RefCounted that
// drops with this root), the entity index, the "Entities" EntityPresenter
// child (which owns the four tick-driven present passes and its "Scars"
// child) and, in the game, the placer's "MissionObjects" container the load
// parents under it; cadence, lifecycle, and input retention remain native.
// GameWorld is its sole live owner. Ported from mission_presentation.gd
// (ADR 0043 slice G5).
//
// Per-tick order (single-sourced here, faithful to the original main loop's
// server-tick-then-render):
//   advance logic (sim) -> present entity state onto nodes -> drain + emit side effects.
// [orig: WacScript_AdvanceTick runs the logic systems; the client then renders the entities. Terrain/foliage/audio
//  are Godot render passes the caller composes around this.]
//
// Cadence: inmatch::Session banks wall clock and dispatches 0..N 62.5 Hz ticks;
// tick() asks that same session for one deterministic local/test step. The
// engine's WAC/BMS dividers remain inside their systems. Stop rewinds both the
// native world baseline and the authored node transforms captured at setup.
//
// Fixed-timestep accumulator. The original decouples the simulation from rendering: the master
// loop accumulates real elapsed time and dispatches the logic update once per 16 ms (62.5 Hz),
// independently of the variable render rate — multiple ticks on a long frame, zero on a short one.
// [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0 (one current_tick++ @ 0x24c1968)]
// The 0.016 s tick quantum is single-sourced natively as
// world::TickAccumulator::kTickDt (Simulation.tick_dt()); the accumulator
// arithmetic itself is native.
//
// This root IS the session's tick sink (simulation/tick_sink.h): the native
// loop calls on_session_tick synchronously per catch-up tick, and the root
// binds itself for exactly the duration of its own advance/step call.
class MissionRoot : public Node3D, private TickSink {
	GDCLASS(MissionRoot, Node3D)

public:
	// The Stats window's SIM_ROLE contract: the three roles the window names.
	enum StatsRole {
		STATS_ROLE_SINGLE_PLAYER = 0,
		STATS_ROLE_HOST = 1,
		STATS_ROLE_JOINER = 2,
	};

	MissionRoot();
	~MissionRoot() override;

	// Create + promote the mission, build the shared index over the placed
	// nodes (`container`), and wire the present pass. `options` is the typed
	// MissionSetupOptions record (ADR 0017); a net session carries its request
	// as options.host_session (HostSessionOptions) or options.join_target
	// (JoinTarget). Returns the AI entity count, or 0 on load failure (the
	// orphan sim is freed). Inspect get_setup_error() to distinguish a valid
	// empty mission from a setup failure. The sim is a RefCounted this root
	// owns.
	int setup(const Ref<MissionData> &p_mission, Node *p_container,
			const Ref<MissionSetupOptions> &p_options = Ref<MissionSetupOptions>());
	int get_setup_error() const { return setup_error_; }

	// World position of the entity addressed by a runtime SSN (WAC/BMS
	// addressing), or null when no live registry entity carries that net id.
	Variant entity_position_for_ssn(int p_ssn) const;
	// Full attached-effect transform for fx2ssn. Simulation owns the LIVE
	// registry lookup and frame data; presentation applies the single
	// canonical basis conversion shared with the mission present pass.
	Variant entity_effect_transform_for_ssn(int p_ssn) const;
	// True after at least one authoritative logic tick has produced a
	// client-view snapshot. Hosts can use this to distinguish "not presented
	// yet" (fall back to an authored Node seed) from "not present in the
	// current tick" (detach).
	bool has_current_present_effect_snapshot() const;
	// Resolve one presented entity by its stable value identity. Placed nodes
	// use bms_id first and (kind,index) as the zero-id fallback; wire-spawned
	// nodes use their wire handle. Returns null when the entity is absent
	// this tick.
	Variant presented_entity_effect_transform(const Ref<EntityRef> &p_entity_ref) const;

	String get_mission_file() const { return mission_file_; }
	String get_mission_name() const { return mission_name_; }

	// --- the local player (Phase 2; ADR 0012). Consumers reach the sim
	// natively via get_sim(). What remains here either composes (has_player),
	// decodes (aim overlay, ADR 0017), or feeds GameWorld's own
	// _music_var_pump. ---
	bool has_player() const;
	// Decoded at the Simulation transport edge (ADR 0017); null when absent/invalid.
	Ref<PlayerAimOverlay> local_player_aim_overlay() const;
	int local_player_team() const;

	// GameWorld hands the shared FrameStats here (game shell -> GameWorld ->
	// each root it creates). The sim folds its native phase spans onto the
	// same board inside advance_session_frame.
	void set_frame_stats(const Ref<FrameStats> &p_board);
	// Manual probe ownership is independent of F3 capture; the native timer
	// stays enabled while either consumer is active.
	void set_runtime_profiling_enabled(bool p_enabled);

	// Mission-presentation counters (probe/diagnostic seam).
	Ref<MissionPresentStats> get_mission_present_stats() const;

	// Bind the present passes' typed collaborators (ADR 0043 d9), after
	// setup(): `audio` (nullable) gates the fire/destruction sound legs — a
	// dedicated serve presents no sounds while the effect legs still run;
	// `fx` (nullable) the effect legs; `lights` (nullable) the MF_Light
	// muzzle glow + the death flash; `environment` (nullable) the scar pass's
	// fog distance + combined terrain light; `anchors` (nullable) the
	// owner-anchor registry the wreck/piece/move effect groups anchor
	// through. The load calls this once the audio and effect stages have run;
	// isolated tests pass what they have.
	void setup_passes(MissionAudio *p_audio, EffectWorld *p_fx,
			EffectLightDirector *p_lights, MissionEnvironment *p_environment,
			ItemEffectDirector *p_anchors);
	// Fire-presentation counters (FirePresentStats, typed per ADR 0017; an
	// empty record before setup).
	Ref<FirePresentStats> get_fire_present_stats() const;
	// Destruction-presentation counters (DestructionPresentStats, typed per ADR 0017).
	Ref<DestructionPresentStats> get_destruction_present_stats() const;
	// Wire-presentation counters (spawned/unresolved/live; empty before setup).
	Ref<WirePresentStats> get_wire_present_stats() const;
	// Cold wire rows the presentation budget still owes. Zero before setup;
	// SessionDrive keys the join-admission edge on this drain.
	int join_wire_present_pending() const;
	// Load-time warm hook: compile the fire-presentation pipelines (the
	// tracer ribbon materials) behind the loading screen; see GameWorld's
	// effect warm.
	void warm_present_pipelines(const Vector3 &p_at_position);
	// Throwable-presentation counters (ThrowablePresentStats, typed per ADR
	// 0017; built per call).
	Ref<ThrowablePresentStats> get_throwable_present_stats() const;
	// Scar-presentation counters (ScarPresentStats, typed per ADR 0017).
	Ref<ScarPresentStats> get_scar_present_stats() const;

	Ref<Simulation> get_sim() const { return sim_; }
	void set_presentation_time_ms(int64_t p_value_ms);
	// The placed-node index (bms_id/kind/group -> live node). The
	// render-occlusion frame resolves building masks and entity render gates
	// through it.
	Ref<EntityIndex> get_entity_index() const { return index_; }
	// The placer the index was built over (null for a placer-less preview).
	Ref<MissionObjectPlacer> get_placer() const { return registry_placer_; }
	// The item database the passes graft husk/round graphics from.
	Ref<ItemDatabase> get_item_db() const { return item_db_; }
	// The mission document the sim booted from.
	Ref<MissionData> get_mission_data() const { return mission_; }
	// The entity presenter (the "Entities" child): the render-occlusion
	// frame's wire render gates + lighting contexts, the wire-handle resolver
	// the destruction/scar passes read, the `wire_node_spawned` signal the
	// world's effect directors subscribe to, and the perf probes'
	// output-channel A/B seam. Null before setup.
	EntityPresenter *get_entity_presenter() const;
	// Re-key the placed-node index after a late placement (a joiner's
	// streamed statics, placed once the host's world stream settles). The
	// mission and wire presenters share this one index: the rebuild bumps its
	// generation, so the wire pass re-plans and releases any node a
	// now-placed row had claimed.
	void rebind_placed_entities(const Ref<MissionObjectPlacer> &p_placer);

	int entity_count() const;
	bool is_playing() const;

	// Advance EXACTLY ONE cadence step and present. Returns true when a logic
	// tick fired (and effects were drained). The deterministic single-tick
	// primitive: standalone F3/MCP Step and isolated tests/tooling previews
	// use this. GameWorld is the sole live real-time owner and advances the
	// native session; the editor preview has no self-ticking mission runtime.
	bool tick();
	// Real-time frame entry: bank `delta`, drain it in fixed tick_dt quanta, run that many single logic
	// ticks (clamped to the native kMaxCatchupTicks), and present ONCE after the batch — the faithful
	// fixed-62.5 Hz accumulator, with a zero-tick frame still presenting current render-only entity
	// rows (camera and local attach/detach change between fixed ticks). The native
	// session owns bank/clamp, input retention, and per-tick order; the GameWorld leg table
	// owns the concrete Godot device order [orig: Game_MainLoop @ 0x52b630].
	Ref<MissionFrameOutcome> advance_session_frame(const Ref<MissionFrameInput> &p_input);
	// The probe counters (built per call; the sim's own counters nest under
	// the record's JSON edge).
	Ref<MissionPerfCounters> get_perf_counters() const;

	// --- Debug/tooling transport (Play / Step / Stop) -----------------------

	// True while a live net session owns this runtime, i.e. the Play/Step/Stop
	// transport is locked out. The world tick is the
	// ONLY pump for the session socket (`Simulation::step` is the sole caller of
	// host_pump/joiner_pump), so halting it stops the C2S uplink, the keepalive and
	// the empty-send interval, and the peer reaps us at `cs_dir0.timeout_ms = 120000`
	// [orig: CNapiNetwork_Init @0x4ca4a0]. Retail multiplayer has no pause at all —
	// its in-game menu overlays a running match — and on a listen host a stopped
	// world would do this to every joiner at once. The ESC pause honours the same
	// rule in the shell; this is the single home so the F3 transport, the perf probe
	// and any future caller cannot bypass it.
	bool is_transport_locked() const;
	// Resume the engine session. False when nothing is loaded or the
	// transition is refused — Simulation.resume_session reports the
	// inmatch::Session verdict.
	bool play();
	// Pause the engine session. False when the session refuses — a net role
	// (inmatch::Session::pause is SinglePlayer-only) or a non-Running state —
	// so callers report the engine verdict instead of re-deriving the rule.
	bool pause();
	// One manual debug/tooling tick: one logic tick + present, outside the
	// real-time loop. The standalone game's F3/MCP Step and isolated
	// tests/tooling previews share this primitive. False when the session
	// refused the step (a net role, nothing loaded).
	bool step_once();
	// Stop: rewind the world to the play-start baseline AND restore the
	// authored node transforms, so the placed world is left exactly as it
	// was. Safe to call when never played.
	void stop();

	// The sim drops with this root's reference when it leaves the tree (a
	// reload / Stop queue_free()s the root): the explicit detach order.
	void _exit_tree() override;

protected:
	static void _bind_methods();

private:
	// The per-tick presentation sink (ADR 0035): pose latch -> the throwable
	// fixed-tick sync -> drain_effects -> effects_drained PER TICK ->
	// fixed_tick_completed, synchronously inside the native catch-up batch.
	bool on_session_tick(const opennova::inmatch::TickOutcome &p_tick) override;

	// Binds this root as the sim's TickSink for exactly one advance/step
	// call (null between calls: a direct Simulation.step() or a stray holder
	// of the sim never reaches a dead owner).
	struct TickSinkScope {
		Simulation *sim;
		TickSinkScope(Simulation *p_sim, TickSink *p_sink) : sim(p_sim) { sim->set_tick_sink(p_sink); }
		~TickSinkScope() { sim->set_tick_sink(nullptr); }
	};

	EntityPresenter *entities() const;
	Node3D *container() const;
	Variant effect_transform_from_state(const PackedVector3Array &p_state) const;
	void clear_present_effect_poses();
	void begin_present_effect_tick(int p_logic_tick);
	void _on_frame_stats_capture_changed(bool p_active);
	void sync_runtime_profiling();
	// A joiner's stamped slot vanished or was re-typed: its placed
	// representation (an individual node or a batched static instance) must
	// stop drawing, since the wire walk now owns whatever occupies that slot.
	void retire_placed_rows();
	void present_entity_rows(bool p_stats_on);
	// One whole present frame: the entity rows plus the tick-driven passes,
	// each pass timed onto the stats board while the Stats tab captures. Owns
	// the bundled perf_present_us_ the probe scripts read.
	void present_frame(bool p_stats_on);
	bool stats_capture_on() const;
	// Pull the frame's tick accounting into the probe counters from the typed
	// outcome. The session phase spans (SIM_STEP and the SIM_* attribution)
	// land on the board natively inside Simulation.advance_session_frame
	// while the profiling clocks run; only the shell-measured legs are fed
	// here (the per-pass present spans land inside the present legs
	// themselves).
	void read_frame_perf(const Ref<MissionFrameOutcome> &p_outcome);
	int stats_role_value() const;
	void feed_projectile_trace_stats();
	void capture_transforms();
	void restore_transforms();
	// Walk the nodes the present pass drives (resolved from the current
	// snapshot through the shared index).
	void for_each_present_node(const std::function<void(ObjectModel *)> &p_fn);

	Ref<Simulation> sim_;
	// THE entity presenter (ADR 0043 d9), the "Entities" child: its placed
	// walk drives the authored nodes on every role (or a tooling/test
	// preview); its wire walk, when set up, materializes un-placed network
	// entities or SP attachment children; it owns the four tick-driven present
	// passes (fire, destruction, throwable, scars) and its "Scars" child.
	ObjectID entities_id_;
	// The pass inputs captured at setup for setup_passes: the mission
	// container the passes graft into, the item database (husk/round
	// graphics) and the resource root (scar strips).
	ObjectID container_id_;
	Ref<ItemDatabase> item_db_;
	Ref<ResourceRoot> resource_root_;
	Ref<MissionData> mission_;
	Ref<EntityIndex> index_;
	Ref<MissionObjectPlacer> registry_placer_;
	// node -> Transform3D captured at setup, for restore-on-stop.
	HashMap<uint64_t, Transform3D> orig_transforms_;
	int64_t perf_tick_us_ = 0;
	int64_t perf_sim_us_ = 0;
	int64_t perf_present_us_ = 0;
	int64_t perf_effects_us_ = 0;
	bool perf_did_tick_ = false;
	// The shared F3 frame-stats board (null outside the game shell). While
	// its Stats tab captures, the sim/net legs and each present pass land
	// their spans on it; otherwise all extra clock reads are skipped.
	Ref<FrameStats> frame_stats_;
	bool runtime_probe_enabled_ = false;
	bool has_trace_stats_sampling_ = false;
	int ticks_last_frame_ = 0; // logic ticks run by the last session frame
	int64_t presentation_time_ms_ = -1; // shared render/PANM DWORD; negative = direct-sim fallback
	// Stable mission identity for diagnostics (the dev tools, the MCP catalog).
	String mission_file_;
	String mission_name_;
	int setup_error_ = OK;
	// The current authoritative present tick (-1 = none yet). Attachment-pose
	// lookups are Simulation's generation-bound native index — the one path;
	// test sims implement the same compact API.
	int effect_pose_snapshot_tick_ = -1;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MissionRoot::StatsRole);
