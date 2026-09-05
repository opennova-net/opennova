#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// The local player's per-segment aim/body overlay for one frame: `body_angles`
// is the lagged BODY heading (BMS yaw/pitch/roll degrees — the frame the hips
// render in); `segment_angles` is one BMS euler per anim overlay class, the
// witnessed per-segment aim/body blend the avatar's skeleton applies as the
// torso twist (anim::compute_aim_overlay_angles; docs/world/world-wac-ai-re.md
// §14, D-INF-11). The THIRD-PERSON held weapon carries its own attach basis
// (anim::compute_held_weapon_attach_angles — deliberately none of the nine
// classes), retail's draw verdict (world::local_held_weapon_visible), and the
// HAND-frame selector (the weapon channel's hold state carrying flag 0x80,
// world::infantry_anim_flags). Produced by Simulation.get_local_player_aim_overlay
// (null without a local body).
class PlayerAimOverlay : public RefCounted {
	GDCLASS(PlayerAimOverlay, RefCounted)

	int aim_state_ = 0;
	int mount_mode_ = 0;
	bool mount_config_valid_ = false;
	int mount_config_ = 0;
	Vector3 body_angles_;
	PackedVector3Array segment_angles_;
	Vector3 weapon_attach_angles_;
	bool weapon_visible_ = false;
	bool weapon_hand_frame_ = false;

protected:
	static void _bind_methods();

public:
	void set_state(int p_aim_state, int p_mount_mode, bool p_mount_config_valid, int p_mount_config);
	void set_angles(const Vector3 &p_body, const PackedVector3Array &p_segments,
			const Vector3 &p_weapon_attach);
	void set_weapon(bool p_visible, bool p_hand_frame);

	int get_aim_state() const { return aim_state_; }
	// world::MountMode of the local body.
	int get_mount_mode() const { return mount_mode_; }
	// The mount target's phrase_set (presence separate: an authored zero is valid).
	bool get_mount_config_valid() const { return mount_config_valid_; }
	int get_mount_config() const { return mount_config_; }
	Vector3 get_body_angles() const { return body_angles_; }
	PackedVector3Array get_segment_angles() const { return segment_angles_; }
	Vector3 get_weapon_attach_angles() const { return weapon_attach_angles_; }
	bool get_weapon_visible() const { return weapon_visible_; }
	bool get_weapon_hand_frame() const { return weapon_hand_frame_; }
};

} // namespace godot
