#ifndef NOVA_SKELETAL_ANIM_H
#define NOVA_SKELETAL_ANIM_H

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <utility>
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

	// Shared core: build bones_/bind_local_/clips_ from already-resolved .bad bytes.
	// p_reset_bytes defines the shared skeleton + bind pose; each (key, bytes) pair is sampled
	// against the shared rest origins and registered as a clip. When p_model_origins is non-empty
	// AND its size equals the bone count, it OVERRIDES the reset .bad's bone positions as the
	// shared rest origins (the .3di model's bone pivots -- see get_bone_origins); otherwise the
	// reset .bad positions are used (unchanged legacy behaviour). p_model_bind enables the
	// witnessed faithful channel semantics -- channels re-based against each .bad's own bind
	// 3x3s, identity rest rotations (the original's skin bind-inverse is the pure translation
	// T(-pivot)) -- see anim_sample.h. Caller clears state first and sets adm_name_. Returns
	// false (with last_error_) on an unusable reset .bad or when no clip survives. Shared by
	// load_from_resource_root and load_from_bad_files.
	bool build_from_bad_bytes(const PackedByteArray &p_reset_bytes,
			const std::vector<std::pair<String, PackedByteArray>> &p_clip_bads,
			const std::vector<opennova::anim::Vec3> &p_model_origins = {},
			bool p_model_bind = false);

protected:
	static void _bind_methods();

public:
	// Load + sample a model's animation set. p_adm_name is the .adm file name resolvable
	// through the resource root (the .bad clips it lists are read the same way). p_model_bone_origins
	// (optional): the .3di model's per-bone pivots (NovaObjectData.get_bone_origins), used as the
	// shared rest origins in place of the lossy .bad positions when its size matches the bone count.
	// p_model_bind (optional): faithful channel semantics (bind-relative channels + pure-translation
	// bind) -- required for the first-person viewmodel rigs, whose stored bind matrices are
	// degenerate; see build_from_bad_bytes.
	bool load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name,
			const PackedVector3Array &p_model_bone_origins = PackedVector3Array(),
			bool p_model_bind = false);

	// Load + sample a skeletal set from EXPLICIT raw .bad files (no .adm), as the original
	// PLAYER_INFO preview does: p_skeleton_bad is the rest/bind source (e.g. "Dt1rst.bad") and
	// p_key_to_bad maps each clip key (e.g. "anim_idle") to a clip .bad basename (e.g.
	// "PI_Idle.BAD"); all resolved through the resource root. The skeleton .bad provides the
	// shared bone offsets + bind pose; each clip is sampled against it. Missing clip .bads are
	// skipped (a missing skeleton .bad fails).
	// [orig: PlayerInfo_InitPreviewModel @ 0x5600d0 -> BoneFile_Load("PI_Idle.BAD"/"Dt1rst.bad")
	//  + AnimChannel_InitFromData; reimpl wraps both raw .bad files as one shared skeletal set.]
	bool load_from_bad_files(const Ref<NovaResourceRoot> &p_resource_root,
			const String &p_skeleton_bad, const Dictionary &p_key_to_bad,
			const PackedVector3Array &p_model_bone_origins = PackedVector3Array(),
			bool p_model_bind = false);

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

	// Per-bone overlay class (anim::OverlayClass) parsed from the BN## bone names, for
	// eval_pose_overlay. Accessory/unparsable bones map to the body class (the original's
	// default case). BODY rigs only: first-person weapon rigs reuse BN## tags for a
	// different 39-bone Pelvis/arms/fingers skeleton (ak47_RST.bad et al.) — never feed
	// a viewmodel through the overlay path. [orig: switch @0x4b1f3a; world-wac-ai-re §14.2]
	PackedInt32Array get_overlay_classes() const;

	// eval_pose plus the third-person aim overlay — the torso bend. p_deltas is one
	// node-frame rotation Basis per anim::OverlayClass (the host builds them from
	// NovaSimulation.get_local_player_aim_overlay() angles via the single-sourced
	// MissionObjectPlacer.bms_to_godot_basis); p_classes from get_overlay_classes().
	// Only local rotations change — origins survive the pivot re-anchor identically.
	// [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14]
	Array eval_pose_overlay(const String &p_key, double p_playhead_seconds,
			const PackedInt32Array &p_classes, const Array &p_deltas) const;

	NovaSkeletalAnim() = default;
};

} // namespace godot

#endif // NOVA_SKELETAL_ANIM_H
