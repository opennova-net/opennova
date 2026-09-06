#pragma once

#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// One authored clip: the Animation it samples (an empty name holds the rest
// pose for `hold_frames` intervals, the reset clip's shape), the .bad stem it
// exports as, the loop flag, the root motion the events carry (metres per
// second in the model's forward / lateral / vertical directions; the body is
// animated in place), the per-key trigger words, and the capsule overrides
// (negative = derived from the posed rig: the root's height and the highest
// bone's height above the ground proxy).
class ClipSpec : public Resource {
	GDCLASS(ClipSpec, Resource)

	String animation_;
	String clip_name_;
	bool loop_ = true;
	int hold_frames_ = 2;
	float forward_speed_ = 0.0f;
	float lateral_speed_ = 0.0f;
	float vertical_speed_ = 0.0f;
	PackedInt32Array triggers_;
	float capsule_bottom_ = -1.0f;
	float capsule_top_ = -1.0f;

protected:
	static void _bind_methods();

public:
	void set_animation(const String &p_value) { animation_ = p_value; }
	String get_animation() const { return animation_; }
	void set_clip_name(const String &p_value) { clip_name_ = p_value; }
	String get_clip_name() const { return clip_name_; }
	void set_loop(bool p_value) { loop_ = p_value; }
	bool get_loop() const { return loop_; }
	void set_hold_frames(int p_value) { hold_frames_ = p_value; }
	int get_hold_frames() const { return hold_frames_; }
	void set_forward_speed(float p_value) { forward_speed_ = p_value; }
	float get_forward_speed() const { return forward_speed_; }
	void set_lateral_speed(float p_value) { lateral_speed_ = p_value; }
	float get_lateral_speed() const { return lateral_speed_; }
	void set_vertical_speed(float p_value) { vertical_speed_ = p_value; }
	float get_vertical_speed() const { return vertical_speed_; }
	void set_triggers(const PackedInt32Array &p_value) { triggers_ = p_value; }
	PackedInt32Array get_triggers() const { return triggers_; }
	void set_capsule_bottom(float p_value) { capsule_bottom_ = p_value; }
	float get_capsule_bottom() const { return capsule_bottom_; }
	void set_capsule_top(float p_value) { capsule_top_ = p_value; }
	float get_capsule_top() const { return capsule_top_; }
};

// One .adm row: the anim slot key and its ring of clip names (ClipSpec stems).
class AnimSetRow : public Resource {
	GDCLASS(AnimSetRow, Resource)

	String key_;
	PackedStringArray variants_;

protected:
	static void _bind_methods();

public:
	void set_key(const String &p_value) { key_ = p_value; }
	String get_key() const { return key_; }
	void set_variants(const PackedStringArray &p_value) { variants_ = p_value; }
	PackedStringArray get_variants() const { return variants_; }
};

// The clip set an authored rig exports: the scene whose Skeleton3D and
// Animations are sampled, the clip specs, the .adm rows over their names, the
// frame rate, the ground-proxy bone the capsule heights are measured from
// (negative = the rig's last bone, ADR 0046's proxy row), and the output
// directory the .bad files and the .adm land in.
class ClipSetSource : public Resource {
	GDCLASS(ClipSetSource, Resource)

	String adm_name_;
	int fps_ = 30;
	Ref<PackedScene> scene_;
	int ground_bone_ = -1;
	TypedArray<ClipSpec> clips_;
	TypedArray<AnimSetRow> rows_;
	String output_directory_ = "res://../assets";

protected:
	static void _bind_methods();

public:
	void set_adm_name(const String &p_value) { adm_name_ = p_value; }
	String get_adm_name() const { return adm_name_; }
	void set_fps(int p_value) { fps_ = p_value; }
	int get_fps() const { return fps_; }
	void set_scene(const Ref<PackedScene> &p_value) { scene_ = p_value; }
	Ref<PackedScene> get_scene() const { return scene_; }
	void set_ground_bone(int p_value) { ground_bone_ = p_value; }
	int get_ground_bone() const { return ground_bone_; }
	void set_clips(const TypedArray<ClipSpec> &p_value) { clips_ = p_value; }
	TypedArray<ClipSpec> get_clips() const { return clips_; }
	void set_rows(const TypedArray<AnimSetRow> &p_value) { rows_ = p_value; }
	TypedArray<AnimSetRow> get_rows() const { return rows_; }
	void set_output_directory(const String &p_value) { output_directory_ = p_value; }
	String get_output_directory() const { return output_directory_; }
	Ref<ClipSpec> find_clip(const String &p_clip_name) const;
};

} // namespace godot
