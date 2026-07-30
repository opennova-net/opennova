#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <array>
#include <unordered_map>
#include <vector>

namespace godot {

class Node3D;

// The native mission present-pass row walk. MissionPresentPass (GDScript,
// godot/engine/world/mission_present_pass.gd) stays the host-facing component —
// this class owns its hot loop: the revision-bound row plan (resolved node,
// capability bitmask, applied-state caches) and the per-frame walk over the
// sim's flat PackedFloat32Array snapshot, dispatching to each resolved node's
// duck-typed NovaEntityVisual surface (ADR 0007) only on change. Per-row work
// that was ~10+ Variant-boxed reads plus a full plan revalidation in GDScript
// becomes raw pointer arithmetic; the dispatched node calls (set_part_phase,
// play_body_clip_at, ...) keep their GDScript implementations.
//
// The walk itself is host presentation glue over the witnessed per-frame
// cadence (entity submission is evaluated every render frame [orig:
// GameLoop_RenderFrame @ 0x521310 -> Render_ProcessMainSceneFrame]); the
// behavioral semantics it applies are the ones already recorded at each leg's
// GDScript origin and carried over verbatim.
class NovaPresentApplier : public RefCounted {
	GDCLASS(NovaPresentApplier, RefCounted)

public:
	enum OutputChannels {
		OUTPUT_TRANSFORM = 1,
		OUTPUT_PART_ANIM = 2,
		OUTPUT_VISIBILITY = 4,
		OUTPUT_BODY_ANIM = 8,
		OUTPUT_ALL = OUTPUT_TRANSFORM | OUTPUT_PART_ANIM | OUTPUT_VISIBILITY |
				OUTPUT_BODY_ANIM,
	};

	// `sim` is duck-typed (NovaSimulation or a test fake): consulted only for the
	// muzzle feedback push. `index` resolves rows to nodes (MissionEntityRegistry
	// or a fake); called only on plan rebuilds plus one get_generation per frame.
	void setup(Object *sim, Object *index);
	void set_output_channels(int channels);
	int get_output_channels() const { return output_channels_; }
	// Shared BY REFERENCE with the host (GameWorld mutates the hidden set in
	// place): the two-bit visibility ownership seam — occlusion may keep a node
	// hidden that the sim wants visible; the release lands on the sim's intent.
	void set_shared_visibility_maps(const Dictionary &occlusion_hidden_ids,
			const Dictionary &present_visibility);

	void present_snapshot(const PackedFloat32Array &snap, int stride,
			int64_t layout_revision);

	Dictionary get_stats() const;

	// The one placement convention, ported beside its GDScript origin
	// (MissionObjectPlacer.bms_to_godot_basis — see the [orig] block there:
	// Entity_SpawnFromBMSRecord @ 0x40eb66 + Math_BuildFixedPointMatrixFromEulerAngles
	// @ 0x613f40 via Entity_UpdateOrientationMatrix @ 0x43b440; R_godot =
	// RotY(90-yaw) * RotZ(pitch) * RotX(roll) * RotY(90)). The two ports are
	// pinned equivalent by mission_present_pass_test.gd's basis parity case —
	// keep them in lockstep.
	static Basis bms_to_godot_basis(const Vector3 &rot_deg);

	// Aim-overlay presentation (aim_overlay_present_pass.gd delegates here so the
	// mission and wire passes share one implementation). root_basis: aim-valid
	// rows own the body rotation; others keep the fallback entity rotation.
	static Basis aim_root_basis(const PackedFloat32Array &snap, int base,
			const Basis &fallback);
	static void aim_apply(Object *node, const PackedFloat32Array &snap, int base,
			bool drive_root_basis);
	static void aim_apply_valid(Object *node, const PackedFloat32Array &snap,
			int base, bool drive_root_basis);

	// Emplaced-weapon CTRL registers (emplaced_weapon_present_pass.gd delegates
	// here): EWEAP_GUNYAW/EWEAP_GUNPITCH only — clear_ctrl_values() would also
	// erase live WAC channels.
	static int emplaced_apply(Object *node, const PackedFloat32Array &snap,
			int base, bool clear_when_invalid);
	static void emplaced_clear(Object *node);
	// Authoritative ground-vehicle VEHICLE_STEERING/VEHICLE_SPEED pair.
	// Invalid compact/non-vehicle rows release only this semantic writer.
	static int vehicle_motion_apply(Object *node,
			const PackedFloat32Array &snap, int base);
	static void vehicle_motion_clear(Object *node);
	// Bounded per-model projection of the retail sector/generic-zone CTRL
	// writers. Validity bits distinguish an omitted global-bus write from a
	// literal zero store; clears affect only these presentation owners.
	static int zone_team_apply(Object *node,
			const PackedFloat32Array &snap, int base);
	static void zone_team_clear(Object *node);
	// Attachment-scoped carrier HEAT_GLOW. A valid row includes cold zero;
	// invalid/unavailable rows release only this dedicated writer.
	static int world_heat_apply(Object *node, const PackedFloat32Array &snap,
			int base);
	static void world_heat_clear(Object *node);

protected:
	static void _bind_methods();

private:
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
		int caps = 0;
		int32_t bms_id = 0;
		// Last-applied edge state (-1 = unknown, first frame always applies).
		int32_t aim_valid = -1;
		int32_t rhc = -1;
		int32_t present_visible = -1;
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

	bool row_plan_is_current(int64_t size, int stride,
			int64_t layout_revision);
	void rebuild_row_plan(const float *p, int64_t size, int stride,
			int64_t layout_revision);
	void release_part_anim_outputs();
	int64_t current_index_generation();
	const String &infantry_key(int state);

	ObjectID sim_id_;
	ObjectID index_id_;
	bool index_has_generation_ = false;
	int output_channels_ = OUTPUT_ALL;
	Dictionary occlusion_hidden_ids_;
	Dictionary present_visibility_;

	int64_t stat_moved_ = 0;
	int64_t stat_posed_ = 0;
	int64_t stat_hidden_ = 0;
	int64_t stat_muzzles_ = 0;
	int64_t stat_plan_rebuilds_ = 0;
	int64_t stat_transform_builds_ = 0;
	int64_t stat_aim_dispatches_ = 0;
	int64_t stat_rhc_dispatches_ = 0;
	int64_t stat_part_dispatches_ = 0;
	int64_t stat_control_dispatches_ = 0;
	int64_t stat_body_dispatches_ = 0;
	int64_t stat_muzzle_queries_ = 0;

	int64_t plan_revision_ = -1;
	int plan_stride_ = 0;
	int64_t plan_snapshot_size_ = -1;
	int64_t plan_index_generation_ = -1;
	bool plan_dirty_ = true;
	std::vector<Row> rows_;

	// infantry_anim_key(state) allocates its String per call natively; the
	// state->key map is tiny and global-stable, so cache per applier.
	std::unordered_map<int, String> infantry_keys_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaPresentApplier::OutputChannels);
