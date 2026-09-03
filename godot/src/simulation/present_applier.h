#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <array>
#include <unordered_map>
#include <vector>

#include <runtime/simassets/sim_pose_provider.h>

#include "object/entity_index.h"
#include "object/object_model.h"
#include "mission/mission_object_placer.h"

namespace godot {

class Simulation;

// The native mission present-pass row walk — the one home of the mission
// present pass (the former mission_present_pass.gd facade dissolved here) —
// owning its hot loop: the revision-bound row plan (resolved typed
// ObjectModel, applied-state caches) and the per-frame walk over the
// sim's flat PackedFloat32Array snapshot, dispatching direct C++ calls only
// on change. There is no name-based dispatch and no capability probing:
// the model class IS the contract.
//
// The walk itself is shell presentation glue over the witnessed per-frame
// cadence (entity submission is evaluated every render frame [orig:
// GameLoop_RenderFrame @ 0x521310 -> Render_ProcessMainSceneFrame]); the
// behavioral semantics it applies are the ones already recorded at each leg's
// GDScript origin and carried over verbatim.
class PresentApplier : public RefCounted {
	GDCLASS(PresentApplier, RefCounted)

public:
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

	// `sim` exposes native lazy muzzle resolution plus the no-native-rig push-back
	// fallback (typed Simulation; converted once at this boundary). `index` resolves
	// rows to typed models — a real EntityIndex, in tests too.
	void setup(Object *sim, Object *index,
			const Ref<MissionObjectPlacer> &placer = Ref<MissionObjectPlacer>());
	void set_output_channels(int channels);
	int get_output_channels() const { return output_channels_; }
	// Shared BY REFERENCE with the shell (GameWorld mutates the hidden set in
	// place): the two-bit visibility ownership seam — occlusion may keep a node
	// hidden that the sim wants visible; the release lands on the sim's intent.
	void set_shared_visibility_maps(const Dictionary &occlusion_hidden_ids,
			const Dictionary &present_visibility);

	// Pull the current snapshot from the wired sim and apply it (the
	// self-driving form; the runtime driver pre-fetches one shared snapshot
	// and calls present_snapshot instead).
	void present();
	void present_snapshot(const PackedFloat32Array &snap, int stride,
			int64_t layout_revision);
	PackedInt64Array profile_present_snapshot(const PackedFloat32Array &snap,
			int stride, int64_t layout_revision);

	Ref<class MissionPresentStats> get_stats_record() const;

	// Aim-overlay presentation (aim_overlay_present_pass.gd delegates here so the
	// mission and wire passes share one implementation). root_basis: aim-valid
	// rows own the body rotation; others keep the fallback entity rotation.
	static Basis aim_root_basis(const PackedFloat32Array &snap, int base,
			const Basis &fallback);
	static void aim_apply(Object *node, const PackedFloat32Array &snap, int base,
			bool drive_root_basis);
	static void aim_apply_valid(Object *node, const PackedFloat32Array &snap,
			int base, bool drive_root_basis);

	// --- The WIRE (joiner/MP) walk: plan + per-row hot path (the native
	// WirePresentPass (wire_present_pass.cpp) keeps the cold
	// spawn/defer/prune path and pushes the finished plan here;
	// present_applier_wire.cpp holds the bodies).

	// `rebuild_held_weapon(handle, adm) -> Node3D|null` stays on the facade,
	// which owns the sim graphic resolve and the weapon-node maps its consumers
	// (muzzle_world_for, tests) read.
	void setup_wire(const Callable &rebuild_held_weapon);
	void begin_wire_plan(int64_t layout_revision, int stride,
			int64_t snapshot_size, int64_t index_generation, int local_handle);
	void append_wire_row(Object *node, int base, int handle, bool spawned_now);
	void append_wire_deferred(Object *node);
	bool wire_plan_is_current(int64_t snapshot_size, int stride,
			int64_t layout_revision, int64_t index_generation, int local_handle);
	void present_wire_rows(const PackedFloat32Array &snap, int stride,
			int tick_delta);
	// A freed/swapped wire node invalidates the plan and its per-handle caches.
	void release_wire_handle(int handle);
	// The occlusion frame's render-gate verdict for a wire row.
	void set_wire_render_culled(int handle, bool culled);
	void clear_wire_render_culled() { wire_render_culled_.clear(); }
	void reset_wire_runtime_state();

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
	enum HeldWeaponConstants {
		HELD_WEAPON_BONE_INDEX =
				opennova::simassets::kHeldWeaponBoneIndex,
	};

	// Emplaced-weapon CTRL registers (bound statics = the coop_two_sim test's
	// seam onto the native leg): EWEAP_GUNYAW/EWEAP_GUNPITCH only —
	// a bulk CTRL clear would also erase live WAC channels.
	static int emplaced_apply(Object *node, const PackedFloat32Array &snap,
			int base, bool clear_when_invalid);

protected:
	static void _bind_methods();

private:
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
		int32_t present_visible = -1;
		int64_t section_visibility_mask = -2;
		bool transform_stamp_valid = false;
		std::array<float, 6> transform_stamp = {};
		bool aim_payload_valid = false;
		std::array<float, 30> aim_payload = {};
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
		// construction (rows rebuild with invalid caches). The field lists live
		// beside the legs in present_applier_wire.cpp.
		static constexpr int kCtrlCacheCount = 21;
		static constexpr int kAimCacheCount = 30;
		float ctrl_cache[kCtrlCacheCount];
		float aim_cache[kAimCacheCount];
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

	// All four semantic CTRL writers over one typed model (the wire walk's
	// per-row bundle).
	static int wire_controls_apply(ObjectModel *model,
			const PackedFloat32Array &snap, int base);

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

	void present_one_wire_row(WireRow &row, ObjectModel *model,
			const PackedFloat32Array &snap, int tick_delta);
	void apply_wire_procedural_part(const WireRow &row, ObjectModel *model,
			const PackedFloat32Array &snap);
	void apply_wire_body_anim(WireRow &row, ObjectModel *model,
			const PackedFloat32Array &snap, int tick_delta);
	void store_wire_remote_body_cache(const WireRow &row);
	void update_wire_held_weapon(WireRow &row, Node3D *node,
			const PackedFloat32Array &snap, bool body_visible);

	Callable wire_rebuild_held_weapon_;
	std::vector<WireRow> wire_rows_;
	std::vector<ObjectID> wire_deferred_ids_;
	HashMap<int32_t, RemoteBodyCache> wire_remote_body_;
	HashMap<int32_t, int32_t> wire_respawn_revisions_;
	HashMap<int32_t, bool> wire_render_culled_;
	void present_wire_row_body_sounds(WireRow &row, const PackedFloat32Array &snap);
	HashMap<int32_t, int32_t> wire_held_weapon_adm_;
	HashMap<int32_t, ObjectID> wire_held_weapon_ids_;
	int64_t wire_plan_revision_ = -1;
	int wire_plan_stride_ = 0;
	int64_t wire_plan_snapshot_size_ = -1;
	int64_t wire_plan_index_generation_ = -1;
	int wire_plan_local_handle_ = -1;
	bool wire_plan_dirty_ = true;

	ObjectID sim_id_;
	Ref<EntityIndex> index_;
	Ref<MissionObjectPlacer> placer_;
	int output_channels_ = OUTPUT_ALL;
	Dictionary occlusion_hidden_ids_;
	Dictionary present_visibility_;
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
	// state->key map is tiny and global-stable, so cache per applier.
	std::unordered_map<int, String> infantry_keys_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::PresentApplier::OutputChannels);
VARIANT_ENUM_CAST(godot::PresentApplier::MissionProfileSlot);
