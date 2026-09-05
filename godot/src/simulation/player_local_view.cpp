#include "simulation/player_local_view.h"

#include "util/axes.h"

using namespace godot;

Vector3 PlayerLocalView::get_tp_anchor() const {
	return mission_to_godot(value_.tp_anchor);
}

float PlayerLocalView::get_fp_pitch_recoil_deg() const {
	return value_.fp_terms_valid ? value_.fp_pitch_recoil_deg : 0.0f;
}

float PlayerLocalView::get_fp_roll_deg() const {
	return value_.fp_terms_valid ? value_.fp_roll_deg : 0.0f;
}

Vector3 PlayerLocalView::get_camera_eye() const {
	return value_.camera_pose_valid ? mission_to_godot(value_.camera.eye) : Vector3();
}

float PlayerLocalView::get_camera_yaw_deg() const {
	return value_.camera_pose_valid ? value_.camera.yaw_deg : 0.0f;
}

float PlayerLocalView::get_camera_pitch_deg() const {
	return value_.camera_pose_valid ? value_.camera.pitch_deg : 0.0f;
}

float PlayerLocalView::get_camera_roll_deg() const {
	return value_.camera_pose_valid ? value_.camera.roll_deg : 0.0f;
}

void PlayerLocalView::_bind_methods() {
#define PLAYER_LOCAL_VIEW_FIELD(m_variant, m_name)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerLocalView::get_##m_name);       \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name, PROPERTY_HINT_NONE, "",                 \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),              \
			"", "get_" #m_name);
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, scope_engaged)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, mounted)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, third_person)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, third_person_selected)
	PLAYER_LOCAL_VIEW_FIELD(Variant::INT, camera_mode)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, camera_mounted)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, vehicle_attack_context)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, scope_fraction)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, suppress_view_bias)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, scope_card_active)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, binoculars_requested)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, binoculars_raised)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, binoculars_view_active)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, binocular_yaw_offset_deg)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, binocular_pitch_offset_deg)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, nvg_active)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, nvg_visible)
	PLAYER_LOCAL_VIEW_FIELD(Variant::INT, nvg_gain)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, fov_h_deg)
	PLAYER_LOCAL_VIEW_FIELD(Variant::VECTOR3, tp_anchor)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, tp_anchor_valid)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, fp_pitch_recoil_deg)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, fp_roll_deg)
	PLAYER_LOCAL_VIEW_FIELD(Variant::BOOL, camera_pose_valid)
	PLAYER_LOCAL_VIEW_FIELD(Variant::VECTOR3, camera_eye)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, camera_yaw_deg)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, camera_pitch_deg)
	PLAYER_LOCAL_VIEW_FIELD(Variant::FLOAT, camera_roll_deg)
#undef PLAYER_LOCAL_VIEW_FIELD
}
