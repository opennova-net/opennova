#include "simulation/present_applier.h"
#include "util/axes.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <algorithm>

#include <runtime/mission/placement_traits.h>
#include <runtime/simassets/sim_pose_provider.h>

#include "simulation/present_stats.h"
#include "simulation/simulation.h"

using namespace godot;

namespace {

// CTRL register names + presentation-owner tags (String VALUES handed to the
// model's owner-aware CTRL store — not dispatch names; the dispatch itself is
// direct C++ calls on ObjectModel).
struct CtrlNames {
	String eweap_gunyaw = String("EWEAP_GUNYAW");
	String eweap_gunpitch = String("EWEAP_GUNPITCH");
	String vehicle_steering = String("VEHICLE_STEERING");
	String vehicle_speed = String("VEHICLE_SPEED");
	// The part-animation registers the same cveh callback publishes (catalog
	// ordinals 46 / 47 / 60, engine/formats/threedi/threedi_ctrl_catalog.h).
	String helo_rotor = String("HELO_ROTOR");
	String helo_tailrotor = String("HELO_TAILROTOR");
	String vehicle_wheels = String("VEHICLE_WHEELS");
	String tex_team = String("TEX_TEAM");
	String team_swing = String("TEAMSWING");
	String lfp_camp_percent = String("LFP_CAMPPERCENT");
	String heat_glow = String("HEAT_GLOW");
	String owner_emplaced = String("present:emplaced");
	String owner_vehicle_motion = String("present:vehicle_motion");
	String owner_sector_team = String("present:sector_team");
	String owner_zone = String("present:zone");
	String owner_world_heat = String("present:world_heat");
};

const CtrlNames &names() {
	static CtrlNames n;
	return n;
}

inline int32_t field_i(const float *p, int base, int field) {
	return static_cast<int32_t>(p[base + field]);
}

void set_owned_ctrl(ObjectModel *model, const String &owner,
		const String &reg, int32_t value) {
	model->set_ctrl_override(owner, reg, value);
}

void clear_owned_ctrl(ObjectModel *model, const String &owner,
		const String &reg) {
	model->clear_ctrl_override(owner, reg);
}

constexpr int AIM_PAYLOAD_FLOATS =
		Simulation::PF_EMPLACED_CONTROLS_VALID -
		Simulation::PF_AIM_BODY_PITCH_DEG;
static_assert(AIM_PAYLOAD_FLOATS == 30,
		"aim cache must cover body Euler plus all nine overlay triples");

} // namespace

void PresentApplier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "sim", "index", "placer"),
			&PresentApplier::setup, DEFVAL(Ref<MissionObjectPlacer>()));
	ClassDB::bind_method(D_METHOD("set_output_channels", "channels"),
			&PresentApplier::set_output_channels);
	ClassDB::bind_method(D_METHOD("get_output_channels"),
			&PresentApplier::get_output_channels);
	ClassDB::bind_method(
			D_METHOD("set_shared_visibility_maps", "occlusion_hidden_ids",
					"present_visibility"),
			&PresentApplier::set_shared_visibility_maps);
	ClassDB::bind_method(
			D_METHOD("present_snapshot", "snap", "stride", "layout_revision"),
			&PresentApplier::present_snapshot);
	ClassDB::bind_method(
			D_METHOD("profile_present_snapshot", "snap", "stride",
					"layout_revision"),
			&PresentApplier::profile_present_snapshot);
	ClassDB::bind_method(D_METHOD("get_stats_record"),
			&PresentApplier::get_stats_record);
	ClassDB::bind_method(D_METHOD("present"), &PresentApplier::present);
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("held_weapon_attach_transform", "body", "attach_angles_bms",
					"hand_frame"),
			&PresentApplier::held_weapon_attach_transform, DEFVAL(false));
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("held_weapon_hand_frame_basis", "bone_model_to_world"),
			&PresentApplier::held_weapon_hand_frame_basis);
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("find_skeleton", "root"), &PresentApplier::find_skeleton);
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("held_weapon_attach_nudge"),
			&PresentApplier::held_weapon_attach_nudge);
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("held_weapon_hand_frame_z_rad"),
			&PresentApplier::held_weapon_hand_frame_z_rad);
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("held_weapon_hand_frame_y_rad"),
			&PresentApplier::held_weapon_hand_frame_y_rad);
	BIND_CONSTANT(HELD_WEAPON_BONE_INDEX);
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("aim_root_basis", "snap", "base", "fallback"),
			&PresentApplier::aim_root_basis);
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("aim_apply", "node", "snap", "base", "drive_root_basis"),
			&PresentApplier::aim_apply, DEFVAL(true));
	ClassDB::bind_static_method("PresentApplier",
			D_METHOD("emplaced_apply", "node", "snap", "base", "clear_when_invalid"),
			&PresentApplier::emplaced_apply);
	BIND_ENUM_CONSTANT(OUTPUT_TRANSFORM);
	BIND_ENUM_CONSTANT(OUTPUT_PART_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_VISIBILITY);
	BIND_ENUM_CONSTANT(OUTPUT_BODY_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_ALL);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_CORE_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_AIM_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_CONTROLS_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_VISIBILITY_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_BODY_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_ROWS);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_SUBMITTED_ROWS);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_BODY_ROWS);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_SLOT_COUNT);
}

void PresentApplier::setup(Object *sim, Object *index,
		const Ref<MissionObjectPlacer> &placer) {
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0) {
		release_part_anim_outputs();
	}
	Simulation *native_sim = Object::cast_to<Simulation>(sim);
	sim_id_ = native_sim != nullptr ? native_sim->get_instance_id() : ObjectID();
	index_ = Ref<EntityIndex>(Object::cast_to<EntityIndex>(index));
	placer_ = placer;
	plan_revision_ = -1; // force a rebuild against the new wiring
	plan_dirty_ = true;
	release_planned_rows();
}

// Retained rows stop being "planned" the moment the plan drops them, so a model
// that later leaves the mission (a despawned row kept alive as a preview) no
// longer moves the lifetime stamp on death.
void PresentApplier::release_planned_rows() {
	for (const Row &row : rows_) {
		ObjectModel *model =
				Object::cast_to<ObjectModel>(ObjectDB::get_instance(row.node_id));
		if (model != nullptr) {
			model->set_present_planned(false);
		}
	}
	rows_.clear();
}

void PresentApplier::set_output_channels(int channels) {
	const int next = channels & OUTPUT_ALL;
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0 &&
			(next & OUTPUT_PART_ANIM) == 0) {
		// Turning a presentation seam off must release its retained writers;
		// otherwise the last pose survives indefinitely on persistent nodes.
		release_part_anim_outputs();
	}
	const int rising = next & ~output_channels_;
	output_channels_ = next;
	if (rising == 0) {
		return;
	}
	for (Row &row : rows_) {
		if ((rising & OUTPUT_TRANSFORM) != 0) {
			row.transform_stamp_valid = false;
		}
		if ((rising & OUTPUT_PART_ANIM) != 0) {
			row.ctrl_publish_state_valid = false;
		}
		if ((rising & OUTPUT_BODY_ANIM) != 0) {
			row.body_stamp_valid = false;
		}
	}
}

void PresentApplier::set_shared_visibility_maps(
		const Dictionary &occlusion_hidden_ids, const Dictionary &present_visibility) {
	occlusion_hidden_ids_ = occlusion_hidden_ids;
	present_visibility_ = present_visibility;
}

Ref<MissionPresentStats> PresentApplier::get_stats_record() const {
	Ref<MissionPresentStats> stats;
	stats.instantiate();
	stats->moved = stat_moved_;
	stats->posed = stat_posed_;
	stats->hidden = stat_hidden_;
	stats->plan_rebuilds = stat_plan_rebuilds_;
	stats->transform_builds = stat_transform_builds_;
	stats->aim_dispatches = stat_aim_dispatches_;
	stats->rhc_dispatches = stat_rhc_dispatches_;
	stats->part_dispatches = stat_part_dispatches_;
	stats->control_dispatches = stat_control_dispatches_;
	stats->body_dispatches = stat_body_dispatches_;
	return stats;
}

void PresentApplier::present() {
	Simulation *native_sim =
			Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
	if (native_sim == nullptr || index_.is_null()) {
		return;
	}
	const int stride = native_sim->get_present_stride();
	if (stride <= 0) {
		return;
	}
	present_snapshot(native_sim->get_present_snapshot(), stride,
			native_sim->get_present_layout_revision());
}

namespace {

// The held weapon placement — the calibration and the full derivation live at
// pivot nudge in raw def units, X negated into the render frame — the values
// engine simassets/sim_pose_provider.h (the sim-side muzzle shares them)
// [orig: flt_7C68E8 = 0.05 +X/-Y, flt_7C9BA8 = 0.051 +Z @ 0x4b2186].
constexpr int kHeldWeaponBoneIndex = opennova::simassets::kHeldWeaponBoneIndex;
const Vector3 kHeldWeaponAttachNudge(opennova::simassets::kHeldWeaponAttachNudgeX,
		opennova::simassets::kHeldWeaponAttachNudgeY,
		opennova::simassets::kHeldWeaponAttachNudgeZ);
// Hand-frame calibration [orig: Rz dbl_7C9BA0 / Ry dbl_7C9B98 via
// Math_BuildRotationMatrix4x4_ByAxis @ 0x611db0].
constexpr double kHandFrameZRad = opennova::simassets::kHeldWeaponHandFrameZRad;
constexpr double kHandFrameYRad = opennova::simassets::kHeldWeaponHandFrameYRad;

} // namespace

Basis PresentApplier::held_weapon_hand_frame_basis(const Basis &bone_model_to_world) {
	// Row-major `Ry_e · Rz_e · M16` = the calibrations on the RIGHT in column
	// form; signs as authored (two inversions cancel — the simassets ledger
	// documents why).
	return bone_model_to_world * Basis(Vector3(0, 0, 1), kHandFrameZRad) *
			Basis(Vector3(0, 1, 0), kHandFrameYRad);
}

Variant PresentApplier::held_weapon_attach_transform(Object *body,
		const Vector3 &attach_angles_bms, bool hand_frame) {
	Skeleton3D *skel = Object::cast_to<Skeleton3D>(find_skeleton(body));
	if (skel == nullptr || skel->get_bone_count() <= kHeldWeaponBoneIndex) {
		return Variant();
	}
	// `M16 · pivot16` IS the joint world position; the nudge rides bone 16's
	// MODEL->WORLD rotation (pose relative to REST — the rest basis is a large
	// rotation) [orig: translation overwrite @ 0x4b22cf..0x4b22f8].
	const Transform3D joint_world = skel->get_global_transform() *
			skel->get_bone_global_pose(kHeldWeaponBoneIndex);
	const Transform3D model_to_world = joint_world *
			skel->get_bone_global_rest(kHeldWeaponBoneIndex).affine_inverse();
	const Basis basis = hand_frame
			? held_weapon_hand_frame_basis(model_to_world.basis)
			: bms_to_godot_basis(attach_angles_bms);
	return Transform3D(basis,
			joint_world.origin + model_to_world.basis.xform(kHeldWeaponAttachNudge));
}

Vector3 PresentApplier::held_weapon_attach_nudge() {
	return kHeldWeaponAttachNudge;
}

double PresentApplier::held_weapon_hand_frame_z_rad() {
	return opennova::simassets::kHeldWeaponHandFrameZRad;
}

double PresentApplier::held_weapon_hand_frame_y_rad() {
	return opennova::simassets::kHeldWeaponHandFrameYRad;
}

Basis PresentApplier::aim_root_basis(const PackedFloat32Array &snap, int base,
		const Basis &fallback) {
	const float *p = snap.ptr();
	if (field_i(p, base, Simulation::PF_AIM_OVERLAY_VALID) == 0) {
		return fallback;
	}
	return bms_to_godot_basis(
			Vector3(p[base + Simulation::PF_AIM_BODY_PITCH_DEG],
					p[base + Simulation::PF_AIM_BODY_YAW_DEG],
					p[base + Simulation::PF_AIM_BODY_ROLL_DEG]));
}

void PresentApplier::aim_apply(Object *node, const PackedFloat32Array &snap,
		int base, bool drive_root_basis) {
	ObjectModel *model = Object::cast_to<ObjectModel>(node);
	if (model == nullptr) {
		return;
	}
	const float *p = snap.ptr();
	// The mounted-seat selector's final skeletal verdict applies to placed and
	// wire models alike [orig: BN17 special row @ 0x4b1290].
	model->set_right_hand_collapsed(
			field_i(p, base, Simulation::PF_RIGHT_HAND_COLLAPSED) != 0);
	if (field_i(p, base, Simulation::PF_AIM_OVERLAY_VALID) == 0) {
		model->clear_aim_overlay();
		return;
	}
	aim_apply_valid(model, snap, base, drive_root_basis);
}

void PresentApplier::aim_apply_valid(Object *node,
		const PackedFloat32Array &snap, int base, bool drive_root_basis) {
	ObjectModel *model = Object::cast_to<ObjectModel>(node);
	if (model == nullptr) {
		return;
	}
	const Basis current_basis = model->get_basis();
	const Basis body_basis = aim_root_basis(snap, base, current_basis);
	if (drive_root_basis && current_basis != body_basis) {
		model->set_basis(body_basis);
	}
	const Basis inverse_body = body_basis.inverse();
	const float *p = snap.ptr();
	Basis deltas[ObjectModel::kAimOverlayClasses];
	for (int overlay_class = 0; overlay_class < ObjectModel::kAimOverlayClasses;
			++overlay_class) {
		const int offset = base + Simulation::PF_AIM_ANGLES +
				overlay_class * Simulation::PF_AIM_CLASS_STRIDE;
		deltas[overlay_class] = inverse_body *
				bms_to_godot_basis(
						Vector3(p[offset], p[offset + 1], p[offset + 2]));
	}
	model->set_aim_overlay_deltas(deltas);
}

namespace {

void emplaced_clear_typed(ObjectModel *model);
void vehicle_motion_clear_typed(ObjectModel *model);
void zone_team_clear_typed(ObjectModel *model);
void world_heat_clear_typed(ObjectModel *model);

int emplaced_apply_typed(ObjectModel *model, const PackedFloat32Array &snap,
		int base, bool clear_when_invalid) {
	const CtrlNames &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, Simulation::PF_EMPLACED_CONTROLS_VALID) == 1) {
		set_owned_ctrl(model, n.owner_emplaced, n.eweap_gunyaw,
				field_i(p, base, Simulation::PF_EWEAP_GUNYAW));
		set_owned_ctrl(model, n.owner_emplaced, n.eweap_gunpitch,
				field_i(p, base, Simulation::PF_EWEAP_GUNPITCH));
		return 2;
	}
	// Nodes persist across dismount/death: remove only the two controls this
	// presenter owns — a bulk CTRL clear would also erase live WAC channels.
	if (clear_when_invalid) {
		emplaced_clear_typed(model);
	}
	return 0;
}

void emplaced_clear_typed(ObjectModel *model) {
	const CtrlNames &n = names();
	clear_owned_ctrl(model, n.owner_emplaced, n.eweap_gunyaw);
	clear_owned_ctrl(model, n.owner_emplaced, n.eweap_gunpitch);
}

int vehicle_motion_apply_typed(ObjectModel *model,
		const PackedFloat32Array &snap, int base) {
	const CtrlNames &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, Simulation::PF_VEHICLE_MOTION_VALID) == 1) {
		// Both fields are owned even at rest (literal zero), exactly as the
		// cveh callback stores them immediately before model submission.
		// [orig: Entity_CacheVehicleHUDStats @0x4929B0;
		//  stores @0x4929D7 / @0x4929F1]
		set_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_steering,
				field_i(p, base, Simulation::PF_VEHICLE_STEERING));
		set_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_speed,
				field_i(p, base, Simulation::PF_VEHICLE_SPEED));
		// The part-animation words are stored by the same callback, again as
		// literal zero at rest [orig: Entity_CacheVehicleHUDStats @0x4929B0 —
		// the rotor word @0x492ACA..0x492ADE for both rotor ordinals, the wheel
		// word @0x4929B4; see docs/world/vehicle-client-movers-re.md §14].
		set_owned_ctrl(model, n.owner_vehicle_motion, n.helo_rotor,
				field_i(p, base, Simulation::PF_VEHICLE_ROTOR));
		set_owned_ctrl(model, n.owner_vehicle_motion, n.helo_tailrotor,
				field_i(p, base, Simulation::PF_VEHICLE_TAIL_ROTOR));
		set_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_wheels,
				field_i(p, base, Simulation::PF_VEHICLE_WHEELS));
		return 5;
	}
	vehicle_motion_clear_typed(model);
	return 0;
}

void vehicle_motion_clear_typed(ObjectModel *model) {
	const CtrlNames &n = names();
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_steering);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_speed);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.helo_rotor);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.helo_tailrotor);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_wheels);
}

int zone_team_apply_typed(ObjectModel *model, const PackedFloat32Array &snap,
		int base) {
	const CtrlNames &n = names();
	const float *p = snap.ptr();
	int writes = 0;
	if (field_i(p, base, Simulation::PF_TEX_TEAM_VALID) == 1) {
		set_owned_ctrl(model, n.owner_sector_team, n.tex_team,
				field_i(p, base, Simulation::PF_TEX_TEAM));
		++writes;
	} else {
		clear_owned_ctrl(model, n.owner_sector_team, n.tex_team);
	}
	if (field_i(p, base, Simulation::PF_ZONE_CTRL_VALID) == 1) {
		// TEAMSWING is an unconditional store inside the packed-zone-byte
		// branch, including literal zero for team 1.
		set_owned_ctrl(model, n.owner_zone, n.team_swing,
				field_i(p, base, Simulation::PF_TEAMSWING));
		++writes;
	} else {
		clear_owned_ctrl(model, n.owner_zone, n.team_swing);
	}
	if (field_i(p, base, Simulation::PF_LFP_CAMPPERCENT_VALID) == 1) {
		set_owned_ctrl(model, n.owner_zone, n.lfp_camp_percent,
				field_i(p, base, Simulation::PF_LFP_CAMPPERCENT));
		++writes;
	} else {
		// A numbered zone without a timer-list entry does not write LFP at all.
		// Releasing our bounded per-model writer represents that omission; it is
		// deliberately not a fabricated zero store.
		clear_owned_ctrl(model, n.owner_zone, n.lfp_camp_percent);
	}
	return writes;
}

void zone_team_clear_typed(ObjectModel *model) {
	const CtrlNames &n = names();
	clear_owned_ctrl(model, n.owner_sector_team, n.tex_team);
	clear_owned_ctrl(model, n.owner_zone, n.team_swing);
	clear_owned_ctrl(model, n.owner_zone, n.lfp_camp_percent);
}

int world_heat_apply_typed(ObjectModel *model, const PackedFloat32Array &snap,
		int base) {
	const CtrlNames &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, Simulation::PF_WORLD_HEAT_GLOW_VALID) == 1) {
		// The valid carrier-attachment scope owns cold zero too.
		// [orig: HUD_CacheWeaponSlotInfo @ 0x440969 / @ 0x440991,
		//  sole caller @ 0x546518]
		set_owned_ctrl(model, n.owner_world_heat, n.heat_glow,
				field_i(p, base, Simulation::PF_WORLD_HEAT_GLOW));
		return 1;
	}
	world_heat_clear_typed(model);
	return 0;
}

void world_heat_clear_typed(ObjectModel *model) {
	const CtrlNames &n = names();
	clear_owned_ctrl(model, n.owner_world_heat, n.heat_glow);
}

} // namespace

int PresentApplier::wire_controls_apply(ObjectModel *model,
		const PackedFloat32Array &snap, int base) {
	int writes = 0;
	writes += emplaced_apply_typed(model, snap, base, true);
	writes += vehicle_motion_apply_typed(model, snap, base);
	writes += zone_team_apply_typed(model, snap, base);
	writes += world_heat_apply_typed(model, snap, base);
	return writes;
}

int PresentApplier::emplaced_apply(Object *node,
		const PackedFloat32Array &snap, int base, bool clear_when_invalid) {
	ObjectModel *model = Object::cast_to<ObjectModel>(node);
	return model != nullptr
			? emplaced_apply_typed(model, snap, base, clear_when_invalid)
			: 0;
}

int64_t PresentApplier::current_index_generation() {
	return index_.is_valid() ? index_->get_generation() : 0;
}

bool PresentApplier::row_plan_is_current(int64_t size, int stride,
		int64_t layout_revision) {
	if (plan_dirty_ || plan_revision_ != layout_revision ||
			plan_stride_ != stride || plan_snapshot_size_ != size ||
			plan_index_generation_ != current_index_generation() ||
			plan_model_lifetime_generation_ !=
					ObjectModel::lifetime_generation()) {
		return false;
	}
	return true;
}

void PresentApplier::rebuild_row_plan(const float *p, int64_t size, int stride,
		int64_t layout_revision) {
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0) {
		release_part_anim_outputs();
	}
	++stat_plan_rebuilds_;
	release_planned_rows();
	present_visibility_.clear();
	plan_revision_ = layout_revision;
	plan_stride_ = stride;
	plan_snapshot_size_ = size;
	plan_index_generation_ = current_index_generation();
	plan_model_lifetime_generation_ = ObjectModel::lifetime_generation();
	plan_dirty_ = false;
	if (index_.is_null()) {
		plan_dirty_ = true;
		return;
	}
	const int64_t count = size / stride;
	rows_.reserve(static_cast<size_t>(count));
	for (int64_t r = 0; r < count; ++r) {
		const int base = static_cast<int>(r * stride);
		const int32_t bms_id = field_i(p, base, Simulation::PF_BMS_ID);
		const int32_t kind = field_i(p, base, Simulation::PF_KIND);
		const int32_t idx = field_i(p, base, Simulation::PF_INDEX);
		ObjectModel *model = index_->resolve(bms_id, kind, idx);
		if (model == nullptr) {
			continue;
		}
		Row row;
		row.base = base;
		row.node_id = model->get_instance_id();
		row.model = model;
		model->set_present_planned(true);
		row.entity_kind = kind;
		row.entity_index = idx;
		row.bms_id = bms_id;
		rows_.push_back(row);
	}
}

void PresentApplier::release_part_anim_outputs() {
	for (const Row &row : rows_) {
		ObjectModel *model =
				Object::cast_to<ObjectModel>(ObjectDB::get_instance(row.node_id));
		if (model == nullptr) continue;
		model->begin_ctrl_update();
		model->clear_part_phase(1);
		model->clear_part_phase(2);
		emplaced_clear_typed(model);
		vehicle_motion_clear_typed(model);
		zone_team_clear_typed(model);
		world_heat_clear_typed(model);
		model->end_ctrl_update();
	}
}

const String &PresentApplier::infantry_key(int state) {
	auto it = infantry_keys_.find(state);
	if (it == infantry_keys_.end()) {
		it = infantry_keys_.emplace(state, Simulation::infantry_anim_key(state))
					 .first;
	}
	return it->second;
}

void PresentApplier::present_snapshot(const PackedFloat32Array &snap,
		int stride, int64_t layout_revision) {
	present_snapshot_impl(snap, stride, layout_revision, nullptr);
}

PackedInt64Array PresentApplier::profile_present_snapshot(
		const PackedFloat32Array &snap, int stride, int64_t layout_revision) {
	MissionFrameProfile profile;
	present_snapshot_impl(snap, stride, layout_revision, &profile);
	PackedInt64Array result;
	result.resize(MISSION_PROFILE_SLOT_COUNT);
	result.set(MISSION_PROFILE_CORE_US, profile.core_us);
	result.set(MISSION_PROFILE_AIM_US, profile.aim_us);
	result.set(MISSION_PROFILE_CONTROLS_US, profile.controls_us);
	result.set(MISSION_PROFILE_VISIBILITY_US, profile.visibility_us);
	result.set(MISSION_PROFILE_BODY_US, profile.body_us);
	result.set(MISSION_PROFILE_ROWS, profile.rows);
	result.set(MISSION_PROFILE_SUBMITTED_ROWS, profile.submitted_rows);
	result.set(MISSION_PROFILE_BODY_ROWS, profile.body_rows);
	return result;
}

void PresentApplier::present_snapshot_impl(const PackedFloat32Array &snap,
		int stride, int64_t layout_revision, MissionFrameProfile *p_profile) {
	if (stride < Simulation::PF_STRIDE || index_.is_null()) {
		return;
	}
	const float *p = snap.ptr();
	const int64_t size = snap.size();
	if (!row_plan_is_current(size, stride, layout_revision)) {
		rebuild_row_plan(p, size, stride, layout_revision);
	}
	Simulation *native_sim =
			Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
	for (Row &row : rows_) {
		// Main-thread Node destruction advances this stamp in PREDELETE. Once
		// a planned model was freed by a notification dispatched during this
		// walk, no retained pointer is trusted for the rest of it: every later
		// row resolves cold through ObjectDB (a freed row releases its
		// visibility intent) and the plan rebinds on the next call, so one
		// free never drops a frame of presentation for the surviving rows.
		if (plan_model_lifetime_generation_ !=
				ObjectModel::lifetime_generation()) {
			plan_dirty_ = true;
		}
		ObjectModel *model = row.model;
		if (plan_dirty_) {
			model = Object::cast_to<ObjectModel>(
					ObjectDB::get_instance(row.node_id));
			if (model == nullptr) {
				present_visibility_.erase(row.bms_id);
				row.present_visible = -1;
				continue;
			}
		}
		const int base = row.base;
		uint64_t profile_phase_start = p_profile != nullptr
				? Time::get_singleton()->get_ticks_usec()
				: 0;
		if (p_profile != nullptr) {
			++p_profile->rows;
		}
		model->set_match_terrain_enabled(
				(field_i(p, base, Simulation::PF_STANCE_BITS) & 0x03) != 0);
		if ((output_channels_ & OUTPUT_TRANSFORM) != 0) {
			// Compare the six packed source floats before constructing either
			// the placement Basis or Transform3D.
			// Position is already Godot-space (x, z, -y); rotation is
			// mission-space degrees, built through the ONE placement convention
			// so a sim-driven entity sits exactly where placement would put it.
			const bool aim_owns_root =
					field_i(p, base,
							Simulation::PF_AIM_OVERLAY_VALID) != 0;
			const int rotation_field = aim_owns_root
					? Simulation::PF_AIM_BODY_PITCH_DEG
					: Simulation::PF_PITCH_DEG;
			const std::array<float, 6> next_stamp = {
				p[base + Simulation::PF_POS_X],
				p[base + Simulation::PF_POS_Y],
				p[base + Simulation::PF_POS_Z],
				p[base + rotation_field],
				p[base + rotation_field + 1],
				p[base + rotation_field + 2],
			};
			// The pass owns these transforms: compare against the last APPLIED
			// value instead of reading the node property back per row (the aim
			// leg below runs with drive_root_basis=false, so nothing else
			// rewrites the root between frames). On a cache miss (fresh plan —
			// including the every-call rebuild of revisionless fake sources)
			// fall back to one live read so an unchanged row never re-dirties
			// the node's tree, exactly like the GDScript live compare did.
			if (!row.transform_stamp_valid ||
					row.transform_stamp != next_stamp) {
				const Transform3D next = model->compose_entity_transform(
						bms_to_godot_basis(
								Vector3(next_stamp[3], next_stamp[4],
										next_stamp[5])),
						Vector3(next_stamp[0], next_stamp[1], next_stamp[2]));
				++stat_transform_builds_;
				if (row.transform_stamp_valid ||
						model->get_transform() != next) {
					model->set_transform(next);
					++stat_moved_;
				}
				if (placer_.is_valid()) {
					// The render node and terrain projection registry consume the
					// same present pose. The placer owns admission and exact-value
					// gating, so rejected rows and repeated snapshots remain free.
					placer_->update_static_terrain_shadow_source_transform(
							static_cast<MissionData::EntityKind>(row.entity_kind),
							row.entity_index, next);
				}
				row.transform_stamp = next_stamp;
				row.transform_stamp_valid = true;
			}
		}
		const bool present_visible =
				field_i(p, base, Simulation::PF_HIDDEN) == 0 &&
				field_i(p, base,
						Simulation::PF_LOCAL_VIEW_SUPPRESSED) == 0;
		// Camera submission: retail runs the presentation writers per
		// SUBMITTED model [orig: Terrain_RenderSectorModels @ 0x5c5d30
		// computes per drawn model; cull/submit @ Entity_RenderVehicleModel
		// @ 0x4407d0]. A row the renderer would not submit — sim-hidden,
		// occlusion-held, or bounds off-screen — skips the aim/part/CTRL
		// dispatch legs below. The applied stamps keep their last-dispatched
		// values, so the next submitted frame re-applies exactly what changed
		// while the row was out (set legs are per-submission re-asserts;
		// falling edges latched in the publish state still clear).
		const bool submitted = present_visible &&
				!occlusion_hidden_ids_.has(row.bms_id) &&
				model->is_on_screen();
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->core_us += now - profile_phase_start;
			profile_phase_start = now;
			if (submitted) {
				++p_profile->submitted_rows;
			}
		}
		// Aim overlay with the capability lookups hoisted into the row plan and
		// the no-overlay clear gated to the valid->invalid edge (the node-side
		// setters no-op on repeats; these gates skip the dispatch itself).
		bool body_dependency_changed = false;
		if (submitted) {
			const int32_t rhc =
					field_i(p, base, Simulation::PF_RIGHT_HAND_COLLAPSED);
			if (rhc != row.rhc) {
				model->set_right_hand_collapsed(rhc != 0);
				++stat_rhc_dispatches_;
				row.rhc = rhc;
				body_dependency_changed = true;
			}
		}
		if (submitted) {
			const int32_t aim_valid =
					field_i(p, base, Simulation::PF_AIM_OVERLAY_VALID);
			if (aim_valid != 0) {
				bool payload_changed =
						row.aim_valid != 1 || !row.aim_payload_valid;
				if (!payload_changed) {
					for (int i = 0; i < AIM_PAYLOAD_FLOATS; ++i) {
						if (row.aim_payload[static_cast<size_t>(i)] !=
								p[base +
										Simulation::PF_AIM_BODY_PITCH_DEG +
										i]) {
							payload_changed = true;
							break;
						}
					}
				}
				if (payload_changed) {
					aim_apply_valid(model, snap, base, false);
					++stat_aim_dispatches_;
					std::copy_n(
							p + base +
									Simulation::PF_AIM_BODY_PITCH_DEG,
							AIM_PAYLOAD_FLOATS, row.aim_payload.begin());
					row.aim_payload_valid = true;
					body_dependency_changed = true;
				}
				row.aim_valid = 1;
			} else if (row.aim_valid != 0) {
				model->clear_aim_overlay();
				++stat_aim_dispatches_;
				row.aim_valid = 0;
				row.aim_payload_valid = false;
				body_dependency_changed = true;
			} else {
				row.aim_valid = 0;
			}
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->aim_us += now - profile_phase_start;
			profile_phase_start = now;
		}
		if (submitted && (output_channels_ & OUTPUT_PART_ANIM) != 0) {
			const int32_t active1_code =
					field_i(p, base, Simulation::PF_ACTIVE1);
			const int32_t active2_code =
					field_i(p, base, Simulation::PF_ACTIVE2);
			// The phase-domain predicate lives with the integrator
			// (world/ai.h part_anim_phase_active).
			const int32_t active1 =
					opennova::world::part_anim_phase_active(active1_code) ? 1 : 0;
			const int32_t active2 =
					opennova::world::part_anim_phase_active(active2_code) ? 1 : 0;
			const int32_t phase1 = active1 != 0
					? Simulation::decode_present_part_anim_phase(
							snap, base, 1)
					: 0;
			const int32_t phase2 = active2 != 0
					? Simulation::decode_present_part_anim_phase(
							snap, base, 2)
					: 0;
			const int32_t controls_valid =
					field_i(p, base,
							Simulation::PF_EMPLACED_CONTROLS_VALID) == 1
					? 1
					: 0;
			const int32_t vehicle_valid =
					field_i(p, base,
							Simulation::PF_VEHICLE_MOTION_VALID) == 1
					? 1
					: 0;
			const int32_t tex_team_valid =
					field_i(p, base, Simulation::PF_TEX_TEAM_VALID) == 1
					? 1
					: 0;
			const int32_t zone_valid =
					field_i(p, base, Simulation::PF_ZONE_CTRL_VALID) == 1
					? 1
					: 0;
			const int32_t lfp_valid =
					field_i(p, base,
							Simulation::PF_LFP_CAMPPERCENT_VALID) == 1
					? 1
					: 0;
			const int32_t heat_valid =
					field_i(p, base,
							Simulation::PF_WORLD_HEAT_GLOW_VALID) == 1
					? 1
					: 0;
			const std::array<int32_t, CTRL_PUBLISH_COUNT>
					next_ctrl_publish_state = {
				active1,
				active2,
				controls_valid,
				vehicle_valid,
				tex_team_valid,
				zone_valid,
				lfp_valid,
				heat_valid,
			};
			const bool cold = !row.ctrl_publish_state_valid;
			const auto was_published = [&](CtrlPublishField field) {
				return row.ctrl_publish_state[
							   static_cast<size_t>(field)] != 0;
			};
			const bool set_part1 = active1 != 0;
			const bool set_part2 = active2 != 0;
			const bool clear_part1 = active1 == 0 &&
					(cold || was_published(CTRL_PUBLISH_PART1));
			const bool clear_part2 = active2 == 0 &&
					(cold || was_published(CTRL_PUBLISH_PART2));
			// A valid writer executes for every retail model submission. This
			// reasserts ownership after any intervening producer, while the
			// model-side setter makes an unchanged owner/value a true no-op.
			// Omitted writers clear only on a cold/falling edge.
			const bool emplaced_work = controls_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_EMPLACED);
			const bool vehicle_work = vehicle_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_VEHICLE);
			const bool zone_work = tex_team_valid != 0 || zone_valid != 0 ||
					lfp_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_TEX_TEAM) ||
					was_published(CTRL_PUBLISH_ZONE) ||
					was_published(CTRL_PUBLISH_LFP);
			const bool heat_work = heat_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_HEAT);
			const bool any_work = set_part1 || set_part2 ||
					clear_part1 || clear_part2 || emplaced_work ||
					vehicle_work || zone_work || heat_work;
			if (any_work) {
				model->begin_ctrl_update();
			}
			// A falling/cold EWEAP release precedes generic phase replay. This
			// preserves the master compatibility surface for a third-party node
			// that aliases a part channel onto EWEAP, without the old
			// unconditional clear/re-add on every valid frame. Production
			// ObjectModel uses fixed VEHICLE_SPECIAL1/2 and cannot alias it.
			if (emplaced_work && controls_valid == 0) {
				emplaced_clear_typed(model);
				stat_control_dispatches_ += 2;
			}
			// PANM phases are integrated by the engine [orig:
			// Entity_ApplyCommand @ 0x43ab60 case 0x22]. ACTIVE is the
			// publication bit: inactive releases this presenter's slot.
			if (set_part1) {
				model->set_part_phase(1, phase1);
				++stat_posed_;
				++stat_part_dispatches_;
			} else if (clear_part1) {
				model->clear_part_phase(1);
				++stat_part_dispatches_;
			}
			if (set_part2) {
				model->set_part_phase(2, phase2);
				++stat_posed_;
				++stat_part_dispatches_;
			} else if (clear_part2) {
				model->clear_part_phase(2);
				++stat_part_dispatches_;
			}
			if (emplaced_work && controls_valid != 0) {
				const int applied = emplaced_apply_typed(model, snap, base, false);
				stat_posed_ += applied;
				stat_control_dispatches_ += applied;
			}
			if (vehicle_work) {
				const int applied = vehicle_motion_apply_typed(model, snap, base);
				stat_posed_ += applied;
				stat_control_dispatches_ += 2;
			}
			if (zone_work) {
				const int applied = zone_team_apply_typed(model, snap, base);
				stat_posed_ += applied;
				stat_control_dispatches_ += 3;
			}
			if (heat_work) {
				const int applied = world_heat_apply_typed(model, snap, base);
				stat_posed_ += applied;
				++stat_control_dispatches_;
			}
			if (any_work) {
				model->end_ctrl_update();
			}
			row.ctrl_publish_state = next_ctrl_publish_state;
			row.ctrl_publish_state_valid = true;
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->controls_us += now - profile_phase_start;
			profile_phase_start = now;
		}
		const int32_t present_visible_int = present_visible ? 1 : 0;
		if (present_visible_int != row.present_visible) {
			present_visibility_[row.bms_id] = present_visible;
			row.present_visible = present_visible_int;
		}
		if ((output_channels_ & OUTPUT_VISIBILITY) != 0) {
			// The row owns the model's section-mask channel only while it
			// publishes PF_SECTION_MASK_VALID. Rows that never publish must
			// not touch the channel at all — the occlusion frame pass drives
			// the same ObjectModel call for buildings, and an unconditional
			// release here would stomp its applied mask after a plan rebuild.
			if (field_i(p, base, Simulation::PF_SECTION_MASK_VALID) != 0) {
				const uint32_t hidden_mask =
						static_cast<uint32_t>(field_i(
								p, base, Simulation::PF_SECTION_MASK_LO)) |
						(static_cast<uint32_t>(field_i(
								p, base, Simulation::PF_SECTION_MASK_HI))
								<< 16);
				const int64_t section_visibility_mask = static_cast<int64_t>(
						hidden_mask ^ 0xffffffffu);
				if (section_visibility_mask != row.section_visibility_mask) {
					model->set_section_visibility_mask(section_visibility_mask);
					row.section_visibility_mask = section_visibility_mask;
				}
			} else if (row.section_visibility_mask != -2 &&
					row.section_visibility_mask != -1) {
				// One release when a previously owned row stops publishing.
				model->set_section_visibility_mask(-1);
				row.section_visibility_mask = -1;
			}
			// Death is not disappearance (corpses and husks keep rendering until
			// the sim despawns via PF_HIDDEN); the local first-person UseGun
			// parent's own world model is presentation-suppressed. Semantics and
			// witnesses recorded at the GDScript origin (mission_present_pass.gd)
			// [orig: Entity_RenderVehicleModel @ 0x4407d0 cull/submit;
			// Flags&4 husk pick @ 0x413086].
			if (model->is_visible() != present_visible) {
				// Two-bit visibility ownership: while the render-occlusion frame
				// claims this node (the shared hidden set), a sim-wants-visible
				// node stays hidden — occlusion releases through the same set.
				if (!(present_visible &&
							occlusion_hidden_ids_.has(row.bms_id))) {
					model->set_visible(present_visible);
				}
			}
			if (!present_visible) {
				++stat_hidden_;
			}
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->visibility_us += now - profile_phase_start;
			profile_phase_start = now;
		}
		// Unsubmitted models (hidden, occlusion-held, off-screen) skip skeletal
		// writes: the simulation resolves AI fire origins from its own pose
		// (world/pose_provider.h), never from a presented skeleton.
		if ((output_channels_ & OUTPUT_BODY_ANIM) != 0 && submitted) {
			if (p_profile != nullptr) {
				++p_profile->body_rows;
			}
			// Main-body skeletal clip: infantry poses to the exact anim-state
			// phase that produced root motion; PF_BODY_ANIM_SLOT is the coarse
			// fallback for compatible non-infantry nodes.
			const int32_t anim_state =
					field_i(p, base, Simulation::PF_ANIM_STATE);
			int32_t body_mode = BODY_NONE;
			int32_t body_selector = -1;
			int32_t body_phase = 0;
			int32_t body_source_selector = -1;
			int32_t body_source_phase = 0;
			float body_blend_weight = 1.0f;
			String body_clip_key;
			String body_source_clip_key;
			{
				const String key =
						anim_state >= 0 ? infantry_key(anim_state) : String();
				const int32_t source_state = field_i(
						p, base, Simulation::PF_ANIM_SOURCE_STATE);
				const String source_key =
						source_state >= 0 ? infantry_key(source_state) : String();
				if (!key.is_empty()) {
					body_selector = anim_state;
					body_phase = field_i(
							p, base, Simulation::PF_ANIM_PHASE_TICKS);
					body_clip_key = key;
					const float target_weight =
							p[base + Simulation::PF_ANIM_BLEND_WEIGHT];
					if (source_state >= 0 && !source_key.is_empty() &&
							target_weight < 1.0f) {
						body_mode = BODY_BLEND_AT;
						body_source_selector = source_state;
						body_source_phase = field_i(
								p, base,
								Simulation::PF_ANIM_SOURCE_PHASE_TICKS);
						body_source_clip_key = source_key;
						body_blend_weight = target_weight;
					} else {
						body_mode = BODY_CLIP_AT;
					}
				} else if (source_state >= 0 && !source_key.is_empty()) {
					body_mode = BODY_CLIP_AT;
					body_selector = source_state;
					body_phase = field_i(
							p, base,
							Simulation::PF_ANIM_SOURCE_PHASE_TICKS);
					body_clip_key = source_key;
				}
			}
			if (body_mode == BODY_NONE) {
				const int32_t body_anim_slot =
						field_i(p, base, Simulation::PF_BODY_ANIM_SLOT);
				if (body_anim_slot >= 0) {
					body_mode = BODY_SLOT_AT;
					body_selector = body_anim_slot;
					body_phase = field_i(
							p, base, Simulation::PF_ANIM_PHASE_TICKS);
				}
			}
			const bool body_stamp_changed =
					!row.body_stamp_valid || row.body_mode != body_mode ||
					row.body_selector != body_selector ||
					row.body_phase != body_phase ||
					row.body_source_selector != body_source_selector ||
					row.body_source_phase != body_source_phase ||
					row.body_blend_weight != body_blend_weight;
			const bool force_external_pose =
					body_dependency_changed &&
					(body_mode == BODY_CLIP_AT ||
							body_mode == BODY_BLEND_AT ||
							body_mode == BODY_SLOT_AT);
			if (body_stamp_changed || force_external_pose) {
				switch (body_mode) {
					case BODY_CLIP_AT:
						model->play_body_clip_at(body_clip_key, body_phase);
						++stat_body_dispatches_;
						break;
					case BODY_BLEND_AT:
						model->play_body_blend_at(body_source_clip_key,
								body_source_phase, body_clip_key, body_phase,
								body_blend_weight);
						++stat_body_dispatches_;
						break;
					case BODY_SLOT_AT:
						model->play_body_anim_at(body_selector, body_phase);
						++stat_body_dispatches_;
						break;
					case BODY_SLOT_PLAY:
					case BODY_NONE:
						break;
				}
				row.body_mode = body_mode;
				row.body_selector = body_selector;
				row.body_phase = body_phase;
				row.body_source_selector = body_source_selector;
				row.body_source_phase = body_source_phase;
				row.body_blend_weight = body_blend_weight;
				row.body_stamp_valid = true;
			}
		} else if ((output_channels_ & OUTPUT_BODY_ANIM) != 0) {
			// Desired state can keep changing while a hidden, non-muzzle row is
			// ineligible. Keep the applied stamp dirty so visibility catches up.
			row.body_stamp_valid = false;
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->body_us += now - profile_phase_start;
			profile_phase_start = now;
		}
	}
}
