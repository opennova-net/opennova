#include "simulation/player_aim_overlay.h"

using namespace godot;

void PlayerAimOverlay::set_state(int p_aim_state, int p_mount_mode, bool p_mount_config_valid,
		int p_mount_config) {
	aim_state_ = p_aim_state;
	mount_mode_ = p_mount_mode;
	mount_config_valid_ = p_mount_config_valid;
	mount_config_ = p_mount_config;
}

void PlayerAimOverlay::set_angles(const Vector3 &p_body, const PackedVector3Array &p_segments,
		const Vector3 &p_weapon_attach) {
	body_angles_ = p_body;
	segment_angles_ = p_segments;
	weapon_attach_angles_ = p_weapon_attach;
}

void PlayerAimOverlay::set_weapon(bool p_visible, bool p_hand_frame) {
	weapon_visible_ = p_visible;
	weapon_hand_frame_ = p_hand_frame;
}

void PlayerAimOverlay::_bind_methods() {
#define PLAYER_AIM_OVERLAY_FIELD(m_variant, m_name)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerAimOverlay::get_##m_name);       \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name, PROPERTY_HINT_NONE, "",                  \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),               \
			"", "get_" #m_name);
	PLAYER_AIM_OVERLAY_FIELD(Variant::INT, aim_state)
	PLAYER_AIM_OVERLAY_FIELD(Variant::INT, mount_mode)
	PLAYER_AIM_OVERLAY_FIELD(Variant::BOOL, mount_config_valid)
	PLAYER_AIM_OVERLAY_FIELD(Variant::INT, mount_config)
	PLAYER_AIM_OVERLAY_FIELD(Variant::VECTOR3, body_angles)
	PLAYER_AIM_OVERLAY_FIELD(Variant::PACKED_VECTOR3_ARRAY, segment_angles)
	PLAYER_AIM_OVERLAY_FIELD(Variant::VECTOR3, weapon_attach_angles)
	PLAYER_AIM_OVERLAY_FIELD(Variant::BOOL, weapon_visible)
	PLAYER_AIM_OVERLAY_FIELD(Variant::BOOL, weapon_hand_frame)
#undef PLAYER_AIM_OVERLAY_FIELD
}
