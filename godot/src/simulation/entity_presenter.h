#pragma once

#include <godot_cpp/variant/packed_int32_array.hpp>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <runtime/world/body_present.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/minefield.h>

#include "object/entity_index.h"
#include "object/item_database.h"
#include "object/object_model.h"
#include "mission/mission_object_placer.h"
#include "resource_index/resource_root.h"
#include "simulation/destruction_events.h"
#include "simulation/destruction_presenter.h"
#include "simulation/fire_presenter.h"
#include "simulation/person_overlay_models.h"
#include "simulation/present_event_records.h"
#include "simulation/present_stats.h"
#include "simulation/throwable_presenter.h"
#include "simulation/vehicle_trail_presenter.h"

namespace godot {

class EffectLightDirector;
class EffectWorld;
class MissionAudio;
class MissionEnvironment;
class ScarDrawList;
class ScarPresenter;
class Simulation;

// THE entity presenter (ADR 0043 decision 9): the one node that renders a
// mission's entity rows from the sim's flat PackedFloat32Array snapshot —
// the PLACED walk over the authored nodes the EntityIndex resolves and the
// WIRE walk over the remote/runtime rows it materializes itself. One object,
// two row tables: `Row` for placed rows and `WireRow` for wire rows (their
// contracts differ; the shared per-row legs are factored into helpers both
// walks call). There is no name-based dispatch and no capability probing:
// the model class IS the contract.
//
// The placed walk owns its hot loop: the revision-bound row plan (resolved
// typed ObjectModel, applied-state caches) and the per-frame walk dispatching
// direct C++ calls only on change. The walk itself is shell presentation glue
// over the witnessed per-frame cadence (entity submission is evaluated every
// render frame [orig: GameLoop_RenderFrame @ 0x521310 ->
// Render_ProcessMainSceneFrame]); the behavioral semantics it applies are the
// ones already recorded at each leg's GDScript origin and carried over
// verbatim.
//
// The wire walk renders a co-op JOINER's remote entities WIRE-DIRECT (docs/net
// §5.23/§5.25/§5.38b; ADR 0026 — the sole presenter for a decoded
// ClientState). A production joiner has no authored placed-node identity
// table. Its native sim separately materializes streamed pools 1-3 at the
// HOST's exact packed handles for world-side gameplay, while remote pool-0
// organics remain decoded client state; this walk renders every remote row
// from the load batches plus live S2C 0x0A instead of resolving a local .bms
// placement. The joiner's OWN player (wire handle H) is self-filtered out of
// the snapshot (PF_TYPE_ID 0) and drawn by LocalPlayerPresenter as the
// motor-driven local avatar L — the live §5.38b two-handle reconciliation.
// Its COLD path (spawn/defer/unresolved bookkeeping, the liveness prune,
// held-weapon builds, spawn signals and stats) and its per-row hot path live
// in entity_presenter_wire.cpp; the pool->kind projection witness is at the
// engine header (runtime/inmatch/wire_present.h).
//
// Beside the two row walks it OWNS the four tick-driven present passes (ADR
// 0043 d9): FirePresenter (AI/remote fire sound + muzzle effect + tracers),
// DestructionPresenter (husk swaps, death pieces, wreck effects) and
// ThrowablePresenter (flying/placed throwable models + the round-bound move
// groups) as members, and the "Scars" ScarPresenter child (the impact-scar
// rings). The driver runs the row walks, then present_passes() — fire ->
// destruction -> throwable -> scars, scars LAST: the entity-ring meshes
// parent under section nodes the row walks built and the husk grafts onto the
// placed/wire models. A zero-tick frame runs only the row walks. The teardown
// (reset_wire_runtime_state) resets the passes' runtime state with the wire
// registry.
class EntityPresenter : public Node3D {
	using PresentRowsView = opennova::world::PresentRowsView;
	GDCLASS(EntityPresenter, Node3D)

public:
	EntityPresenter();

	enum OutputChannels {
		OUTPUT_TRANSFORM = 1,
		OUTPUT_PART_ANIM = 2,
		OUTPUT_VISIBILITY = 4,
		OUTPUT_BODY_ANIM = 8,
		OUTPUT_ALL = OUTPUT_TRANSFORM | OUTPUT_PART_ANIM | OUTPUT_VISIBILITY |
				OUTPUT_BODY_ANIM,
	};
	// Fixed packed layout returned only by profile_present_snapshot(). The
	// ordinary present_snapshot() path takes no timestamps or result allocation.
	enum MissionProfileSlot {
		MISSION_PROFILE_CORE_US = 0,
		MISSION_PROFILE_AIM_US,
		MISSION_PROFILE_CONTROLS_US,
		MISSION_PROFILE_VISIBILITY_US,
		MISSION_PROFILE_BODY_US,
		MISSION_PROFILE_ROWS,
		MISSION_PROFILE_SUBMITTED_ROWS,
		MISSION_PROFILE_BODY_ROWS,
		MISSION_PROFILE_SLOT_COUNT,
	};
	// Building one streamed model can synchronously assemble enough Godot
	// resources to take a substantial part of a frame — a platform cost with
	// no retail counterpart (retail materializes its world stream under the
	// loading/DEATH hold). The budget caps cold builds per presentation call
	// so a large cold topology cannot occupy the SceneTree thread; four keeps
	// tiny dynamic cohorts atomic while yielding hundreds-row streamed loads
	// promptly. SessionDrive holds the join-admission edge until
	// pending_spawn_count() drains to zero, so the revealed world is fully
	// materialized, like retail's.
	static constexpr int DEFAULT_COLD_SPAWN_BUDGET = 4;
	enum HeldWeaponConstants {
		HELD_WEAPON_BONE_INDEX =
				opennova::world::kHeldWeaponBoneIndex,
	};

	// --- The PLACED walk -----------------------------------------------------

	// `sim` exposes native lazy muzzle resolution plus the no-native-rig push-back
	// fallback (typed Simulation; converted once at this boundary). `index` resolves
	// rows to typed models — a real EntityIndex, in tests too.
	void setup(Object *sim, Object *index,
			const Ref<MissionObjectPlacer> &placer = Ref<MissionObjectPlacer>());
	void set_output_channels(int channels);
	int get_output_channels() const { return output_channels_; }

	// The runtime driver pre-fetches one shared snapshot and hands it to both
	// walks (present_snapshot, then present_wire_snapshot). `door_phases` is
	// the snapshot's door side table (Simulation::get_present_door_phases):
	// (row index, count, phase[count]) entries in row order for the rows whose
	// PF_DOOR_COUNT is nonzero; a source without doors passes none.
	void present_snapshot(const PackedFloat32Array &snap, int stride,
			int64_t layout_revision,
			const PackedInt32Array &door_phases = PackedInt32Array());
	void present_rows(PresentRowsView snap, int stride, int64_t layout_revision,
			const int32_t *door_phases, int64_t door_phase_count);
	PackedInt64Array profile_present_snapshot(PresentRowsView snap,
			int stride, int64_t layout_revision,
			const int32_t *door_phases, int64_t door_phase_count);
	// The rows whose casters a render slot newly captured (`p_captured`, model
	// instance ids) after this frame's walks skipped their legs (no view the
	// frame draws collected them), presented again over the frame's own rows
	// ahead of the slot's capture: retail's shadow pass draws its caster through
	// the caster's own render callback (engine renderer/render_slot_shadow.h
	// carries the witness). No time passes (a wire row's tick delta is spent).
	void present_slot_captures(PresentRowsView snap, int stride, int64_t layout_revision,
			const int32_t *door_phases, int64_t door_phase_count,
			const std::vector<uint64_t> &p_captured);
	// The script form over a packed snapshot (the present_snapshot precedent).
	void present_slot_captures_snapshot(const PackedFloat32Array &p_snap, int p_stride,
			int64_t p_layout_revision, const PackedInt64Array &p_captured);

	Ref<MissionPresentStats> get_stats_record() const;

	// --- The WIRE walk -------------------------------------------------------

	// defer_index: any wire row that resolves to an authored placed node is
	// rendered by the placed walk instead. On the HOST that leaves admitted
	// joiners; a production header-only JOINER passes no defer index and
	// draws every remote row.
	void setup_wire(Object *p_sim, const Ref<MissionObjectPlacer> &p_placer,
			Node3D *p_container, const Ref<EntityIndex> &p_defer_index);
	// SP attachment presentation: only synthetic runtime rows (kind 255,
	// index 0xFFFFFF) render wire-direct.
	void set_synthetic_origin_only(bool p_enabled);
	void set_cold_spawn_budget(int p_budget);
	// Spectator-only one-shot overview camera; live presenters never set one.
	void set_spectator_camera(Camera3D *p_camera);

	void present_wire_snapshot(const PackedFloat32Array &p_snap, int p_stride,
			int64_t p_layout_revision);
	void present_wire_rows_view(PresentRowsView p_snap, int p_stride,
			int64_t p_layout_revision);
	// One wire pass that builds every cold row whatever the cold-spawn budget, which stands as it
	// was after it: the load's, behind the loading screen, so no frame past the reveal builds a
	// model (D-NET-1377). Retail draws every pool row from its first frame; the budget is this
	// presenter's own pacing.
	void materialize_wire_rows_view(PresentRowsView p_snap, int p_stride, int64_t p_layout_revision);
	void materialize_wire_snapshot(const PackedFloat32Array &p_snap, int p_stride,
			int64_t p_layout_revision);
	// The wire walk's parts, timed (an instrument with no behaviour: MissionRoot takes it in
	// place of present_wire_rows_view while the frame-stats board captures, for the F3 wire
	// rows and the --verbose SessionLeg line). Nanoseconds per part; the rows' walk total
	// beside them, whose remainder past the parts is the walk's own loop.
	enum WireProfilePart {
		WIRE_PROFILE_PLAN = 0,
		WIRE_PROFILE_POSE,
		WIRE_PROFILE_CTRL,
		WIRE_PROFILE_BODY,
		WIRE_PROFILE_WEAPON,
		WIRE_PROFILE_SOUNDS,
		WIRE_PROFILE_HELD,
		WIRE_PROFILE_VIEWS,
		WIRE_PROFILE_CULLED,
		WIRE_PROFILE_PART_COUNT,
	};
	struct WireProfile {
		int64_t ns[WIRE_PROFILE_PART_COUNT] = {};
		int64_t walk_ns = 0;
		int64_t rows = 0;
		int64_t cold_plans = 0;
	};
	WireProfile profile_present_wire_rows_view(PresentRowsView p_snap, int p_stride,
			int64_t p_layout_revision);

	// The occlusion frame's render-gate verdict for one wire row (the
	// collector gate the placed rows get by bms id). A culled row's node
	// hides and skips its presentation legs until the gate releases it.
	void set_render_culled(int p_handle, bool p_culled);
	// Forget every render-gate verdict (paired with the sim's applied-state
	// baseline reset: the next frame re-emits the full set).
	void clear_render_culled() { wire_render_culled_.clear(); }
	// The weapon Inset pass's own collector verdicts for the wire rows, live
	// while that pass renders (OcclusionFrame::apply_inset_frame): a row
	// either view draws runs its presentation legs, and the body and its held
	// gun take each view's verdict (the Inset's drawn by an ObjectModel twin,
	// object/object_model.h).
	void set_render_culled_inset(int p_handle, bool p_culled);
	void set_wire_inset_view(bool p_active);
	// A water mirror's own collector verdicts for the wire rows (view 0 the
	// main scene's mirror, 1 the weapon Inset's), live while that mirror
	// renders: the reflected subscene draws its own collect's lists, so a row
	// a mirror draws runs its legs and the body, its held gun and its overlays
	// take that mirror's verdict (drawn by an ObjectModel twin where it
	// differs, object/object_model.h).
	void set_render_culled_mirror(int p_view, int p_handle, bool p_culled);
	void set_wire_mirror_view(int p_view, bool p_active);

	// Cold rows the budget deferred on the last presented frame.
	int pending_spawn_count() const { return pending_spawn_count_; }
	Ref<WirePresentStats> get_wire_stats_record() const;

	// The live model owned by this presenter for a packed pool/slot handle
	// (runtime-only entities have no authored BMS identity). Freed entries are
	// ordinary (world teardown frees the container's children first).
	ObjectModel *resolve_wire_handle(int p_handle) const;
	// The live held-weapon model for a wire body (null when unarmed/freed).
	ObjectModel *held_weapon_node(int p_handle) const;
	// A wire body's item overlays (canopy, goggles, binoculars, carried
	// object); null until the body first publishes one.
	Ref<PersonOverlayModels> person_overlays_for(int p_handle) const;
	// Cache and apply one wire draw's environment-lighting context to both its
	// body and held weapon. The cache makes a quality change authoritative even
	// when cold-spawn budgeting has not built either node yet.
	// The interior light group (containing building BMS id + blink volume
	// section) rides along for the per-draw point-light select.
	void set_entity_lighting_context(int p_handle, float p_effect_scale,
			bool p_interior_lerp, float p_light_transfer, int p_interior_bms = 0,
			int p_interior_section = 0);
	// World position of a named userpoint on this wire body's HELD WEAPON —
	// the anchor retail's adm-arm fire effect spawns at. Falls back to the
	// body's own origin, never the wire fire position (witness:
	// runtime/inmatch/wire_present.h ledger, rigid weapon draw + userpoint
	// fallback).
	Vector3 muzzle_world_for(int p_handle, const String &p_userpoint) const;
	int wire_entity_count() const { return int(nodes_.size()); }
	// The live spawned bodies, in registry order: a consumer that subscribes
	// to `wire_node_spawned` after the first present replays these itself.
	TypedArray<ObjectModel> wire_nodes() const;

	// Injection seam (tests/tooling): adopt an existing node as this
	// presenter's wire avatar for `handle`, exactly as a cold build would
	// have. The presenter does NOT take ownership.
	void register_wire_node(int p_handle, ObjectModel *p_node);
	// Injection seam (tests/tooling): adopt an existing model as the
	// third-person gun of `handle` (what a wire ADM edge would have built), so
	// muzzle_world_for resolves its userpoints. Not owned.
	void register_wire_held_weapon(int p_handle, ObjectModel *p_node);

	// Free every wire body/weapon and forget the wire plan, caches and
	// verdicts, then reset the present passes' runtime state (the teardown's
	// first leg).
	void reset_wire_runtime_state();
	// Wire teardown plus the placed plan's release and the passes' teardown
	// (the node is going away).
	void teardown();

	// --- The local view's virtual display ------------------------------------

	// The live model a packed pool/slot handle presents through EITHER walk:
	// the wire registry, else the placed row plan (each planned row keeps its
	// row's PF_WIRE_HANDLE). Null for a handle neither walk draws.
	ObjectModel *resolve_present_handle(int p_handle) const;
	// The device half of the vehicle render class's first-person swap (the
	// rule and its witnesses live with world::LocalPlayerViewFrame, engine
	// runtime/world/local_player_view.h). While the frame says the local
	// player's vehicle draws its virtual display -- the same frame that hull's
	// row is PF_LOCAL_VIEW_SUPPRESSED, so the walks above hid its node and kept
	// stamping its transform -- this shows graphic `p_model` rigidly at the
	// carrier node's CURRENT transform, as an ordinary world model beside the
	// carrier, lit as the carrier is. It hides otherwise; an empty graphic key
	// draws nothing (the def authors no virtual display). The local-view frame
	// leg feeds the frame's triple once per display frame, after the row walks
	// stamped the carrier; tests feed the same triple. One model is built per
	// graphic key and carrier node and reused -- shown or hidden -- until
	// either changes or the runtime state resets.
	void present_virtual_display(bool p_active, int p_carrier_handle, const String &p_model);
	// The built virtual-display model (null before the first show and after a
	// reset) -- the read seam.
	ObjectModel *virtual_display_node() const;

	// --- The present passes (ADR 0043 decision 9) ----------------------------

	enum PassProfileSlot {
		PASS_PROFILE_FIRE_US = 0,
		PASS_PROFILE_DESTRUCTION_US,
		PASS_PROFILE_THROWABLE_US,
		PASS_PROFILE_SCARS_US,
		PASS_PROFILE_SLOT_COUNT,
	};

	// Wire the passes over the typed collaborators (after setup()/setup_wire():
	// the sim, index and placer are the ones those bound). `container` (the
	// mission container, nullable) hosts the tracer geometry, the throwable
	// models and the node-less husk grafts; `item_db` names the husk/round
	// graphics; `resource_root` resolves the scar strips. `audio` (nullable)
	// gates the fire/destruction sound legs: no audio, no sounds, the effect
	// legs still run (a dedicated host binds no pass at all, MissionRoot's
	// session step presents nothing there); `fx` (nullable) the effect
	// legs; `lights` (nullable) the MF_Light muzzle glow and the death flash;
	// `environment` (nullable) the scar fog cull + terrain light. The wreck, piece and round
	// effect groups are the engine's (world::DestructionEmitters, world::RoundEmitters).
	void setup_passes(Node3D *p_container, const Ref<ItemDatabase> &p_item_db,
			const Ref<ResourceRoot> &p_resource_root, MissionAudio *p_audio,
			EffectWorld *p_fx, EffectLightDirector *p_lights,
			MissionEnvironment *p_environment);
	// The presenting shell's listener: the camera position the driver pushes
	// once per present frame (MissionFrameInput.set_camera_sample carries the
	// same sample to the sim's fire-sound distance gate, world/fire_sound.h);
	// fire's ribbon camera and scar's fog-box cull both read it. A host with
	// no camera (a dedicated serve) never pushes, which is the witnessed peer
	// gate (engine: runtime/world/fire_sound.cpp). Non-finite until the first push.
	void set_listener_position(const Vector3 &p_position);
	Vector3 listener_position() const { return listener_position_; }
	// The four passes in drive order, once per ticked present frame.
	void present_passes();
	// The fire pass's tracer ribbons, once per drawn scene frame after the frame
	// placed its views (FirePresenter::draw_frame_tracers).
	void draw_frame_tracers();
	// present_passes() timed per pass (PassProfileSlot, microseconds) for the
	// stats board.
	PackedInt64Array profile_present_passes();
	// Throwable's fixed-tick half: the round-bound move groups reconcile at the
	// tick sink BEFORE the effect world advances (the model reconcile stays in
	// present_passes).
	void sync_fixed_tick_effects();
	Ref<FirePresentStats> get_fire_present_stats() const;
	Ref<DestructionPresentStats> get_destruction_present_stats() const;
	// The skeletal rows this frame's walks presented, drawn or not, as the
	// ragdoll corpses read them (ADR 0060). The ragdoll leg takes them once a
	// frame; a frame whose walks did not run hands over none.
	struct RagdollRow {
		ObjectModel *model = nullptr;
		int32_t handle = -1;
		int32_t life = 0;       // PF_RESPAWN_REVISION
		int32_t anim_state = -1;
		bool hidden = false;
		bool severed = false;   // a dismemberment piece (its hips hidden)
	};
	std::vector<RagdollRow> take_ragdoll_rows();
	// Off (the default) records nothing: only a session with ragdolls on
	// collects the rows its leg takes.
	void set_collect_ragdoll_rows(bool p_collect);
	// Built per call (the live census).
	Ref<ThrowablePresentStats> get_throwable_present_stats() const;
	Ref<ScarPresentStats> get_scar_present_stats() const;
	void warm_fire_pipelines(const Vector3 &p_position);
	// The load's forced frame's warm of the wire walk's cold spawns (GameWorld::
	// build_person_pipeline_warm): one model per visual identity (type word, character id) among
	// the snapshot's rows the walk would build a node for, under `p_parent`, so their meshes,
	// materials and rigs are made and their pipelines compile in that frame instead of the
	// first revealed one. The caller frees them past the frame; nothing here is registered.
	// Appends each built model's graphic to `r_graphics` and returns the count.
	int warm_wire_spawn_pipelines(const PackedFloat32Array &p_snap, int p_stride, Node3D *p_parent,
			PackedStringArray &r_graphics);
	Ref<ArrayMesh> fire_ribbon_mesh() const;
	// The water mirrors' cameras the fire pass builds each mirror's tracer
	// ribbons against (FirePresenter::set_mirror_cameras), mirror `p_mirror`'s
	// ribbon vertices of this drawn frame (0 the main scene's mirror, 1 the
	// Inset's), the shadings of the batches this frame's mirror ribbons submit
	// in overlay slot `p_slot` (FirePresenter::append_mirror_ribbons into a
	// throwaway frame) and the weapon Inset scene's ribbons.
	void set_tracer_mirror_cameras(Camera3D *p_mirror, Camera3D *p_inset_mirror);
	PackedVector3Array fire_mirror_ribbon_vertices(int p_mirror) const;
	PackedInt32Array fire_mirror_ribbon_shadings(int p_slot);
	Ref<ArrayMesh> fire_inset_ribbon_mesh() const;
	// The owned "Scars" child (the device read seam: its ScarWorld mesh).
	ScarPresenter *scar_presenter() const;
	// The data legs (the present_snapshot precedent): production
	// present_passes() drains the typed sim; tests feed the same rows.
	void present_fires(const TypedArray<FirePresentationEvent> &p_events);
	void present_fire_sounds(const TypedArray<FireSoundRow> &p_sounds);
	void present_slot_sounds(const TypedArray<SlotSoundRow> &p_events);
	void present_sound_emitters(const TypedArray<SoundEmitterRow> &p_events);
	void draw_tracer_rows(const PackedFloat32Array &p_rows);
	void present_destruction_drained(const Ref<DestructionDrain> &p_events,
			const TypedArray<DeathPieceRow> &p_pieces);
	// The frame's death-piece draws (the occlusion frame's collect,
	// Simulation::scene_draws); the typed form is the tests' data leg.
	void present_death_piece_draws_native(
			const std::vector<opennova::world::DeathPieceDraw> &p_draws);
	// The frame's draw-leg output (Simulation::take_draw_husk_output), at the occlusion leg's
	// end: its husk swaps through the destruction pass, and each changed row's swap-fade slots
	// re-stamped on its model, so the collect's publication draws this frame, as retail's draw
	// publishes and then draws in one call (engine world::run_draw_husk_leg).
	void present_draw_husk_leg(const std::vector<opennova::world::HuskSwapEvent> &p_swaps,
			const std::vector<opennova::world::DrawFadeStamp> &p_fades);
	void present_death_piece_draws(const TypedArray<DeathPieceDraw> &p_draws);
	// The weapon Inset pass's own piece draws (OcclusionFrame::apply_inset_frame).
	void present_death_piece_draws_inset(
			const std::vector<opennova::world::DeathPieceDraw> &p_draws);
	void present_death_piece_draws_mirror(int p_view,
			const std::vector<opennova::world::DeathPieceDraw> &p_draws);
	// A death-piece slot's model (null when none) — the draw leg's read seam.
	ObjectModel *death_piece_model(int p_slot) const;
	// The frame's NVG laser beams into the overlay tail (FirePresenter::
	// append_nvg_laser_beams over Simulation::nvg_laser_sources).
	int append_nvg_laser_beams(Simulation *p_sim, const NvgLaserView &p_view,
			SceneOverlaySubmission &r_submission);
	// Each rendering water mirror's tracer ribbons of this drawn frame into its
	// own overlay slot under the reflected scene's pass fog
	// (FirePresenter::append_mirror_ribbons).
	int append_mirror_tracer_ribbons(const opennova::renderer::SceneOverlayFog &p_pass_fog,
			SceneOverlaySubmission &r_submission);
	// The data leg: one candidate row through the same beam path into a
	// throwaway frame; returns the batches it drew in the view's slot
	// (NvgLaserBeams, or InsetNvgLaserBeams for the weapon Inset view's walk).
	int nvg_laser_beam_batches(int p_handle, int p_attach_bone, int p_weapon_flags,
			int p_launch_userpoint, bool p_local_player, bool p_nvg_active, int p_camera_mode,
			const Transform3D &p_eye, bool p_inset_view = false);
	void present_throwable_visuals(const TypedArray<ThrowableVisualRow> &p_visuals);
	// The flying-round models the main scene's and the weapon Inset's collects draw this
	// frame, by presentation key (Simulation::scene_draws, InsetOcclusionView::draws):
	// each round model takes the view's verdict (ThrowablePresenter::apply_round_draws).
	void present_round_draws(const std::vector<int64_t> &p_keys);
	void present_round_draws_inset(const std::vector<int64_t> &p_keys);
	void present_vehicle_trail_visuals(const TypedArray<VehicleTrailVisualRow> &p_visuals);
	void present_scar_draw_list(const Ref<ScarDrawList> &p_draw_list);

	// --- Statics shared by both walks and their consumers --------------------

	// Aim-overlay presentation (the local avatar and the tests delegate here
	// so every body shares one implementation). root_basis: aim-valid rows own
	// the body rotation; others keep the fallback entity rotation.
	static Basis aim_root_basis(const PackedFloat32Array &snap, int base,
			const Basis &fallback);
	static void aim_apply(Object *node, const PackedFloat32Array &snap, int base,
			bool drive_root_basis);
	static Basis aim_root_basis_native(PresentRowsView snap, int base, const Basis &fallback);
	static void aim_apply_valid(Object *node, PresentRowsView snap,
			int base, bool drive_root_basis);

	// Third-person held-weapon placement: the engine's (world::held_weapon_attach
	// over the body's posed bone 16, world::posed_bone_frames), placed by the
	// body's world transform as the presenter composed it; returns a
	// Transform3D, or null when the body has no posed rig to place one.
	static Variant held_weapon_attach_transform(ObjectModel *body, const Transform3D &body_world,
			const Vector3 &attach_angles_bms, bool hand_frame);
	// The hand frame's calibration over a bone basis (the engine's), for tests/tooling.
	static Basis held_weapon_hand_frame_basis(const Basis &bone_model_to_world);
	// The engine calibration re-exported for tests/tooling (float/vector
	// values cannot be class constants).
	static Vector3 held_weapon_attach_nudge();
	static double held_weapon_hand_frame_z_rad();
	static double held_weapon_hand_frame_y_rad();

	// Emplaced-weapon CTRL registers (bound statics = the coop_two_sim test's
	// seam onto the native leg): EWEAP_GUNYAW/EWEAP_GUNPITCH/WEAP_SPIN only —
	// a bulk CTRL clear would also erase live WAC channels.
	static int emplaced_apply(Object *node, const PackedFloat32Array &snap,
			int base, bool clear_when_invalid);

protected:
	static void _bind_methods();

private:
    struct MineMarkerNode {
        ObjectID node;
        uint64_t spawn_id = 0;
        bool seen = false;
    };
    std::unordered_map<uint32_t, MineMarkerNode> minefield_nodes_;
    std::vector<opennova::world::MinefieldDraw> minefield_draws_;
    void present_minefields();
    void reset_minefields();
	// The virtual display: the built model, and the graphic key and carrier
	// node it was built for.
	ObjectID virtual_display_id_;
	String virtual_display_graphic_;
	ObjectID virtual_display_carrier_id_;
	void reset_virtual_display();
	// resolve_present_handle's last answering placed row (a lookup hint only).
	mutable size_t present_handle_hint_ = 0;
	struct MissionFrameProfile {
		int64_t core_us = 0;
		int64_t aim_us = 0;
		int64_t controls_us = 0;
		int64_t visibility_us = 0;
		int64_t body_us = 0;
		int64_t rows = 0;
		int64_t submitted_rows = 0;
		int64_t body_rows = 0;
	};
	enum CtrlPublishField {
		CTRL_PUBLISH_PART1 = 0,
		CTRL_PUBLISH_PART2,
		CTRL_PUBLISH_EMPLACED,
		CTRL_PUBLISH_VEHICLE,
		CTRL_PUBLISH_TEX_TEAM,
		CTRL_PUBLISH_ZONE,
		CTRL_PUBLISH_LFP,
		CTRL_PUBLISH_HEAT,
        CTRL_PUBLISH_DOORS,
		CTRL_PUBLISH_COUNT,
	};

	// The aim-overlay leg's inputs: the body Euler triple plus all nine
	// overlay-class triples (aim_apply_valid's exact reads), one contiguous
	// PF span both walks compare before dispatching.
	static constexpr int kAimPayloadFloats = 30;

	struct Row {
		int base = 0;
		ObjectID node_id;
		// Topology-stamped typed hot-path reference. ObjectModel's lifetime
		// generation invalidates the plan before a freed pointer can be reused;
		// node_id remains the cold release/fallback identity.
		ObjectModel *model = nullptr;
		int32_t entity_kind = -1;
		int32_t entity_index = -1;
		int32_t bms_id = 0;
		// The row's PF_WIRE_HANDLE (the packed registry handle; stable for the
		// plan's layout revision): resolve_present_handle's placed key.
		int32_t handle = -1;
		// Last-applied edge state (-1 = unknown, first frame always applies).
		int32_t aim_valid = -1;
		int32_t rhc = -1;
		int64_t cut_sections = -1;
		int64_t destroyed_section_mask = -2;
		// The swap fade's publication edge (stamp_destroy_phases).
		int32_t destroy_published = -1;
		bool transform_stamp_valid = false;
		std::array<float, 6> transform_stamp = {};
		bool aim_payload_valid = false;
		std::array<float, kAimPayloadFloats> aim_payload = {};
		// Validity/ownership edges for the semantic CTRL publishers. Active
		// retail writers are reasserted on every submission so another owner
		// cannot leave a retained value behind; omitted writers clear only on
		// the cold/falling edge.
		bool ctrl_publish_state_valid = false;
		std::array<int32_t, CTRL_PUBLISH_COUNT> ctrl_publish_state = {};
		// The body clip request last dispatched (world::placed_body_clip_request).
		bool body_stamp_valid = false;
		opennova::world::BodyClipRequest body_request;
		// The placed walk that last submitted the row (placed_walk_serial_).
		uint64_t submitted_walk = 0;
	};

	struct WireRow {
		int base = 0;
		int handle = 0;
		ObjectID node_id;
		bool spawned_now = false;
		bool culled = false; // the last walk took the culled branch
		// Last-applied edge state (-1 = unknown, first hot frame applies).
		int32_t aim_valid = -1;
		int32_t rhc = -1;
		int64_t cut_sections = -1;
		int64_t destroyed_section_mask = -2;
		int32_t destroy_published = -1; // stamp_destroy_phases' edge
		// The PF_HUSK pick last handed to the husk swap (-1: none stands, the
		// next husked frame hands it) and the husk the swap stamped on the
		// model then (ObjectModel::get_husk_visual_id).
		int32_t husk_applied = -1;
		ObjectID husk_visual;
		// The remote body-transition latch (world::present_wire_body; re-seeded
		// from the per-handle cache on every plan build).
		opennova::world::WireBodyLatch body_latch;
		// Per-leg input caches for the measured every-frame legs (the 2026-08-04
		// joiner profile: ~21us/row ungated over a full streamed world). Each
		// holds the exact PF fields its leg consumes; an invalid cache forces
		// the first application, and a cold plan rebuild re-applies once by
		// construction (rows rebuild with invalid caches). The CTRL field list
		// lives beside its leg in entity_presenter_wire.cpp; the aim cache is
		// the same contiguous payload the placed walk compares.
		static constexpr int kCtrlCacheCount = 55;
		float ctrl_cache[kCtrlCacheCount];
		std::array<float, kAimPayloadFloats> aim_cache = {};
		bool ctrl_cache_valid = false;
		bool aim_cache_valid = false;
		int32_t wpn_state = INT32_MIN;
		int32_t wpn_src_state = INT32_MIN;
		int32_t wpn_src_phase = -1;
		float wpn_weight = 1.0f;
		int32_t wpn_variant = 0;
		int32_t wpn_src_variant = 0;
		bool wpn_parked = false; // PF_WPN_PHASE_PARKED
		int32_t wpn_phase = INT32_MIN;
		// The footstep scan's consumed playhead: the last wire clip phase
		// whose authored trigger words were queued, and the clip state it
		// belongs to. foot_state -2 = "no clip seen" — the next armed frame
		// seeds the cursor through the scan contract's two forms
		// (adm_root_motion.h): a clip observed at its start scans from -1 so
		// frame 0 fires; one observed mid-clip seeds at the playhead so
		// nothing back-fires.
		int32_t foot_state = -2;
		int32_t foot_phase = -1;
	};

	// Mirrors the retail per-entity lighting fields (Terrain_SetupEffectForEntity
	// @0x5c74a0; sun visibility Entity_ComputeSunVisibility @0x5c6800 -
	// docs/render/render-lighting-re.md).
	struct LightingContext {
		float effect_scale = 1.0f;
		bool interior_lerp = false;
		float light_transfer = 0.0f;
		int interior_bms = 0;
		int interior_section = 0;
	};

	// --- The per-row legs both walks share (entity_presenter.cpp) ---
	// Stance bits gate the MATCHTERRAIN tier (Terrain_RenderSectorEntitiesBySide
	// @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md).
	static void stamp_match_terrain(ObjectModel *model, const float *p, int base);
	// The render-slot facts of the row: the march start's offset, the ride
	// facts (PF_SLOT_RIDE_BITS) with the model of the entity it stands on, which
	// is resolved only for a carrier's refNum-walk rider
	// (renderer::slot_ride_walk_child), the seat facts (PF_SLOT_SEAT_BITS) with
	// the model whose pool-0 walk draws it, and whether only the local view
	// keeps it off the camera (renderer::SlotCasterVisibility).
	void stamp_slot_facts(ObjectModel *model, const float *p, int base) const;
	// The swap fade's six CTRL slots (OBJECT_DESTROY, OBJECT_DESTROY01..05) of a
	// SUBMITTED row: a husked row (PF_HUSK) re-asserts its six phases at each
	// submission, a row that published releases them once at its first
	// submission back intact, and an intact row that never published does no
	// work. `published` is the row's edge: -1 unknown (a cold row releases),
	// 0 nothing held, 1 published.
	void stamp_destroy_phases(ObjectModel *model, const float *p, int base,
			int32_t &published);
	// The same stamp from the values themselves (the husk bit and the six phases).
	void stamp_destroy_values(ObjectModel *model, bool husked, const int32_t *phases,
			int32_t &published);
	// The mounted right-hand collapse rides its own packed field, edge-gated
	// to the value change; true when the model was written.
	static bool stamp_right_hand_collapsed(ObjectModel *model, const float *p,
			int base, int32_t &last_rhc);
	// A person's cut sections (PF_CUT_SECTIONS, the +0x134 word its bone walk
	// folds), edge-gated the same way; true when the model was written.
	static bool stamp_cut_sections(ObjectModel *model, const float *p, int base,
			int64_t &last_cut);
	// Compare-and-refresh the aim payload cache: true when it changed (apply),
	// false when every float is bit-identical to the last applied set. A NaN
	// field never compares equal, so a poisoned row degrades to per-frame
	// application, never to a stale hold.
	static bool aim_payload_changed(const float *p, int base,
			std::array<float, kAimPayloadFloats> &cache, bool &cache_valid);
	// The row owns the model's destroyed-section channel only while it
	// publishes PF_SECTION_MASK_VALID; the occlusion frame owns the model's
	// separate verdict channel, and ObjectModel ORs the two per part like
	// retail's entity+0x138 | g_HiddenSectionMask. One release when a
	// previously owned row stops publishing.
	void stamp_section_mask(ObjectModel *model, const float *p, int base,
			int64_t &last_mask);
	// All four semantic CTRL writers over one typed model (the wire walk's
	// per-row bundle).
	static int wire_controls_apply(ObjectModel *model,
			PresentRowsView snap, int base);

	// --- The placed walk (entity_presenter.cpp) ---
	bool row_plan_is_current(int64_t size, int stride,
			int64_t layout_revision);
	// `p_captured` (present_slot_captures) limits the walk to those models' rows
	// that it did not submit this frame.
	void present_snapshot_impl(PresentRowsView snap, int stride,
			int64_t layout_revision, const int32_t *door_phases, int64_t door_phase_count,
			MissionFrameProfile *p_profile, const HashSet<uint64_t> *p_captured = nullptr);
	void rebuild_row_plan(const float *p, int64_t size, int stride,
			int64_t layout_revision);
	void release_planned_rows();
	void release_part_anim_outputs();
	int64_t current_index_generation();
	Simulation *sim() const;

	// --- The wire walk (entity_presenter_wire.cpp) ---
	Node3D *container() const;
	void begin_wire_plan(int64_t layout_revision, int stride,
			int64_t snapshot_size, int64_t index_generation, int local_handle);
	void append_wire_row(Object *node, int base, int handle, bool spawned_now);
	void append_wire_deferred(Object *node);
	bool wire_plan_is_current(int64_t snapshot_size, int stride,
			int64_t layout_revision, int64_t index_generation, int local_handle);
	// `p_captured` (present_slot_captures) limits the walk to those models' rows
	// that took the culled branch this frame.
	void present_wire_rows(PresentRowsView snap, int stride,
			int tick_delta, const HashSet<uint64_t> *p_captured = nullptr);
	void present_one_wire_row(WireRow &row, ObjectModel *model,
			PresentRowsView snap, int tick_delta);
	// A wire row's root transform: its position under its aim-owned or entity
	// basis, composed with the model's scale.
	static Transform3D wire_row_transform(ObjectModel *model, PresentRowsView snap, int base);
	// Each view's collector verdict on a row's body and its held gun (the
	// models' owner bits; the Inset's through their Inset state).
	void apply_wire_row_views(const WireRow &row, ObjectModel *model, bool p_present_visible,
			bool p_main_culled, bool p_inset_culled);
	void present_wire_row_body_sounds(WireRow &row, PresentRowsView snap);
	void apply_wire_procedural_part(const WireRow &row, ObjectModel *model,
			PresentRowsView snap);
	void apply_wire_body_anim(WireRow &row, ObjectModel *model,
			PresentRowsView snap, int tick_delta);
	void update_wire_held_weapon(WireRow &row, Node3D *node,
			PresentRowsView snap, bool body_visible);
	// The same third-person gun leg for a placed person row (the host's and
	// SP's mission NPCs are placed models the wire walk defers), keyed by the
	// row's wire handle; frees the guns of placed rows the walk no longer visits.
	// `body_present` is the body's present intent; the gun takes the body's
	// occlusion claim, so a render slot that captured the body draws it too.
	void update_placed_held_weapon(const Row &row, ObjectModel *body,
			PresentRowsView snap, bool body_present);
	void retire_unvisited_placed_held_weapons();
	void update_wire_person_overlays(const WireRow &row, ObjectModel *body,
			PresentRowsView snap, bool body_visible);
	// A freed/swapped wire node invalidates the plan and its per-handle caches.
	void release_wire_handle(int handle);
	void reset_wire_plan_state();
	void free_wire_node(int p_handle);
	void free_held_weapon(int p_handle);
	void apply_lighting_context(int p_handle);
	bool wire_node_matches_row(ObjectModel *p_node,
			PresentRowsView p_snap, int p_base, int p_type_id) const;
	int consume_present_logic_tick_delta();
	void frame_spectator_camera();
	// Builds/frees the third-person gun when a body's ADM changes.
	Node3D *rebuild_held_weapon(int p_handle, int p_adm);

	// --- Shared wiring ---
	ObjectID sim_id_;
	Ref<MissionObjectPlacer> placer_;

	// --- Placed walk state ---
	Ref<EntityIndex> index_;
	int output_channels_ = OUTPUT_ALL;
	int64_t stat_moved_ = 0;
	int64_t stat_posed_ = 0;
	int64_t stat_hidden_ = 0;
	int64_t stat_plan_rebuilds_ = 0;
	int64_t stat_transform_builds_ = 0;
	// The transform leg's work on rows no view collected (of the totals above).
	int64_t stat_transform_builds_unsubmitted_ = 0;
	int64_t stat_moved_unsubmitted_ = 0;
	int64_t stat_aim_dispatches_ = 0;
	int64_t stat_rhc_dispatches_ = 0;
	int64_t stat_part_dispatches_ = 0;
	int64_t stat_control_dispatches_ = 0;
	int64_t stat_body_dispatches_ = 0;
	int64_t plan_revision_ = -1;
	int plan_stride_ = 0;
	int64_t plan_snapshot_size_ = -1;
	int64_t plan_index_generation_ = -1;
	uint64_t plan_model_lifetime_generation_ = 0;
	bool plan_dirty_ = true;
	std::vector<Row> rows_;
	std::vector<RagdollRow> ragdoll_rows_;
	bool collect_ragdoll_rows_ = false;
	void note_ragdoll_row(ObjectModel *model, int32_t handle, const float *p, int base);
	// infantry_anim_key(state) allocates its String per call natively; the
	// state->key map is tiny and global-stable, so cache per presenter (both
	// walks read it).

	// --- Wire walk state: the plan + per-row caches ---
	std::vector<WireRow> wire_rows_;
	std::vector<ObjectID> wire_deferred_ids_;
	HashMap<int32_t, opennova::world::WireBodyLatch> wire_remote_body_;
	// The footstep cursor (WireRow::foot_state, foot_phase) per wire handle:
	// a cold re-plan rebuilds every WireRow, and a fresh cursor re-seeds at
	// the playhead, dropping the steps the re-plan frame crossed.
	HashMap<int32_t, Vector2i> wire_foot_cursor_;
	HashMap<int32_t, int32_t> wire_respawn_revisions_;
	HashMap<int32_t, bool> wire_render_culled_;
	HashMap<int32_t, bool> wire_render_culled_inset_;
	bool wire_inset_view_ = false;
	HashMap<int32_t, bool> wire_render_culled_mirror_[2];
	bool wire_mirror_view_[2] = { false, false };
	HashMap<int32_t, int32_t> wire_held_weapon_adm_;
	HashMap<int32_t, ObjectID> wire_held_weapon_ids_;
	// Placed person rows holding a third-person gun: handle -> the placed walk
	// serial that last visited the row.
	HashMap<int32_t, uint64_t> placed_held_weapons_;
	uint64_t placed_walk_serial_ = 0;
	int64_t wire_plan_revision_ = -1;
	int wire_plan_stride_ = 0;
	int64_t wire_plan_snapshot_size_ = -1;
	int64_t wire_plan_index_generation_ = -1;
	int wire_plan_local_handle_ = -1;
	bool wire_plan_dirty_ = true;

	// --- Wire walk state: the cold path + registry ---
	ObjectID container_id_;
	Ref<EntityIndex> defer_index_;
	bool synthetic_origin_only_ = false;
	int cold_spawn_budget_ = DEFAULT_COLD_SPAWN_BUDGET;
	ObjectID camera_id_;
	bool camera_framed_ = false;
	HashMap<int32_t, ObjectID> nodes_;
	HashMap<int32_t, int32_t> unresolved_;
	HashMap<int32_t, ObjectID> weapon_nodes_;
	HashMap<int32_t, String> weapon_graphics_;
	HashMap<int32_t, Ref<PersonOverlayModels>> person_overlays_;
	HashMap<int32_t, LightingContext> lighting_contexts_;
	int pending_spawn_count_ = 0;
	// The cold walk's instrument (--verbose): the new rows' first presents timed apart.
	bool wire_profile_first_presents_ = false;
	uint64_t wire_first_present_usec_ = 0;
	// The parts' instrument (profile_present_wire_rows_view): set for that one walk.
	WireProfile *wire_profile_ = nullptr;
	int64_t last_present_logic_tick_ = -1;
	int64_t stat_spawned_ = 0;
	int64_t stat_unresolved_ = 0;
	int64_t stat_live_ = 0;

	// --- The present passes ---
	void present_scars();
	ScarPresenter *scars() const;
	MissionEnvironment *environment() const;
	std::unique_ptr<FirePresenter> fire_;
	Ref<DestructionPresenter> destruction_;
	Ref<ThrowablePresenter> throwable_;
	Ref<VehicleTrailPresenter> vehicle_trail_;
	ObjectID scars_id_;
	ObjectID environment_id_;
	Vector3 listener_position_ = Vector3(INFINITY, INFINITY, INFINITY);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::EntityPresenter::OutputChannels);
VARIANT_ENUM_CAST(godot::EntityPresenter::MissionProfileSlot);
VARIANT_ENUM_CAST(godot::EntityPresenter::PassProfileSlot);
