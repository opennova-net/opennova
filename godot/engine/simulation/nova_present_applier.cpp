#include "simulation/nova_present_applier.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <algorithm>

#include "simulation/nova_simulation.h"

using namespace godot;

namespace {

// Dispatch-surface StringNames built once (function-local statics: safe after
// Godot init, shared across appliers).
struct DispatchNames {
	StringName resolve = StringName("resolve");
	StringName get_generation = StringName("get_generation");
	StringName set_part_phase = StringName("set_part_phase");
	StringName clear_part_phase = StringName("clear_part_phase");
	StringName play_body_clip_at = StringName("play_body_clip_at");
	StringName play_body_blend_at = StringName("play_body_blend_at");
	StringName play_body_anim_at = StringName("play_body_anim_at");
	StringName play_body_anim = StringName("play_body_anim");
	StringName set_aim_overlay = StringName("set_aim_overlay");
	StringName set_right_hand_collapsed = StringName("set_right_hand_collapsed");
	StringName set_ctrl_value = StringName("set_ctrl_value");
	StringName clear_ctrl_value = StringName("clear_ctrl_value");
	StringName set_ctrl_override = StringName("set_ctrl_override");
	StringName clear_ctrl_override = StringName("clear_ctrl_override");
	StringName begin_ctrl_update = StringName("begin_ctrl_update");
	StringName end_ctrl_update = StringName("end_ctrl_update");
	StringName has_muzzle = StringName("has_muzzle");
	StringName get_muzzle_world_position = StringName("get_muzzle_world_position");
	StringName set_ai_muzzle_world = StringName("set_ai_muzzle_world");
	StringName basis = StringName("basis");
	String eweap_gunyaw = String("EWEAP_GUNYAW");
	String eweap_gunpitch = String("EWEAP_GUNPITCH");
	String vehicle_steering = String("VEHICLE_STEERING");
	String vehicle_speed = String("VEHICLE_SPEED");
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

const DispatchNames &names() {
	static DispatchNames n;
	return n;
}

// Per-row node capabilities, resolved once at plan rebuild (has_method is a
// per-frame string lookup otherwise).
enum RowCaps {
	CAP_BODY_CLIP = 1,
	CAP_BODY_SLOT = 2,
	CAP_BODY_PLAY = 4,
	CAP_AIM = 8,
	CAP_RHC = 16,
	CAP_CTRL = 32,
	CAP_MUZZLE = 64,
	CAP_PART = 128,
	CAP_BODY_BLEND = 256,
	CAP_PART_CLEAR = 512,
	CAP_CTRL_BATCH = 1024,
};
static_assert((CAP_PART | CAP_BODY_BLEND | CAP_PART_CLEAR | CAP_CTRL_BATCH) ==
				(128 | 256 | 512 | 1024),
		"presentation capabilities need distinct bits");

inline int32_t field_i(const float *p, int base, int field) {
	return static_cast<int32_t>(p[base + field]);
}

int ctrl_dispatch_capabilities(Object *node) {
	if (node == nullptr) return 0;
	const DispatchNames &n = names();
	int caps = 0;
	if (node->has_method(n.set_ctrl_override) &&
			node->has_method(n.clear_ctrl_override)) {
		caps |= NovaPresentApplier::VISUAL_CTRL_OWNED;
	} else if (node->has_method(n.set_ctrl_value) &&
			node->has_method(n.clear_ctrl_value)) {
		caps |= NovaPresentApplier::VISUAL_CTRL_LEGACY;
	}
	return caps;
}

int visual_control_capabilities(Object *node) {
	if (node == nullptr) return 0;
	const DispatchNames &n = names();
	int caps = ctrl_dispatch_capabilities(node);
	if (node->has_method(n.begin_ctrl_update) &&
			node->has_method(n.end_ctrl_update)) {
		caps |= NovaPresentApplier::VISUAL_CTRL_BATCH;
	}
	if (node->has_method(n.set_part_phase)) {
		caps |= NovaPresentApplier::VISUAL_PART_PHASE;
	}
	if (node->has_method(n.clear_part_phase)) {
		caps |= NovaPresentApplier::VISUAL_PART_CLEAR;
	}
	return caps;
}

bool has_ctrl_surface(int capabilities) {
	return (capabilities & (NovaPresentApplier::VISUAL_CTRL_OWNED |
								  NovaPresentApplier::VISUAL_CTRL_LEGACY)) != 0;
}

void set_owned_ctrl(Object *node, int capabilities, const String &owner,
		const String &reg, int32_t value) {
	if (node == nullptr) return;
	const DispatchNames &n = names();
	if ((capabilities & NovaPresentApplier::VISUAL_CTRL_OWNED) != 0) {
		node->call(n.set_ctrl_override, owner, reg, value);
	} else if ((capabilities &
					   NovaPresentApplier::VISUAL_CTRL_LEGACY) != 0) {
		// Compatibility surface for test doubles and third-party visual nodes.
		node->call(n.set_ctrl_value, reg, value);
	}
}

void clear_owned_ctrl(Object *node, int capabilities, const String &owner,
		const String &reg) {
	if (node == nullptr) return;
	const DispatchNames &n = names();
	if ((capabilities & NovaPresentApplier::VISUAL_CTRL_OWNED) != 0) {
		node->call(n.clear_ctrl_override, owner, reg);
	} else if ((capabilities &
					   NovaPresentApplier::VISUAL_CTRL_LEGACY) != 0) {
		node->call(n.clear_ctrl_value, reg);
	}
}

constexpr int AIM_PAYLOAD_FLOATS =
		NovaSimulation::PF_EMPLACED_CONTROLS_VALID -
		NovaSimulation::PF_AIM_BODY_PITCH_DEG;
static_assert(AIM_PAYLOAD_FLOATS == 30,
		"aim cache must cover body Euler plus all nine overlay triples");

} // namespace

void NovaPresentApplier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "sim", "index"),
			&NovaPresentApplier::setup);
	ClassDB::bind_method(D_METHOD("set_output_channels", "channels"),
			&NovaPresentApplier::set_output_channels);
	ClassDB::bind_method(D_METHOD("get_output_channels"),
			&NovaPresentApplier::get_output_channels);
	ClassDB::bind_method(
			D_METHOD("set_shared_visibility_maps", "occlusion_hidden_ids",
					"present_visibility"),
			&NovaPresentApplier::set_shared_visibility_maps);
	ClassDB::bind_method(
			D_METHOD("present_snapshot", "snap", "stride", "layout_revision"),
			&NovaPresentApplier::present_snapshot);
	ClassDB::bind_method(D_METHOD("get_stats"), &NovaPresentApplier::get_stats);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("bms_to_godot_basis", "rot_deg"),
			&NovaPresentApplier::bms_to_godot_basis);
	ClassDB::bind_method(D_METHOD("setup_wire", "rebuild_held_weapon"),
			&NovaPresentApplier::setup_wire);
	ClassDB::bind_method(
			D_METHOD("begin_wire_plan", "layout_revision", "stride",
					"snapshot_size", "index_generation", "local_handle"),
			&NovaPresentApplier::begin_wire_plan);
	ClassDB::bind_method(
			D_METHOD("append_wire_row", "node", "base", "handle", "spawned_now"),
			&NovaPresentApplier::append_wire_row);
	ClassDB::bind_method(D_METHOD("append_wire_deferred", "node"),
			&NovaPresentApplier::append_wire_deferred);
	ClassDB::bind_method(
			D_METHOD("wire_plan_is_current", "snapshot_size", "stride",
					"layout_revision", "index_generation", "local_handle"),
			&NovaPresentApplier::wire_plan_is_current);
	ClassDB::bind_method(
			D_METHOD("present_wire_rows", "snap", "stride", "tick_delta"),
			&NovaPresentApplier::present_wire_rows);
	ClassDB::bind_method(D_METHOD("release_wire_handle", "handle"),
			&NovaPresentApplier::release_wire_handle);
	ClassDB::bind_method(D_METHOD("reset_wire_runtime_state"),
			&NovaPresentApplier::reset_wire_runtime_state);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("held_weapon_attach_transform", "skeleton", "attach_angles_bms",
					"hand_frame"),
			&NovaPresentApplier::held_weapon_attach_transform);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("held_weapon_hand_frame_basis", "bone_model_to_world"),
			&NovaPresentApplier::held_weapon_hand_frame_basis);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("aim_root_basis", "snap", "base", "fallback"),
			&NovaPresentApplier::aim_root_basis);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("aim_apply", "node", "snap", "base", "drive_root_basis"),
			&NovaPresentApplier::aim_apply);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("aim_apply_valid", "node", "snap", "base", "drive_root_basis"),
			&NovaPresentApplier::aim_apply_valid);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("get_visual_control_capabilities", "node"),
			&NovaPresentApplier::get_visual_control_capabilities);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("ctrl_set_with_capabilities", "node", "capabilities",
					"owner", "register", "value"),
			&NovaPresentApplier::ctrl_set_with_capabilities);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("ctrl_clear_with_capabilities", "node", "capabilities",
					"owner", "register"),
			&NovaPresentApplier::ctrl_clear_with_capabilities);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("wire_controls_apply_with_capabilities", "node", "snap",
					"base", "capabilities"),
			&NovaPresentApplier::wire_controls_apply_with_capabilities);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("emplaced_apply", "node", "snap", "base", "clear_when_invalid"),
			&NovaPresentApplier::emplaced_apply);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("emplaced_clear", "node"), &NovaPresentApplier::emplaced_clear);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("vehicle_motion_apply", "node", "snap", "base"),
			&NovaPresentApplier::vehicle_motion_apply);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("vehicle_motion_clear", "node"),
			&NovaPresentApplier::vehicle_motion_clear);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("zone_team_apply", "node", "snap", "base"),
			&NovaPresentApplier::zone_team_apply);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("zone_team_clear", "node"),
			&NovaPresentApplier::zone_team_clear);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("world_heat_apply", "node", "snap", "base"),
			&NovaPresentApplier::world_heat_apply);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("world_heat_clear", "node"),
			&NovaPresentApplier::world_heat_clear);
	BIND_ENUM_CONSTANT(OUTPUT_TRANSFORM);
	BIND_ENUM_CONSTANT(OUTPUT_PART_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_VISIBILITY);
	BIND_ENUM_CONSTANT(OUTPUT_BODY_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_ALL);
	BIND_ENUM_CONSTANT(VISUAL_CTRL_OWNED);
	BIND_ENUM_CONSTANT(VISUAL_CTRL_LEGACY);
	BIND_ENUM_CONSTANT(VISUAL_CTRL_BATCH);
	BIND_ENUM_CONSTANT(VISUAL_PART_PHASE);
	BIND_ENUM_CONSTANT(VISUAL_PART_CLEAR);
}

void NovaPresentApplier::setup(Object *sim, Object *index) {
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0) {
		release_part_anim_outputs();
	}
	sim_id_ = sim != nullptr ? sim->get_instance_id() : ObjectID();
	index_id_ = index != nullptr ? index->get_instance_id() : ObjectID();
	index_has_generation_ =
			index != nullptr && index->has_method(names().get_generation);
	plan_revision_ = -1; // force a rebuild against the new wiring
	plan_dirty_ = true;
	rows_.clear();
}

void NovaPresentApplier::set_output_channels(int channels) {
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

void NovaPresentApplier::set_shared_visibility_maps(
		const Dictionary &occlusion_hidden_ids, const Dictionary &present_visibility) {
	occlusion_hidden_ids_ = occlusion_hidden_ids;
	present_visibility_ = present_visibility;
}

Dictionary NovaPresentApplier::get_stats() const {
	Dictionary d;
	d["moved"] = stat_moved_;
	d["posed"] = stat_posed_;
	d["hidden"] = stat_hidden_;
	d["muzzles"] = stat_muzzles_;
	d["plan_rebuilds"] = stat_plan_rebuilds_;
	d["transform_builds"] = stat_transform_builds_;
	d["aim_dispatches"] = stat_aim_dispatches_;
	d["rhc_dispatches"] = stat_rhc_dispatches_;
	d["part_dispatches"] = stat_part_dispatches_;
	d["control_dispatches"] = stat_control_dispatches_;
	d["body_dispatches"] = stat_body_dispatches_;
	d["muzzle_queries"] = stat_muzzle_queries_;
	return d;
}

Basis NovaPresentApplier::bms_to_godot_basis(const Vector3 &rot_deg) {
	// R_godot = RotY(90 - yaw) * RotZ(pitch) * RotX(roll) * RotY(90) — the one
	// placement convention (see mission_object_placer.gd's [orig] derivation);
	// the GDScript twin is pinned equivalent by the basis parity GUT case.
	const double pitch = Math::deg_to_rad(static_cast<double>(rot_deg.x));
	const double yaw = Math::deg_to_rad(static_cast<double>(rot_deg.y));
	const double roll = Math::deg_to_rad(static_cast<double>(rot_deg.z));
	return Basis(Vector3(0, 1, 0), Math::deg_to_rad(90.0) - yaw) *
			Basis(Vector3(0, 0, 1), pitch) * Basis(Vector3(1, 0, 0), roll) *
			Basis(Vector3(0, 1, 0), Math::deg_to_rad(90.0));
}

namespace {

// The held weapon rides bone INDEX 16 (".bad row BN17 R Hand") with a fixed
// pivot nudge in raw def units, X negated into the render frame — the values
// and every derivation live at the GDScript reference, present_held_weapon.gd
// [orig: flt_7C68E8 = 0.05 +X/-Y, flt_7C9BA8 = 0.051 +Z @ 0x4b2186].
constexpr int kHeldWeaponBoneIndex = 16;
const Vector3 kHeldWeaponAttachNudge(-0.05f, -0.05f, 0.051f);
// Hand-frame calibration [orig: Rz dbl_7C9BA0 / Ry dbl_7C9B98 via
// Math_BuildRotationMatrix4x4_ByAxis @ 0x611db0].
constexpr double kHandFrameZRad = 0.5759761961496483;
constexpr double kHandFrameYRad = -1.3613982818082597;

} // namespace

Basis NovaPresentApplier::held_weapon_hand_frame_basis(const Basis &bone_model_to_world) {
	// Row-major `Ry_e · Rz_e · M16` = the calibrations on the RIGHT in column
	// form; signs as authored (two inversions cancel — present_held_weapon.gd
	// documents why) [orig: branch @ 0x4b220f, Rz @ 0x4b2215, Ry @ 0x4b2256].
	return bone_model_to_world * Basis(Vector3(0, 0, 1), kHandFrameZRad) *
			Basis(Vector3(0, 1, 0), kHandFrameYRad);
}

Variant NovaPresentApplier::held_weapon_attach_transform(Object *skeleton,
		const Vector3 &attach_angles_bms, bool hand_frame) {
	Skeleton3D *skel = Object::cast_to<Skeleton3D>(skeleton);
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

Basis NovaPresentApplier::aim_root_basis(const PackedFloat32Array &snap, int base,
		const Basis &fallback) {
	const float *p = snap.ptr();
	if (field_i(p, base, NovaSimulation::PF_AIM_OVERLAY_VALID) == 0) {
		return fallback;
	}
	return bms_to_godot_basis(
			Vector3(p[base + NovaSimulation::PF_AIM_BODY_PITCH_DEG],
					p[base + NovaSimulation::PF_AIM_BODY_YAW_DEG],
					p[base + NovaSimulation::PF_AIM_BODY_ROLL_DEG]));
}

void NovaPresentApplier::aim_apply(Object *node, const PackedFloat32Array &snap,
		int base, bool drive_root_basis) {
	if (node == nullptr) {
		return;
	}
	const float *p = snap.ptr();
	// The mounted-seat selector's final skeletal verdict applies to placed and
	// wire models alike [orig: BN17 special row @ 0x4b1290].
	if (node->has_method(names().set_right_hand_collapsed)) {
		node->call(names().set_right_hand_collapsed,
				field_i(p, base, NovaSimulation::PF_RIGHT_HAND_COLLAPSED) != 0);
	}
	if (!node->has_method(names().set_aim_overlay)) {
		return;
	}
	if (field_i(p, base, NovaSimulation::PF_AIM_OVERLAY_VALID) == 0) {
		node->call(names().set_aim_overlay, Array());
		return;
	}
	aim_apply_valid(node, snap, base, drive_root_basis);
}

void NovaPresentApplier::aim_apply_valid(Object *node,
		const PackedFloat32Array &snap, int base, bool drive_root_basis) {
	if (node == nullptr) {
		return;
	}
	Node3D *n3 = Object::cast_to<Node3D>(node);
	const Basis current_basis = n3 != nullptr
			? n3->get_basis()
			: static_cast<Basis>(node->get(names().basis));
	const Basis body_basis = aim_root_basis(snap, base, current_basis);
	if (drive_root_basis && current_basis != body_basis) {
		if (n3 != nullptr) {
			n3->set_basis(body_basis);
		} else {
			node->set(names().basis, body_basis);
		}
	}
	const Basis inverse_body = body_basis.inverse();
	const float *p = snap.ptr();
	Array deltas;
	deltas.resize(9);
	for (int overlay_class = 0; overlay_class < 9; ++overlay_class) {
		const int offset = base + NovaSimulation::PF_AIM_ANGLES +
				overlay_class * NovaSimulation::PF_AIM_CLASS_STRIDE;
		deltas[overlay_class] = inverse_body *
				bms_to_godot_basis(
						Vector3(p[offset], p[offset + 1], p[offset + 2]));
	}
	node->call(names().set_aim_overlay, deltas);
}

int NovaPresentApplier::get_visual_control_capabilities(Object *node) {
	return visual_control_capabilities(node);
}

namespace {

void emplaced_clear_capable(Object *node, int capabilities);
void vehicle_motion_clear_capable(Object *node, int capabilities);
void zone_team_clear_capable(Object *node, int capabilities);
void world_heat_clear_capable(Object *node, int capabilities);

int emplaced_apply_capable(Object *node, const PackedFloat32Array &snap,
		int base, bool clear_when_invalid, int capabilities) {
	if (!has_ctrl_surface(capabilities)) {
		return 0;
	}
	const DispatchNames &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, NovaSimulation::PF_EMPLACED_CONTROLS_VALID) == 1) {
		set_owned_ctrl(node, capabilities, n.owner_emplaced, n.eweap_gunyaw,
				field_i(p, base, NovaSimulation::PF_EWEAP_GUNYAW));
		set_owned_ctrl(node, capabilities, n.owner_emplaced, n.eweap_gunpitch,
				field_i(p, base, NovaSimulation::PF_EWEAP_GUNPITCH));
		return 2;
	}
	// Nodes persist across dismount/death: remove only the two controls this
	// presenter owns — clear_ctrl_values() would also erase live WAC channels.
	if (clear_when_invalid) {
		emplaced_clear_capable(node, capabilities);
	}
	return 0;
}

void emplaced_clear_capable(Object *node, int capabilities) {
	const DispatchNames &n = names();
	clear_owned_ctrl(
			node, capabilities, n.owner_emplaced, n.eweap_gunyaw);
	clear_owned_ctrl(
			node, capabilities, n.owner_emplaced, n.eweap_gunpitch);
}

int vehicle_motion_apply_capable(Object *node,
		const PackedFloat32Array &snap, int base, int capabilities) {
	if (!has_ctrl_surface(capabilities)) return 0;
	const DispatchNames &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, NovaSimulation::PF_VEHICLE_MOTION_VALID) == 1) {
		// Both fields are owned even at rest (literal zero), exactly as the
		// cveh callback stores them immediately before model submission.
		// [orig: Entity_CacheVehicleHUDStats @0x4929B0;
		//  stores @0x4929D7 / @0x4929F1]
		set_owned_ctrl(node, capabilities, n.owner_vehicle_motion,
				n.vehicle_steering,
				field_i(p, base, NovaSimulation::PF_VEHICLE_STEERING));
		set_owned_ctrl(node, capabilities, n.owner_vehicle_motion,
				n.vehicle_speed,
				field_i(p, base, NovaSimulation::PF_VEHICLE_SPEED));
		return 2;
	}
	vehicle_motion_clear_capable(node, capabilities);
	return 0;
}

void vehicle_motion_clear_capable(Object *node, int capabilities) {
	const DispatchNames &n = names();
	clear_owned_ctrl(node, capabilities, n.owner_vehicle_motion,
			n.vehicle_steering);
	clear_owned_ctrl(
			node, capabilities, n.owner_vehicle_motion, n.vehicle_speed);
}

int zone_team_apply_capable(Object *node, const PackedFloat32Array &snap,
		int base, int capabilities) {
	if (!has_ctrl_surface(capabilities)) return 0;
	const DispatchNames &n = names();
	const float *p = snap.ptr();
	int writes = 0;
	if (field_i(p, base, NovaSimulation::PF_TEX_TEAM_VALID) == 1) {
		set_owned_ctrl(node, capabilities, n.owner_sector_team, n.tex_team,
				field_i(p, base, NovaSimulation::PF_TEX_TEAM));
		++writes;
	} else {
		clear_owned_ctrl(
				node, capabilities, n.owner_sector_team, n.tex_team);
	}
	if (field_i(p, base, NovaSimulation::PF_ZONE_CTRL_VALID) == 1) {
		// TEAMSWING is an unconditional store inside the packed-zone-byte
		// branch, including literal zero for team 1.
		set_owned_ctrl(node, capabilities, n.owner_zone, n.team_swing,
				field_i(p, base, NovaSimulation::PF_TEAMSWING));
		++writes;
	} else {
		clear_owned_ctrl(node, capabilities, n.owner_zone, n.team_swing);
	}
	if (field_i(p, base, NovaSimulation::PF_LFP_CAMPPERCENT_VALID) == 1) {
		set_owned_ctrl(node, capabilities, n.owner_zone, n.lfp_camp_percent,
				field_i(p, base, NovaSimulation::PF_LFP_CAMPPERCENT));
		++writes;
	} else {
		// A numbered zone without a timer-list entry does not write LFP at all.
		// Releasing our bounded per-model writer represents that omission; it is
		// deliberately not a fabricated zero store.
		clear_owned_ctrl(
				node, capabilities, n.owner_zone, n.lfp_camp_percent);
	}
	return writes;
}

void zone_team_clear_capable(Object *node, int capabilities) {
	const DispatchNames &n = names();
	clear_owned_ctrl(
			node, capabilities, n.owner_sector_team, n.tex_team);
	clear_owned_ctrl(node, capabilities, n.owner_zone, n.team_swing);
	clear_owned_ctrl(
			node, capabilities, n.owner_zone, n.lfp_camp_percent);
}

int world_heat_apply_capable(Object *node, const PackedFloat32Array &snap,
		int base, int capabilities) {
	if (!has_ctrl_surface(capabilities)) return 0;
	const DispatchNames &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, NovaSimulation::PF_WORLD_HEAT_GLOW_VALID) == 1) {
		// The valid carrier-attachment scope owns cold zero too.
		// [orig: HUD_CacheWeaponSlotInfo @ 0x440969 / @ 0x440991,
		//  sole caller @ 0x546518]
		set_owned_ctrl(node, capabilities, n.owner_world_heat, n.heat_glow,
				field_i(p, base, NovaSimulation::PF_WORLD_HEAT_GLOW));
		return 1;
	}
	world_heat_clear_capable(node, capabilities);
	return 0;
}

void world_heat_clear_capable(Object *node, int capabilities) {
	const DispatchNames &n = names();
	clear_owned_ctrl(node, capabilities, n.owner_world_heat, n.heat_glow);
}

} // namespace

void NovaPresentApplier::ctrl_set_with_capabilities(Object *node,
		int capabilities, const String &owner, const String &reg, int value) {
	set_owned_ctrl(node, capabilities, owner, reg, value);
}

void NovaPresentApplier::ctrl_clear_with_capabilities(Object *node,
		int capabilities, const String &owner, const String &reg) {
	clear_owned_ctrl(node, capabilities, owner, reg);
}

int NovaPresentApplier::wire_controls_apply_with_capabilities(Object *node,
		const PackedFloat32Array &snap, int base, int capabilities) {
	int writes = 0;
	writes += emplaced_apply_capable(
			node, snap, base, true, capabilities);
	writes += vehicle_motion_apply_capable(
			node, snap, base, capabilities);
	writes += zone_team_apply_capable(
			node, snap, base, capabilities);
	writes += world_heat_apply_capable(
			node, snap, base, capabilities);
	return writes;
}

int NovaPresentApplier::emplaced_apply(Object *node,
		const PackedFloat32Array &snap, int base, bool clear_when_invalid) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	return emplaced_apply_capable(
			node, snap, base, clear_when_invalid, capabilities);
}

void NovaPresentApplier::emplaced_clear(Object *node) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	emplaced_clear_capable(node, capabilities);
}

int NovaPresentApplier::vehicle_motion_apply(Object *node,
		const PackedFloat32Array &snap, int base) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	return vehicle_motion_apply_capable(node, snap, base, capabilities);
}

void NovaPresentApplier::vehicle_motion_clear(Object *node) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	vehicle_motion_clear_capable(node, capabilities);
}

int NovaPresentApplier::zone_team_apply(Object *node,
		const PackedFloat32Array &snap, int base) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	return zone_team_apply_capable(node, snap, base, capabilities);
}

void NovaPresentApplier::zone_team_clear(Object *node) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	zone_team_clear_capable(node, capabilities);
}

int NovaPresentApplier::world_heat_apply(Object *node,
		const PackedFloat32Array &snap, int base) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	return world_heat_apply_capable(node, snap, base, capabilities);
}

void NovaPresentApplier::world_heat_clear(Object *node) {
	const int capabilities = ctrl_dispatch_capabilities(node);
	world_heat_clear_capable(node, capabilities);
}

int64_t NovaPresentApplier::current_index_generation() {
	if (!index_has_generation_) {
		return 0;
	}
	Object *index = ObjectDB::get_instance(index_id_);
	if (index == nullptr) {
		return 0;
	}
	return static_cast<int64_t>(index->call(names().get_generation));
}

bool NovaPresentApplier::row_plan_is_current(int64_t size, int stride,
		int64_t layout_revision) {
	// No revision means a compatible fake/custom source: preserve the original
	// full-resolution behavior rather than trusting an unverifiable row order.
	if (plan_dirty_ || layout_revision < 0 ||
			plan_revision_ != layout_revision ||
			plan_stride_ != stride || plan_snapshot_size_ != size ||
			plan_index_generation_ != current_index_generation()) {
		return false;
	}
	return true;
}

void NovaPresentApplier::rebuild_row_plan(const float *p, int64_t size, int stride,
		int64_t layout_revision) {
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0) {
		release_part_anim_outputs();
	}
	++stat_plan_rebuilds_;
	rows_.clear();
	present_visibility_.clear();
	plan_revision_ = layout_revision;
	plan_stride_ = stride;
	plan_snapshot_size_ = size;
	plan_index_generation_ = current_index_generation();
	plan_dirty_ = false;
	Object *index = ObjectDB::get_instance(index_id_);
	if (index == nullptr) {
		plan_dirty_ = true;
		return;
	}
	const int64_t count = size / stride;
	rows_.reserve(static_cast<size_t>(count));
	for (int64_t r = 0; r < count; ++r) {
		const int base = static_cast<int>(r * stride);
		const int32_t bms_id = field_i(p, base, NovaSimulation::PF_BMS_ID);
		const int32_t kind = field_i(p, base, NovaSimulation::PF_KIND);
		const int32_t idx = field_i(p, base, NovaSimulation::PF_INDEX);
		Object *node = index->call(names().resolve, bms_id, kind, idx);
		if (node == nullptr) {
			continue;
		}
		Row row;
		row.base = base;
		row.node_id = node->get_instance_id();
		row.bms_id = bms_id;
		// Capability lookups are stable per node script: resolve them once per
		// topology change so the per-frame loop never pays has_method() again.
		int caps = 0;
		if (node->has_method(names().play_body_clip_at)) {
			caps |= CAP_BODY_CLIP;
		}
		if (node->has_method(names().play_body_blend_at)) {
			caps |= CAP_BODY_BLEND;
		}
		if (node->has_method(names().play_body_anim_at)) {
			caps |= CAP_BODY_SLOT;
		}
		if (node->has_method(names().play_body_anim)) {
			caps |= CAP_BODY_PLAY;
		}
		if (node->has_method(names().set_aim_overlay)) {
			caps |= CAP_AIM;
		}
		if (node->has_method(names().set_right_hand_collapsed)) {
			caps |= CAP_RHC;
		}
		const int visual_ctrl_caps = visual_control_capabilities(node);
		if ((visual_ctrl_caps &
					(VISUAL_CTRL_OWNED | VISUAL_CTRL_LEGACY)) != 0) {
			caps |= CAP_CTRL;
		}
		if ((visual_ctrl_caps & VISUAL_CTRL_BATCH) != 0) {
			caps |= CAP_CTRL_BATCH;
		}
		if (node->has_method(names().has_muzzle) &&
				node->has_method(names().get_muzzle_world_position) &&
				static_cast<bool>(node->call(names().has_muzzle))) {
			caps |= CAP_MUZZLE;
		}
		if ((visual_ctrl_caps & VISUAL_PART_PHASE) != 0) {
			caps |= CAP_PART;
		}
		if ((visual_ctrl_caps & VISUAL_PART_CLEAR) != 0) {
			caps |= CAP_PART_CLEAR;
		}
		row.caps = caps;
		row.visual_ctrl_caps = visual_ctrl_caps;
		rows_.push_back(row);
	}
}

void NovaPresentApplier::release_part_anim_outputs() {
	const DispatchNames &n = names();
	for (const Row &row : rows_) {
		Object *node = ObjectDB::get_instance(row.node_id);
		if (node == nullptr) continue;
		const bool batch = (row.caps & CAP_CTRL_BATCH) != 0;
		if (batch) node->call(n.begin_ctrl_update);
		if ((row.caps & CAP_PART_CLEAR) != 0) {
			node->call(n.clear_part_phase, 1);
			node->call(n.clear_part_phase, 2);
		}
		emplaced_clear_capable(node, row.visual_ctrl_caps);
		vehicle_motion_clear_capable(node, row.visual_ctrl_caps);
		zone_team_clear_capable(node, row.visual_ctrl_caps);
		world_heat_clear_capable(node, row.visual_ctrl_caps);
		if (batch) node->call(n.end_ctrl_update);
	}
}

const String &NovaPresentApplier::infantry_key(int state) {
	auto it = infantry_keys_.find(state);
	if (it == infantry_keys_.end()) {
		it = infantry_keys_.emplace(state, NovaSimulation::infantry_anim_key(state))
					 .first;
	}
	return it->second;
}

void NovaPresentApplier::present_snapshot(const PackedFloat32Array &snap,
		int stride, int64_t layout_revision) {
	if (stride < NovaSimulation::PF_STRIDE || index_id_ == ObjectID()) {
		return;
	}
	const float *p = snap.ptr();
	const int64_t size = snap.size();
	if (!row_plan_is_current(size, stride, layout_revision)) {
		rebuild_row_plan(p, size, stride, layout_revision);
	}
	Object *sim = ObjectDB::get_instance(sim_id_);
	NovaSimulation *native_sim = Object::cast_to<NovaSimulation>(sim);
	const DispatchNames &n = names();
	for (Row &row : rows_) {
		Object *node = ObjectDB::get_instance(row.node_id);
		if (node == nullptr) {
			// Revisioned snapshots trust their topology stamp instead of
			// pre-scanning every ObjectID. Rebind on the next frame and release
			// this row's visibility intent immediately.
			present_visibility_.erase(row.bms_id);
			row.present_visible = -1;
			plan_dirty_ = true;
			continue;
		}
		Node3D *n3 = Object::cast_to<Node3D>(node);
		const int base = row.base;
		const int caps = row.caps;
		if ((output_channels_ & OUTPUT_TRANSFORM) != 0 && n3 != nullptr) {
			// Compare the six packed source floats before constructing either
			// the placement Basis or Transform3D.
			// Position is already Godot-space (x, z, -y); rotation is
			// mission-space degrees, built through the ONE placement convention
			// so a sim-driven entity sits exactly where placement would put it.
			const bool aim_owns_root = (caps & CAP_AIM) != 0 &&
					field_i(p, base,
							NovaSimulation::PF_AIM_OVERLAY_VALID) != 0;
			const int rotation_field = aim_owns_root
					? NovaSimulation::PF_AIM_BODY_PITCH_DEG
					: NovaSimulation::PF_PITCH_DEG;
			const std::array<float, 6> next_stamp = {
				p[base + NovaSimulation::PF_POS_X],
				p[base + NovaSimulation::PF_POS_Y],
				p[base + NovaSimulation::PF_POS_Z],
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
				const Transform3D next(
						bms_to_godot_basis(
								Vector3(next_stamp[3], next_stamp[4],
										next_stamp[5])),
						Vector3(next_stamp[0], next_stamp[1], next_stamp[2]));
				++stat_transform_builds_;
				if (row.transform_stamp_valid ||
						n3->get_transform() != next) {
					n3->set_transform(next);
					++stat_moved_;
				}
				row.transform_stamp = next_stamp;
				row.transform_stamp_valid = true;
			}
		}
		// Aim overlay with the capability lookups hoisted into the row plan and
		// the no-overlay clear gated to the valid->invalid edge (the node-side
		// setters no-op on repeats; these gates skip the dispatch itself).
		bool body_dependency_changed = false;
		if ((caps & CAP_RHC) != 0) {
			const int32_t rhc =
					field_i(p, base, NovaSimulation::PF_RIGHT_HAND_COLLAPSED);
			if (rhc != row.rhc) {
				node->call(n.set_right_hand_collapsed, rhc != 0);
				++stat_rhc_dispatches_;
				row.rhc = rhc;
				body_dependency_changed = true;
			}
		}
		if ((caps & CAP_AIM) != 0) {
			const int32_t aim_valid =
					field_i(p, base, NovaSimulation::PF_AIM_OVERLAY_VALID);
			if (aim_valid != 0) {
				bool payload_changed =
						row.aim_valid != 1 || !row.aim_payload_valid;
				if (!payload_changed) {
					for (int i = 0; i < AIM_PAYLOAD_FLOATS; ++i) {
						if (row.aim_payload[static_cast<size_t>(i)] !=
								p[base +
										NovaSimulation::PF_AIM_BODY_PITCH_DEG +
										i]) {
							payload_changed = true;
							break;
						}
					}
				}
				if (payload_changed) {
					aim_apply_valid(node, snap, base, false);
					++stat_aim_dispatches_;
					std::copy_n(
							p + base +
									NovaSimulation::PF_AIM_BODY_PITCH_DEG,
							AIM_PAYLOAD_FLOATS, row.aim_payload.begin());
					row.aim_payload_valid = true;
					body_dependency_changed = true;
				}
				row.aim_valid = 1;
			} else if (row.aim_valid != 0) {
				node->call(n.set_aim_overlay, Array());
				++stat_aim_dispatches_;
				row.aim_valid = 0;
				row.aim_payload_valid = false;
				body_dependency_changed = true;
			} else {
				row.aim_valid = 0;
			}
		}
		if ((output_channels_ & OUTPUT_PART_ANIM) != 0 &&
				(caps & (CAP_PART | CAP_PART_CLEAR | CAP_CTRL)) != 0) {
			const int32_t active1_code =
					field_i(p, base, NovaSimulation::PF_ACTIVE1);
			const int32_t active2_code =
					field_i(p, base, NovaSimulation::PF_ACTIVE2);
			const int32_t active1 =
					active1_code > 0 && active1_code <= 0x10000 ? 1 : 0;
			const int32_t active2 =
					active2_code > 0 && active2_code <= 0x10000 ? 1 : 0;
			const int32_t phase1 = active1 != 0
					? NovaSimulation::decode_present_part_anim_phase(
							snap, base, 1)
					: 0;
			const int32_t phase2 = active2 != 0
					? NovaSimulation::decode_present_part_anim_phase(
							snap, base, 2)
					: 0;
			const int32_t controls_valid =
					field_i(p, base,
							NovaSimulation::PF_EMPLACED_CONTROLS_VALID) == 1
					? 1
					: 0;
			const int32_t vehicle_valid =
					field_i(p, base,
							NovaSimulation::PF_VEHICLE_MOTION_VALID) == 1
					? 1
					: 0;
			const int32_t tex_team_valid =
					field_i(p, base, NovaSimulation::PF_TEX_TEAM_VALID) == 1
					? 1
					: 0;
			const int32_t zone_valid =
					field_i(p, base, NovaSimulation::PF_ZONE_CTRL_VALID) == 1
					? 1
					: 0;
			const int32_t lfp_valid =
					field_i(p, base,
							NovaSimulation::PF_LFP_CAMPPERCENT_VALID) == 1
					? 1
					: 0;
			const int32_t heat_valid =
					field_i(p, base,
							NovaSimulation::PF_WORLD_HEAT_GLOW_VALID) == 1
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
			const bool set_part1 =
					active1 != 0 && (caps & CAP_PART) != 0;
			const bool set_part2 =
					active2 != 0 && (caps & CAP_PART) != 0;
			const bool clear_part1 =
					active1 == 0 && (caps & CAP_PART_CLEAR) != 0 &&
					(cold || was_published(CTRL_PUBLISH_PART1));
			const bool clear_part2 =
					active2 == 0 && (caps & CAP_PART_CLEAR) != 0 &&
					(cold || was_published(CTRL_PUBLISH_PART2));
			// A valid writer executes for every retail model submission. This
			// reasserts ownership after any intervening producer, while the
			// model-side setter makes an unchanged owner/value a true no-op.
			// Omitted writers clear only on a cold/falling edge.
			const bool emplaced_work = (caps & CAP_CTRL) != 0 &&
					(controls_valid != 0 || cold ||
							was_published(CTRL_PUBLISH_EMPLACED));
			const bool vehicle_work = (caps & CAP_CTRL) != 0 &&
					(vehicle_valid != 0 || cold ||
							was_published(CTRL_PUBLISH_VEHICLE));
			const bool zone_work = (caps & CAP_CTRL) != 0 &&
					(tex_team_valid != 0 || zone_valid != 0 ||
							lfp_valid != 0 || cold ||
							was_published(CTRL_PUBLISH_TEX_TEAM) ||
							was_published(CTRL_PUBLISH_ZONE) ||
							was_published(CTRL_PUBLISH_LFP));
			const bool heat_work = (caps & CAP_CTRL) != 0 &&
					(heat_valid != 0 || cold ||
							was_published(CTRL_PUBLISH_HEAT));
			const bool any_work = set_part1 || set_part2 ||
					clear_part1 || clear_part2 || emplaced_work ||
					vehicle_work || zone_work || heat_work;
			if (any_work && (caps & CAP_CTRL_BATCH) != 0) {
				node->call(n.begin_ctrl_update);
			}
			// A falling/cold EWEAP release precedes generic phase replay. This
			// preserves the master compatibility surface for a third-party node
			// that aliases a part channel onto EWEAP, without the old
			// unconditional clear/re-add on every valid frame. Production
			// NovaObjectModel uses fixed VEHICLE_SPECIAL1/2 and cannot alias it.
			if (emplaced_work && controls_valid == 0) {
				emplaced_clear_capable(node, row.visual_ctrl_caps);
				stat_control_dispatches_ += 2;
			}
			// PANM phases are integrated by the engine [orig:
			// Entity_ApplyCommand @ 0x43ab60 case 0x22]. ACTIVE is the
			// publication bit: inactive releases this presenter's slot.
			if (set_part1) {
				node->call(n.set_part_phase, 1, phase1);
				++stat_posed_;
				++stat_part_dispatches_;
			} else if (clear_part1) {
				node->call(n.clear_part_phase, 1);
				++stat_part_dispatches_;
			}
			if (set_part2) {
				node->call(n.set_part_phase, 2, phase2);
				++stat_posed_;
				++stat_part_dispatches_;
			} else if (clear_part2) {
				node->call(n.clear_part_phase, 2);
				++stat_part_dispatches_;
			}
			if (emplaced_work && controls_valid != 0) {
				const int applied = emplaced_apply_capable(
						node, snap, base, false, row.visual_ctrl_caps);
				stat_posed_ += applied;
				stat_control_dispatches_ += applied;
			}
			if (vehicle_work) {
				const int applied = vehicle_motion_apply_capable(
						node, snap, base, row.visual_ctrl_caps);
				stat_posed_ += applied;
				stat_control_dispatches_ += 2;
			}
			if (zone_work) {
				const int applied = zone_team_apply_capable(
						node, snap, base, row.visual_ctrl_caps);
				stat_posed_ += applied;
				stat_control_dispatches_ += 3;
			}
			if (heat_work) {
				const int applied = world_heat_apply_capable(
						node, snap, base, row.visual_ctrl_caps);
				stat_posed_ += applied;
				++stat_control_dispatches_;
			}
			if (any_work && (caps & CAP_CTRL_BATCH) != 0) {
				node->call(n.end_ctrl_update);
			}
			row.ctrl_publish_state = next_ctrl_publish_state;
			row.ctrl_publish_state_valid = true;
		}
		const bool present_visible =
				field_i(p, base, NovaSimulation::PF_HIDDEN) == 0 &&
				field_i(p, base, NovaSimulation::PF_LOCAL_VIEW_SUPPRESSED) == 0;
		const int32_t present_visible_int = present_visible ? 1 : 0;
		if (present_visible_int != row.present_visible) {
			present_visibility_[row.bms_id] = present_visible;
			row.present_visible = present_visible_int;
		}
		if ((output_channels_ & OUTPUT_VISIBILITY) != 0 && n3 != nullptr) {
			// Death is not disappearance (corpses and husks keep rendering until
			// the sim despawns via PF_HIDDEN); the local first-person UseGun
			// parent's own world model is presentation-suppressed. Semantics and
			// witnesses recorded at the GDScript origin (mission_present_pass.gd)
			// [orig: Entity_RenderVehicleModel @ 0x4407d0 cull/submit;
			// Flags&4 husk pick @ 0x413086].
			if (n3->is_visible() != present_visible) {
				// Two-bit visibility ownership: while the render-occlusion frame
				// claims this node (the shared hidden set), a sim-wants-visible
				// node stays hidden — occlusion releases through the same set.
				if (!(present_visible &&
							occlusion_hidden_ids_.has(row.bms_id))) {
					n3->set_visible(present_visible);
				}
			}
			if (!present_visible) {
				++stat_hidden_;
			}
		}
		const int32_t net_id = field_i(p, base, NovaSimulation::PF_NET_ID);
		// Hidden models skip skeletal writes unless they own the authoritative
		// posed-muzzle feedback seam (AI fire origins survive; hidden non-weapon
		// actors take the cheap path).
		const bool body_eligible =
				present_visible ||
				(net_id > 0 && (caps & CAP_MUZZLE) != 0);
		if ((output_channels_ & OUTPUT_BODY_ANIM) != 0 && body_eligible) {
			// Main-body skeletal clip: infantry poses to the exact anim-state
			// phase that produced root motion; PF_BODY_ANIM_SLOT is the coarse
			// fallback for compatible non-infantry nodes.
			const int32_t anim_state =
					field_i(p, base, NovaSimulation::PF_ANIM_STATE);
			int32_t body_mode = BODY_NONE;
			int32_t body_selector = -1;
			int32_t body_phase = 0;
			int32_t body_source_selector = -1;
			int32_t body_source_phase = 0;
			float body_blend_weight = 1.0f;
			String body_clip_key;
			String body_source_clip_key;
			if ((caps & CAP_BODY_CLIP) != 0) {
				const String key =
						anim_state >= 0 ? infantry_key(anim_state) : String();
				const int32_t source_state = field_i(
						p, base, NovaSimulation::PF_ANIM_SOURCE_STATE);
				const String source_key =
						source_state >= 0 ? infantry_key(source_state) : String();
				if (!key.is_empty()) {
					body_selector = anim_state;
					body_phase = field_i(
							p, base, NovaSimulation::PF_ANIM_PHASE_TICKS);
					body_clip_key = key;
					const float target_weight =
							p[base + NovaSimulation::PF_ANIM_BLEND_WEIGHT];
					if (source_state >= 0 && !source_key.is_empty() &&
							target_weight < 1.0f &&
							(caps & CAP_BODY_BLEND) != 0) {
						body_mode = BODY_BLEND_AT;
						body_source_selector = source_state;
						body_source_phase = field_i(
								p, base,
								NovaSimulation::PF_ANIM_SOURCE_PHASE_TICKS);
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
							NovaSimulation::PF_ANIM_SOURCE_PHASE_TICKS);
					body_clip_key = source_key;
				}
			}
			if (body_mode == BODY_NONE) {
				const int32_t body_anim_slot =
						field_i(p, base, NovaSimulation::PF_BODY_ANIM_SLOT);
				if (body_anim_slot >= 0) {
					if ((caps & CAP_BODY_SLOT) != 0) {
						body_mode = BODY_SLOT_AT;
						body_selector = body_anim_slot;
						body_phase = field_i(
								p, base,
								NovaSimulation::PF_ANIM_PHASE_TICKS);
					} else if ((caps & CAP_BODY_PLAY) != 0) {
						body_mode = BODY_SLOT_PLAY;
						body_selector = body_anim_slot;
					}
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
						node->call(n.play_body_clip_at, body_clip_key,
								body_phase);
						++stat_body_dispatches_;
						break;
					case BODY_BLEND_AT:
						node->call(n.play_body_blend_at,
								body_source_clip_key, body_source_phase,
								body_clip_key, body_phase,
								body_blend_weight);
						++stat_body_dispatches_;
						break;
					case BODY_SLOT_AT:
						node->call(n.play_body_anim_at, body_selector,
								body_phase);
						++stat_body_dispatches_;
						break;
					case BODY_SLOT_PLAY:
						node->call(n.play_body_anim, body_selector);
						++stat_body_dispatches_;
						break;
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
		if (net_id > 0 && (caps & CAP_MUZZLE) != 0) {
			// The D-AI-6 muzzle seam: feed the posed gun-flash userpoint back to
			// the sim so AI rounds leave the GUN (one-frame staleness, ledgered)
			// [orig: Entity_GetAttachmentWorldPosition @ 0x4b2670 — our sim has
			// no skeletal pose, so the present layer pushes it back].
			++stat_muzzle_queries_;
			if (static_cast<bool>(node->call(n.has_muzzle))) {
				const Vector3 muzzle = node->call(n.get_muzzle_world_position);
				if (native_sim != nullptr) {
					native_sim->set_ai_muzzle_world(net_id, muzzle);
					++stat_muzzles_;
				} else if (sim != nullptr &&
						sim->has_method(n.set_ai_muzzle_world)) {
					sim->call(n.set_ai_muzzle_world, net_id, muzzle);
					++stat_muzzles_;
				}
			}
		}
	}
}
