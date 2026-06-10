#ifndef NOVA_SKELETAL_ANIM_H
#define NOVA_SKELETAL_ANIM_H

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <vector>

#include <anim/anim_sample.h>

namespace godot {

class NovaResourceRoot;

// Host adapter over the portable .bad/.adm skeletal runtime (libs/anim + libs/bad +
// libs/adm). Loads a model's .adm (a key -> .bad-basename map), parses + samples every
// referenced .bad clip, and exposes Godot-typed results so the GDScript host can build a
// Skeleton3D + Skin + drive per-bone poses:
//   - get_skeleton_bones(): bind-pose bones (name/parent/parent-local rest Transform3D)
//   - eval_pose(key, t):    per-bone parent-local pose Transform3D at playhead t (seconds)
//
// The portable sampler produces engine-native (Y-up) transforms -- the same space Godot
// and the original engine use -- so poses map directly to Godot Transform3Ds (no change of
// basis). The mesh carries the (-x,y,z) handedness flip; the Skin's global-rest-inverse
// bind keeps mesh and bones consistent. (Matches the proven oscarmike Godot port.)
// [orig: AnimMap_PlayAnimBySlot @0x40bda0 selects a clip; the .bad pose chain is
//  BoneAnim_TransformBones @0x410360 / build_world_bone_matrices @0x40c770.]
class NovaSkeletalAnim : public RefCounted {
	GDCLASS(NovaSkeletalAnim, RefCounted)

private:
	struct LoadedClip {
		String key;                 // ADM key, e.g. "anim_walk"
		opennova::anim::Clip clip;  // sampled (Z-up sample space)
	};

	std::vector<opennova::anim::ClipBone> bones_;  // canonical skeleton (names + parents)
	std::vector<Transform3D> bind_local_;          // per-bone parent-local rest (Godot space)
	std::vector<LoadedClip> clips_;
	String adm_name_;
	String last_error_;
	bool loaded_ = false;

	const LoadedClip *find_clip(const String &p_key) const;

protected:
	static void _bind_methods();

public:
	// Load + sample a model's animation set. p_adm_name is the .adm file name resolvable
	// through the resource root (the .bad clips it lists are read the same way).
	bool load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name);

	bool is_loaded() const { return loaded_; }
	String get_last_error() const { return last_error_; }
	String get_adm_name() const { return adm_name_; }

	int get_bone_count() const { return static_cast<int>(bones_.size()); }
	PackedStringArray get_clip_keys() const;
	// [{ name: String, parent_index: int, rest: Transform3D }], parent-local bind pose.
	Array get_skeleton_bones() const;

	// Resolve a canonical AI body-anim slot (opennova::world::BodyAnim) to a clip key in this
	// model's .adm, falling back to anim_idle / anim_reset when the slot's key is absent.
	// Empty if nothing suitable is loaded.
	String slot_to_key(int p_slot) const;

	bool has_clip(const String &p_key) const { return find_clip(p_key) != nullptr; }
	int get_clip_frame_count(const String &p_key) const;
	float get_clip_fps(const String &p_key) const;
	float get_clip_length(const String &p_key) const;  // seconds
	bool is_clip_looping(const String &p_key) const;

	// Per-bone parent-local pose at playhead t (seconds), Godot space. Size == bone count.
	// Returns the bind pose for an unknown clip / empty result.
	Array eval_pose(const String &p_key, double p_playhead_seconds) const;

	NovaSkeletalAnim() = default;
};

} // namespace godot

#endif // NOVA_SKELETAL_ANIM_H
