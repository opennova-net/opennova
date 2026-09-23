#pragma once

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

#include <string>
#include <utility>
#include <vector>

#include <runtime/anim/anim_sample.h>
#include <runtime/assets/asset_store.h>
#include <runtime/anim/skeletal_clips.h>

namespace godot {

class ResourceRoot;
class Skeleton3D;

// Godot adapter over a shared immutable native rig (runtime/assets + runtime/anim).
// Marshals model bone tables and native poses into Godot values so consumers
// can build a Skeleton3D + Skin and drive per-bone poses:
//   - get_skeleton_bones(): bind-pose bones (name/parent/parent-local rest Transform3D)
//   - eval_pose(key, t):    per-bone parent-local pose Transform3D at playhead t (seconds)
//
// The portable sampler produces engine-native (Y-up) transforms -- the same space Godot
// and the original engine use -- so poses map directly to Godot Transform3Ds (no change of
// basis). The mesh carries the (-x,y,z) handedness flip; the Skin's global-rest-inverse
// bind keeps mesh and bones consistent. (Matches the proven oscarmike Godot port.)
// [orig: AnimMap_PlayAnimBySlot @0x40bda0 selects a clip]; the .bad pose math
// (frame-window walk, blend, bind rest) is anim/skeletal_pose.h's.
class SkeletalAnim : public RefCounted {
	GDCLASS(SkeletalAnim, RefCounted)

private:
	using LoadedClip = opennova::anim::SkeletalClips::LoadedClip;
	opennova::assets::SkeletalRig rig_;
	String last_error_;
	const opennova::anim::SkeletalClips &rig() const;
	const LoadedClip *find_clip(const String &key) const;
	const LoadedClip *find_clip_variant(const String &key, int variant) const;
	Array apply_pose_overlay(std::vector<opennova::anim::PoseBone> p_pose,
			const PackedInt32Array &p_classes, const Basis *p_deltas,
			const String &p_wpn_key, double p_wpn_playhead_seconds,
			bool p_collapse_right_hand, const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0) const;
	void write_pose_to_skeleton(Skeleton3D *p_skeleton, const Array &p_pose,
			bool p_collapse_right_hand) const;

protected:
	static void _bind_methods();

public:
	// Load + sample a model's animation set. p_adm_name is the .adm file name resolvable
	// through the resource root (the .bad clips it lists are read the same way).
	// p_model_bone_origins + p_model_bone_parents (optional, paired): the .3di model's bone
	// table (ObjectData.get_bone_origins / get_bone_parents) -- when both are supplied
	// with equal sizes, the MODEL defines the rig (count, hierarchy, pivots-by-reconstruction)
	// and the .bad contributes rotations only, exactly as the original consumes
	// modelDef+52/+56; origins alone are the legacy positional override (see
	// anim::SkeletalClips).
	bool load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_adm_name,
			const PackedVector3Array &p_model_bone_origins = PackedVector3Array(),
			const PackedInt32Array &p_model_bone_parents = PackedInt32Array());

	// Load + sample a skeletal set from EXPLICIT raw .bad files (no .adm), as the original
	// PLAYER_INFO preview does: p_skeleton_bad is the rest/bind source (e.g. "Dt1rst.bad") and
	// p_key_to_bad maps each clip key (e.g. "anim_idle") to a clip .bad basename (e.g.
	// "PI_Idle.BAD"); all resolved through the resource root. The skeleton .bad provides the
	// shared bone offsets + bind pose; each clip is sampled against it. Missing clip .bads are
	// skipped (a missing skeleton .bad fails).
	// [orig: PlayerInfo_InitPreviewModel @ 0x5600d0 -> BoneFile_Load("PI_Idle.BAD"/"Dt1rst.bad")
	//  + AnimChannel_InitFromData; reimpl wraps both raw .bad files as one shared skeletal set.]
	bool load_from_bad_files(const Ref<ResourceRoot> &p_resource_root,
			const String &p_skeleton_bad, const Dictionary &p_key_to_bad,
			const PackedVector3Array &p_model_bone_origins = PackedVector3Array(),
			const PackedInt32Array &p_model_bone_parents = PackedInt32Array());

	bool is_loaded() const { return rig_ != nullptr; }
	String get_last_error() const { return last_error_; }

	int get_bone_count() const { return static_cast<int>(rig().bone_count()); }
	PackedStringArray get_clip_keys() const;
	// [{ name: String, parent_index: int, rest: Transform3D }], parent-local bind pose.
	Array get_skeleton_bones() const;

	// Resolve a canonical AI body-anim slot (opennova::world::BodyAnim) to a clip key in this
	// model's .adm, falling back to anim_idle / anim_reset when the slot's key is absent.
	// Empty if nothing suitable is loaded.
	String slot_to_key(int p_slot) const;

	bool has_clip(const String &p_key) const { return find_clip(p_key) != nullptr; }
	// Variant surface: p_variant selects among same-key clips (0-based file order,
	// wrapped). All getters/eval are non-consuming PEEKS — the consuming ring
	// rotation (serve-then-advance on duration reads and play starts [orig:
	// Anim_GetDurationTicks @ 0x53ee10 / AnimMap_PlayAnimBySlot @ 0x40bda0]) is
	// driven by the FSM owner, which passes the served index back in.
	int get_clip_variant_count(const String &p_key) const;
	// Every variant's length (seconds) in ring order — the FSM owner's ring data.
	PackedFloat32Array get_clip_variant_lengths(const String &p_key) const;
	int get_clip_frame_count(const String &p_key, int p_variant = 0) const;
	float get_clip_fps(const String &p_key, int p_variant = 0) const;
	double get_clip_phase_seconds(const String &p_key, int p_ticks, int p_variant = 0) const;
	float get_clip_length(const String &p_key, int p_variant = 0) const;  // seconds
	bool is_clip_looping(const String &p_key, int p_variant = 0) const;

	// Per-bone parent-local pose at playhead t (seconds), Godot space. Size == bone count.
	// Returns the bind pose for an unknown clip / empty result.
	Array eval_pose(const String &p_key, double p_playhead_seconds, int p_variant = 0) const;
	// Blend two PRIMARY channels before any weapon-mask or aim-overlay
	// composition. Missing semantic channels use anim_reset when the ADM carries
	// it; without RESET, a valid remaining channel is retained so an unresolved
	// key never leaves a stale Skeleton3D pose resident.
	// p_source_variant / p_target_variant select each channel's served ring entry
	// (0-based file order, wrapped modulo the count — the +68 play latch).
	Array eval_pose_blended(const String &p_source_key,
			double p_source_playhead_seconds, const String &p_target_key,
			double p_target_playhead_seconds, float p_weight,
			int p_source_variant = 0, int p_target_variant = 0) const;

	// Per-bone overlay class (anim::OverlayClass) parsed from the BN## bone names, for
	// eval_pose_overlay. Accessory/unparsable bones map to the body class (the original's
	// default case). BODY rigs only: first-person weapon rigs reuse BN## tags for a
	// different 39-bone Pelvis/arms/fingers skeleton (ak47_RST.bad et al.) — never feed
	// a viewmodel through the overlay path. [orig: switch @0x4b1f3a; world-wac-ai-re §14.2]
	PackedInt32Array get_overlay_classes() const;

	// eval_pose plus the third-person aim overlay — the torso bend. p_deltas is one
	// node-frame rotation Basis per anim::OverlayClass (the owner builds them from
	// Simulation.get_local_player_aim_overlay() angles via the single-sourced
	// MissionObjectPlacer.bms_to_godot_basis); p_classes from get_overlay_classes().
	// Only local rotations change — origins survive the pivot re-anchor identically.
	// p_wpn_key (optional): the weapon channel's clip, spliced onto the mask bones at
	// p_wpn_playhead BEFORE the overlay composes — the witnessed order (primary sample →
	// mask override → overlay multiply). Empty = single channel.
	// collapse_right_hand emits a terminal zero-scale BN17 pose at its sampled
	// local joint even when overlay inputs are unavailable and this falls back to
	// the sampled pose. Collision adapts that verdict to its final-row convention.
	// [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14/§14.8.6]
	// p_variant / p_source_variant / p_target_variant: the primary channels'
	// served ring entries.
	Array eval_pose_overlay_deltas(const String &p_key, double p_playhead_seconds,
			const PackedInt32Array &p_classes, const Basis *p_deltas,
			const String &p_wpn_key = String(), double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false,
			const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0, int p_variant = 0) const;
	Array eval_pose_blended_overlay_deltas(const String &p_source_key,
			double p_source_playhead_seconds, const String &p_target_key,
			double p_target_playhead_seconds, float p_weight,
			const PackedInt32Array &p_classes, const Basis *p_deltas,
			const String &p_wpn_key = String(),
			double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false,
			const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0, int p_source_variant = 0,
			int p_target_variant = 0) const;

	// The whole per-frame body-pose write in one call: evaluate the pose
	// (eval_pose_overlay when classes+deltas are non-empty, eval_pose otherwise)
	// and write every bone's position/rotation/scale onto p_skeleton, including
	// the BN17 zero-scale collapse branch. Exactly the loop ObjectModel ran
	// in GDScript — moved native because it executes per animated model per
	// render frame (bone-count boxed Transform3Ds + 3 cross-boundary calls per
	// bone from script dominated the present pass).
	void pose_skeleton_deltas(Skeleton3D *p_skeleton, const String &p_key,
			double p_playhead_seconds, int p_variant,
			const PackedInt32Array &p_classes, const Basis *p_deltas,
			const String &p_wpn_key = String(), double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false,
			const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0) const;
	// The GDScript-facing forms of the three *_deltas entry points above: the aim
	// overlay arrives as an Array of up to 9 Basis (index = overlay class, a missing
	// or non-Basis entry reads identity; an empty Array means no overlay). The
	// present applier calls the Basis-pointer forms directly.
	Array eval_pose_overlay(const String &p_key, double p_playhead_seconds,
			const PackedInt32Array &p_classes, const Array &p_deltas,
			const String &p_wpn_key = String(), double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false,
			const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0) const;
	Array eval_pose_blended_overlay(const String &p_source_key,
			double p_source_playhead_seconds, const String &p_target_key,
			double p_target_playhead_seconds, float p_weight,
			const PackedInt32Array &p_classes, const Array &p_deltas,
			const String &p_wpn_key = String(),
			double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false,
			const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0) const;
	void pose_skeleton(Skeleton3D *p_skeleton, const String &p_key,
			double p_playhead_seconds, int p_variant,
			const PackedInt32Array &p_classes, const Array &p_deltas,
			const String &p_wpn_key = String(), double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false,
			const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0) const;
	void pose_skeleton_blended(Skeleton3D *p_skeleton,
			const String &p_source_key, double p_source_playhead_seconds,
			const String &p_target_key, double p_target_playhead_seconds,
			float p_weight, const PackedInt32Array &p_classes,
			const Basis *p_deltas, const String &p_wpn_key = String(),
			double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false,
			const String &p_wpn_prev_key = String(),
			double p_wpn_prev_playhead_seconds = 0.0,
			float p_wpn_weight = 1.0f, int p_wpn_variant = 0,
			int p_wpn_prev_variant = 0, int p_source_variant = 0,
			int p_target_variant = 0) const;

	SkeletalAnim() = default;
};

} // namespace godot

