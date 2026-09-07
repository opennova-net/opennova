#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <runtime/simassets/sim_pose_provider.h>
#include <runtime/world/minefield.h>

#include "object/entity_index.h"
#include "object/item_database.h"
#include "object/object_model.h"
#include "mission/mission_object_placer.h"
#include "resource_index/resource_root.h"
#include "simulation/destruction_events.h"
#include "simulation/destruction_presenter.h"
#include "simulation/fire_presenter.h"
#include "simulation/present_event_records.h"
#include "simulation/present_stats.h"
#include "simulation/throwable_presenter.h"
#include "simulation/vehicle_wake_presenter.h"

namespace godot {

class EffectLightDirector;
class EffectWorld;
class ItemEffectDirector;
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
// engine header (npruntime/wire_present.h).
//
// Beside the two row walks it OWNS the four tick-driven present passes (ADR
// 0043 d9): FirePresenter (AI/remote fire sound + muzzle effect + tracers),
// DestructionPresenter (husk swaps, death pieces, wreck effects) and
// ThrowablePresenter (flying/placed throwable models + the round-bound move
// groups) as members, and the "Scars" ScarPresenter child (the impact-scar
// rings). The driver runs the row walks, then present_passes() — fire ->
// destruction -> throwable -> scars, scars LAST: the entity-ring meshes
// parent under section nodes the row walks built and the husk grafts onto the
// placed/wire models. A zero-tick frame runs only the row walks. The
// Stop -> Play boundary (reset_wire_runtime_state) resets the passes' runtime
// state with the wire registry.
class EntityPresenter : public Node3D {
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
				opennova::simassets::kHeldWeaponBoneIndex,
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
	// walks (present_snapshot, then present_wire_snapshot).
	void present_snapshot(const PackedFloat32Array &snap, int stride,
			int64_t layout_revision);
	PackedInt64Array profile_present_snapshot(const PackedFloat32Array &snap,
			int stride, int64_t layout_revision);

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

	// The occlusion frame's render-gate verdict for one wire row (the
	// collector gate the placed rows get by bms id). A culled row's node
	// hides and skips its presentation legs until the gate releases it.
	void set_render_culled(int p_handle, bool p_culled);
	// Forget every render-gate verdict (paired with the sim's applied-state
	// baseline reset: the next frame re-emits the full set).
	void clear_render_culled() { wire_render_culled_.clear(); }

	// Cold rows the budget deferred on the last presented frame.
	int pending_spawn_count() const { return pending_spawn_count_; }
	Ref<WirePresentStats> get_wire_stats_record() const;

	// The live model owned by this presenter for a packed pool/slot handle
	// (runtime-only entities have no authored BMS identity). Freed entries are
	// ordinary (world teardown frees the container's children first).
	ObjectModel *resolve_wire_handle(int p_handle) const;
	// The live held-weapon model for a wire body (null when unarmed/freed).
	ObjectModel *held_weapon_node(int p_handle) const;
	// Cache and apply one wire draw's environment-lighting context to both its
	// body and held weapon. The cache makes a quality change authoritative even
	// when cold-spawn budgeting has not built either node yet.
	void set_entity_lighting_context(int p_handle, float p_effect_scale,
			bool p_interior_lerp, float p_light_transfer);
	// World position of a named userpoint on this wire body's HELD WEAPON —
	// the anchor retail's adm-arm fire effect spawns at. Falls back to the
	// body's own origin, never the wire fire position (witness:
	// npruntime/wire_present.h ledger, rigid weapon draw + userpoint
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
	// verdicts, then reset the present passes' runtime state (the Stop ->
	// Play boundary; the sim's restart signal).
	void reset_wire_runtime_state();
	// Wire teardown plus the placed plan's release and the passes' teardown
	// (the node is going away).
	void teardown();

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
	// gates the fire/destruction sound legs — the dedicated-host tri-state: no
	// audio, no sounds, the effect legs still run; `fx` (nullable) the effect
	// legs; `lights` (nullable) the MF_Light muzzle glow and the death flash;
	// `environment` (nullable) the scar fog cull + terrain light; `anchors`
	// (nullable) the owner-anchor registry the wreck/piece/move effect groups
	// anchor through.
	void setup_passes(Node3D *p_container, const Ref<ItemDatabase> &p_item_db,
			const Ref<ResourceRoot> &p_resource_root, MissionAudio *p_audio,
			EffectWorld *p_fx, EffectLightDirector *p_lights,
			MissionEnvironment *p_environment, ItemEffectDirector *p_anchors);
	// The presenting shell's listener: the camera position the driver pushes
	// once per present frame (MissionFrameInput.set_camera_sample carries the
	// same sample to the sim's fire-sound distance gate, world/fire_sound.h);
	// fire's ribbon camera and scar's fog-box cull both read it. A host with
	// no camera (a dedicated serve) never pushes, which is the witnessed peer
	// gate [orig: @ 0x528e57]. Non-finite until the first push.
	void set_listener_position(const Vector3 &p_position);
	Vector3 listener_position() const { return listener_position_; }
	// The four passes in drive order, once per ticked present frame.
	void present_passes();
	// present_passes() timed per pass (PassProfileSlot, microseconds) for the
	// stats board.
	PackedInt64Array profile_present_passes();
	// Throwable's fixed-tick half: the round-bound move groups reconcile at the
	// tick sink BEFORE the effect world advances (the model reconcile stays in
	// present_passes).
	void sync_fixed_tick_effects();
	Ref<FirePresentStats> get_fire_present_stats() const;
	Ref<DestructionPresentStats> get_destruction_present_stats() const;
	// Built per call (the live census).
	Ref<ThrowablePresentStats> get_throwable_present_stats() const;
	Ref<ScarPresentStats> get_scar_present_stats() const;
	bool has_active_wreck_fire(const String &p_owner_key) const;
	void warm_fire_pipelines(const Vector3 &p_position);
	Ref<ImmediateMesh> fire_ribbon_mesh() const;
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
	void present_throwable_visuals(const TypedArray<ThrowableVisualRow> &p_visuals);
	void present_vehicle_wake_visuals(const TypedArray<VehicleWakeVisualRow> &p_visuals);
	void present_scar_draw_list(const Ref<ScarDrawList> &p_draw_list);

	// --- Statics shared by both walks and their consumers --------------------

	// Aim-overlay presentation (the local avatar and the tests delegate here
	// so every body shares one implementation). root_basis: aim-valid rows own
	// the body rotation; others keep the fallback entity rotation.
	static Basis aim_root_basis(const PackedFloat32Array &snap, int base,
			const Basis &fallback);
	static void aim_apply(Object *node, const PackedFloat32Array &snap, int base,
			bool drive_root_basis);
	static void aim_apply_valid(Object *node, const PackedFloat32Array &snap,
			int base, bool drive_root_basis);

	// Third-person held-weapon placement — the ONE home (the witnessed
	// calibration constants and the full derivation live at engine
	// simassets/sim_pose_provider.h). `body` may be the body root or the
	// skeleton itself (resolved via find_skeleton); returns a Transform3D, or
	// null when the skeleton cannot place one.
	static Variant held_weapon_attach_transform(Object *body,
			const Vector3 &attach_angles_bms, bool hand_frame);
	static Basis held_weapon_hand_frame_basis(const Basis &bone_model_to_world);
	// The first Skeleton3D under `root` (or `root` itself), or null.
	static Object *find_skeleton(Object *root);
	// The engine calibration re-exported for tests/tooling (float/vector
	// values cannot be class constants).
	static Vector3 held_weapon_attach_nudge();
	static double held_weapon_hand_frame_z_rad();
	static double held_weapon_hand_frame_y_rad();

	// Emplaced-weapon CTRL registers (bound statics = the coop_two_sim test's
	// seam onto the native leg): EWEAP_GUNYAW/EWEAP_GUNPITCH only —
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
	enum BodyDispatchMode {
		BODY_NONE = 0,
		BODY_CLIP_AT,
		BODY_BLEND_AT,
		BODY_SLOT_AT,
		BODY_SLOT_PLAY,
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
		// Last-applied edge state (-1 = unknown, first frame always applies).
		int32_t aim_valid = -1;
		int32_t rhc = -1;
		int64_t section_visibility_mask = -2;
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
		bool body_stamp_valid = false;
		int32_t body_mode = BODY_NONE;
		int32_t body_selector = -1;
		int32_t body_phase = 0;
		int32_t body_source_selector = -1;
		int32_t body_source_phase = 0;
		float body_blend_weight = 1.0f;
	};

	struct WireRow {
		int base = 0;
		int handle = 0;
		ObjectID node_id;
		bool spawned_now = false;
		// Last-applied edge state (-1 = unknown, first hot frame applies).
		int32_t aim_valid = -1;
		int32_t rhc = -1;
		int64_t section_visibility_mask = -2;
		// The remote body-transition scalars (re-seeded from the per-handle
		// cache on every plan build).
		int32_t anim_state = -2;
		int32_t anim_request = -1;
		int32_t remote_body_tick = 0;
		// Per-leg input caches for the measured every-frame legs (the 2026-08-04
		// joiner profile: ~21us/row ungated over a full streamed world). Each
		// holds the exact PF fields its leg consumes; an invalid cache forces
		// the first application, and a cold plan rebuild re-applies once by
		// construction (rows rebuild with invalid caches). The CTRL field list
		// lives beside its leg in entity_presenter_wire.cpp; the aim cache is
		// the same contiguous payload the placed walk compares.
		static constexpr int kCtrlCacheCount = 32;
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

	struct RemoteBodyCache {
		int32_t state = -2;
		int32_t request = -1;
		int32_t latch = 0;
	};

	// Mirrors the retail per-entity lighting fields (setup_terrain_effect_for_entity
	// @0x5c74a0; sun visibility Entity_ComputeSunVisibility @0x5c6800 -
	// docs/render/render-lighting-re.md).
	struct LightingContext {
		float effect_scale = 1.0f;
		bool interior_lerp = false;
		float light_transfer = 0.0f;
	};

	// --- The per-row legs both walks share (entity_presenter.cpp) ---
	// Stance bits gate the MATCHTERRAIN tier (Terrain_RenderSectorEntitiesBySide
	// @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md).
	static void stamp_match_terrain(ObjectModel *model, const float *p, int base);
	// The mounted right-hand collapse rides its own packed field, edge-gated
	// to the value change; true when the model was written.
	static bool stamp_right_hand_collapsed(ObjectModel *model, const float *p,
			int base, int32_t &last_rhc);
	// Compare-and-refresh the aim payload cache: true when it changed (apply),
	// false when every float is bit-identical to the last applied set. A NaN
	// field never compares equal, so a poisoned row degrades to per-frame
	// application, never to a stale hold.
	static bool aim_payload_changed(const float *p, int base,
			std::array<float, kAimPayloadFloats> &cache, bool &cache_valid);
	// The row owns the model's section-mask channel only while it publishes
	// PF_SECTION_MASK_VALID. Rows that never publish must not touch the
	// channel at all — the occlusion frame pass drives the same ObjectModel
	// call for buildings, and an unconditional release here would stomp its
	// applied mask after a plan rebuild. One release when a previously owned
	// row stops publishing.
	static void stamp_section_mask(ObjectModel *model, const float *p, int base,
			int64_t &last_mask);
	// All four semantic CTRL writers over one typed model (the wire walk's
	// per-row bundle).
	static int wire_controls_apply(ObjectModel *model,
			const PackedFloat32Array &snap, int base);

	// --- The placed walk (entity_presenter.cpp) ---
	bool row_plan_is_current(int64_t size, int stride,
			int64_t layout_revision);
	void present_snapshot_impl(const PackedFloat32Array &snap, int stride,
			int64_t layout_revision, MissionFrameProfile *p_profile);
	void rebuild_row_plan(const float *p, int64_t size, int stride,
			int64_t layout_revision);
	void release_planned_rows();
	void release_part_anim_outputs();
	int64_t current_index_generation();
	const String &infantry_key(int state);
	Simulation *sim() const;

	// --- The wire walk (entity_presenter_wire.cpp) ---
	Node3D *container() const;
	void begin_wire_plan(int64_t layout_revision, int stride,
			int64_t snapshot_size, int64_t index_generation, int local_handle);
	void append_wire_row(Object *node, int base, int handle, bool spawned_now);
	void append_wire_deferred(Object *node);
	bool wire_plan_is_current(int64_t snapshot_size, int stride,
			int64_t layout_revision, int64_t index_generation, int local_handle);
	void present_wire_rows(const PackedFloat32Array &snap, int stride,
			int tick_delta);
	void present_one_wire_row(WireRow &row, ObjectModel *model,
			const PackedFloat32Array &snap, int tick_delta);
	void present_wire_row_body_sounds(WireRow &row, const PackedFloat32Array &snap);
	void apply_wire_procedural_part(const WireRow &row, ObjectModel *model,
			const PackedFloat32Array &snap);
	void apply_wire_body_anim(WireRow &row, ObjectModel *model,
			const PackedFloat32Array &snap, int tick_delta);
	void store_wire_remote_body_cache(const WireRow &row);
	void update_wire_held_weapon(WireRow &row, Node3D *node,
			const PackedFloat32Array &snap, bool body_visible);
	// A freed/swapped wire node invalidates the plan and its per-handle caches.
	void release_wire_handle(int handle);
	void reset_wire_plan_state();
	void free_wire_node(int p_handle);
	void free_held_weapon(int p_handle);
	void apply_lighting_context(int p_handle);
	bool wire_node_matches_row(ObjectModel *p_node,
			const PackedFloat32Array &p_snap, int p_base, int p_type_id) const;
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
	// infantry_anim_key(state) allocates its String per call natively; the
	// state->key map is tiny and global-stable, so cache per presenter (both
	// walks read it).
	std::unordered_map<int, String> infantry_keys_;

	// --- Wire walk state: the plan + per-row caches ---
	std::vector<WireRow> wire_rows_;
	std::vector<ObjectID> wire_deferred_ids_;
	HashMap<int32_t, RemoteBodyCache> wire_remote_body_;
	HashMap<int32_t, int32_t> wire_respawn_revisions_;
	HashMap<int32_t, bool> wire_render_culled_;
	HashMap<int32_t, int32_t> wire_held_weapon_adm_;
	HashMap<int32_t, ObjectID> wire_held_weapon_ids_;
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
	HashMap<int32_t, LightingContext> lighting_contexts_;
	int pending_spawn_count_ = 0;
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
	Ref<VehicleWakePresenter> vehicle_wake_;
	ObjectID scars_id_;
	ObjectID environment_id_;
	Vector3 listener_position_ = Vector3(INFINITY, INFINITY, INFINITY);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::EntityPresenter::OutputChannels);
VARIANT_ENUM_CAST(godot::EntityPresenter::MissionProfileSlot);
VARIANT_ENUM_CAST(godot::EntityPresenter::PassProfileSlot);
