#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/local_player_view.h>

namespace godot {

// The local player's view-state snapshot (world::LocalPlayerViewFrame), ticked
// in the SIM at the world cadence (62.5 Hz) so the ADS ease, the fov policy and
// the third-person anchor chase are render-rate independent; presenters only
// read it and place nodes. Positions are Godot space; angles mission-euler
// degrees. Field witnesses live on the engine struct
// (<runtime/world/local_player_view.h>, <runtime/world/player_view.h>).
class PlayerLocalView : public RefCounted {
	GDCLASS(PlayerLocalView, RefCounted)

	opennova::world::LocalPlayerViewFrame value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::LocalPlayerViewFrame &p_value) { value_ = p_value; }

	bool get_scope_engaged() const { return value_.scope_engaged; }
	bool get_mounted() const { return value_.mounted; }
	// The RESOLVED camera mode (0 first person, 1 the chase, 4 the death lerp),
	// the chase preference behind it, and the mounted camera leg.
	bool get_third_person() const { return value_.third_person; }
	bool get_third_person_selected() const { return value_.third_person_selected; }
	int get_camera_mode() const { return value_.camera_mode; }
	bool get_camera_mounted() const { return value_.camera_mounted; }
	bool get_vehicle_attack_context() const { return value_.vehicle_attack_context; }
	// 0 = hip .. 1 = sighted, over the toggle's ease steps.
	float get_scope_fraction() const { return value_.scope_fraction; }
	// The NoCardSwitch reload rule: the FP view bias is dropped for the frame.
	bool get_suppress_view_bias() const { return value_.suppress_view_bias; }
	// A Scoped/Sighted weapon at full raise shows its SIGHTS rows instead of the viewmodel.
	bool get_scope_card_active() const { return value_.scope_card_active; }
	bool get_binoculars_requested() const { return value_.binoculars_requested; }
	bool get_binoculars_raised() const { return value_.binoculars_raised; }
	bool get_binoculars_view_active() const { return value_.binoculars_view_active; }
	float get_binocular_yaw_offset_deg() const { return value_.binocular_yaw_offset_deg; }
	float get_binocular_pitch_offset_deg() const { return value_.binocular_pitch_offset_deg; }
	bool get_nvg_active() const { return value_.nvg_active; }
	bool get_nvg_visible() const { return value_.nvg_visible; }
	int get_nvg_gain() const { return value_.nvg_gain; }
	// The thermal-imaging view's two gates (the engine struct carries the
	// witnesses): the world block / fog / clear latch, and the terrain-ramp gate.
	bool get_thermal_view() const { return value_.thermal_view; }
	bool get_thermal_terrain_view() const { return value_.thermal_terrain_view; }
	// The main camera's HORIZONTAL fov with the policy applied.
	float get_fov_h_deg() const { return value_.fov_h_deg; }
	// The chased eye anchor (Godot space).
	Vector3 get_tp_anchor() const;
	bool get_tp_anchor_valid() const { return value_.tp_anchor_valid; }
	// Retail's camera-only recoil doubling and the composed FP roll
	// (torsoRoll + lean/4); 0 without a local body to compose over.
	float get_fp_pitch_recoil_deg() const;
	float get_fp_roll_deg() const;
	// The composed camera pose (world/player_view.h): eye in Godot space,
	// mission-euler view angles; the angles read 0 while the pose is invalid.
	bool get_camera_pose_valid() const { return value_.camera_pose_valid; }
	Vector3 get_camera_eye() const;
	float get_camera_yaw_deg() const;
	float get_camera_pitch_deg() const;
	float get_camera_roll_deg() const;
};

} // namespace godot
