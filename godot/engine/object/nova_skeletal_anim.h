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

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <anim/anim_sample.h>

namespace godot {

class NovaResourceRoot;
class Skeleton3D;

// Godot adapter over the portable .bad/.adm skeletal runtime (libs/anim + libs/bad +
// libs/adm). Loads a model's .adm (a key -> .bad-basename map), parses + samples every
// referenced .bad clip, and exposes Godot-typed results so the GDScript owner can build a
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
	// A multi-clip .adm row registers one clip PER quoted token under the same key,
	// kept in file order — the slot's variant ring in the original; consecutive
	// same-key entries here [orig: AnimMap_ParseConfigLine @ 0x40cb60 registers every
	// token; AnimMap_RegisterBoneNode @ 0x40c2d0 links them into a circular ring].
	// The ring CURSOR is not here: rotation state lives with the weapon/entity FSM
	// that consumes it (NovaSimulation), exactly as the original keeps the heads on
	// the per-entity animState (+72) — this class is the loaded clip data only, so
	// the two viewmodel parts sharing one .adm stay in lockstep.
	std::vector<LoadedClip> clips_;
	// Case-folded key -> clips_ indices in insertion (= .adm file / ring) order.
	// The present pass resolves clips per animated model per render frame
	// (has_clip / get_clip_fps / get_clip_length / eval), so lookups must not
	// linear-scan clips_ with per-entry case-insensitive compares.
	std::unordered_map<std::string, std::vector<size_t>> clip_index_;
	String adm_name_;
	String last_error_;
	bool loaded_ = false;

	// ASCII case fold matching nocasecmp_to over this format's key alphabet
	// (.adm/.def keys are ASCII; non-ASCII bytes pass through unfolded).
	static std::string fold_clip_key(const String &p_key);
	void rebuild_clip_index();

	const LoadedClip *find_clip(const String &p_key) const;
	// The p_variant-th same-key clip (file order, wrapped modulo the variant count —
	// robust when a variant's .bad failed to load on one part). p_variant <= 0 or a
	// single-clip key serve the first match.
	const LoadedClip *find_clip_variant(const String &p_key, int p_variant) const;
	Array apply_pose_overlay(Array p_pose,
			const PackedInt32Array &p_classes, const Array &p_deltas,
			const String &p_wpn_key, double p_wpn_playhead_seconds,
			bool p_collapse_right_hand) const;
	void write_pose_to_skeleton(Skeleton3D *p_skeleton, const Array &p_pose,
			bool p_collapse_right_hand) const;

	// Shared core: build bones_/bind_local_/clips_ from already-resolved .bad bytes.
	// p_reset_bytes defines the shared skeleton + bind pose; each (key, bytes) pair is sampled
	// against the shared rest origins and registered as a clip. When p_model_parents pairs with
	// p_model_origins (same non-zero size), the MODEL's bone table defines the rig outright --
	// row count, hierarchy, and pivots; the .bad contributes rotations only, by row index, and
	// its bone count/parents/positions are never read [orig: BoneAnim_BuildWorldMatrices
	// @0x40c400 bounds the FK by modelDef+52 and reads parent/pivot from the modelDef+56 rows;
	// extra rows past the .bad's channels take bone 0's composed matrix, the padding loop
	// @0x40c5a1]. The skeleton's rest POSITIONS are RECONSTRUCTED from the model table + the
	// reset .bad's bind rotations via the corpus-exact export relation
	// (anim_sample.h positions_from_model), and the clips run the SAME rest-carrying
	// composition the body pipeline uses -- so a rig whose shipped BadBone.position is
	// zeroed/stale renders exactly like a healthy one, and a healthy one renders bit-for-bit
	// like the .bad-driven path (the proven-equivalent factorization of the witnessed composed
	// builders; sample_clip's model_bind mode remains the direct reference implementation,
	// exercised by ctest). Legacy (no parents): p_model_origins sized like the .bad's bone
	// count OVERRIDES the reset .bad's bone positions as the shared rest origins; otherwise
	// the reset .bad positions are used (menu-preview semantics). Caller clears state first
	// and sets adm_name_. Returns false (with last_error_) on an unusable reset .bad or when
	// no clip survives. Shared by load_from_resource_root and load_from_bad_files.
	bool build_from_bad_bytes(const PackedByteArray &p_reset_bytes,
			const std::vector<std::pair<String, PackedByteArray>> &p_clip_bads,
			const std::vector<opennova::anim::Vec3> &p_model_origins = {},
			const std::vector<int> &p_model_parents = {});

protected:
	static void _bind_methods();

public:
	// Load + sample a model's animation set. p_adm_name is the .adm file name resolvable
	// through the resource root (the .bad clips it lists are read the same way).
	// p_model_bone_origins + p_model_bone_parents (optional, paired): the .3di model's bone
	// table (NovaObjectData.get_bone_origins / get_bone_parents) -- when both are supplied
	// with equal sizes, the MODEL defines the rig (count, hierarchy, pivots-by-reconstruction)
	// and the .bad contributes rotations only, exactly as the original consumes
	// modelDef+52/+56; origins alone are the legacy positional override (see
	// build_from_bad_bytes).
	bool load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name,
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
	bool load_from_bad_files(const Ref<NovaResourceRoot> &p_resource_root,
			const String &p_skeleton_bad, const Dictionary &p_key_to_bad,
			const PackedVector3Array &p_model_bone_origins = PackedVector3Array(),
			const PackedInt32Array &p_model_bone_parents = PackedInt32Array());

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
	float get_clip_length(const String &p_key, int p_variant = 0) const;  // seconds
	bool is_clip_looping(const String &p_key, int p_variant = 0) const;

	// Per-bone parent-local pose at playhead t (seconds), Godot space. Size == bone count.
	// Returns the bind pose for an unknown clip / empty result.
	Array eval_pose(const String &p_key, double p_playhead_seconds, int p_variant = 0) const;
	// Blend two PRIMARY channels before any weapon-mask or aim-overlay
	// composition. Missing semantic channels use anim_reset when the ADM carries
	// it; without RESET, a valid remaining channel is retained so an unresolved
	// key never leaves a stale Skeleton3D pose resident.
	Array eval_pose_blended(const String &p_source_key,
			double p_source_playhead_seconds, const String &p_target_key,
			double p_target_playhead_seconds, float p_weight) const;

	// The upper-body WEAPON channel: sample p_wpn_key at ITS OWN playhead and hard-override
	// the mask bones' WORLD rotations (clavicles/arms/forearms/neck/head/hands — the
	// anim::kWeaponChannelMaskBones set by BN## index), then re-localize the complete
	// mixed hierarchy. Origins keep the primary pose's (the shared skeleton owns the
	// pivots). No-op when the key is unknown or sizes mismatch. [orig: the mask override in
	// Entity_BuildBoneTransformMatrices @0x4b14db/@0x4b16a7; world-wac-ai-re.md §14.8.6]
	void splice_weapon_channel(Array &p_pose, const String &p_wpn_key,
			double p_wpn_playhead_seconds) const;

	// Per-bone overlay class (anim::OverlayClass) parsed from the BN## bone names, for
	// eval_pose_overlay. Accessory/unparsable bones map to the body class (the original's
	// default case). BODY rigs only: first-person weapon rigs reuse BN## tags for a
	// different 39-bone Pelvis/arms/fingers skeleton (ak47_RST.bad et al.) — never feed
	// a viewmodel through the overlay path. [orig: switch @0x4b1f3a; world-wac-ai-re §14.2]
	PackedInt32Array get_overlay_classes() const;

	// eval_pose plus the third-person aim overlay — the torso bend. p_deltas is one
	// node-frame rotation Basis per anim::OverlayClass (the owner builds them from
	// NovaSimulation.get_local_player_aim_overlay() angles via the single-sourced
	// MissionObjectPlacer.bms_to_godot_basis); p_classes from get_overlay_classes().
	// Only local rotations change — origins survive the pivot re-anchor identically.
	// p_wpn_key (optional): the weapon channel's clip, spliced onto the mask bones at
	// p_wpn_playhead BEFORE the overlay composes — the witnessed order (primary sample →
	// mask override → overlay multiply). Empty = single channel.
	// collapse_right_hand emits a terminal zero-scale BN17 pose at its sampled
	// local joint even when overlay inputs are unavailable and this falls back to
	// the sampled pose. Collision adapts that verdict to its final-row convention.
	// [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14/§14.8.6]
	Array eval_pose_overlay(const String &p_key, double p_playhead_seconds,
			const PackedInt32Array &p_classes, const Array &p_deltas,
			const String &p_wpn_key = String(), double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false) const;
	Array eval_pose_blended_overlay(const String &p_source_key,
			double p_source_playhead_seconds, const String &p_target_key,
			double p_target_playhead_seconds, float p_weight,
			const PackedInt32Array &p_classes, const Array &p_deltas,
			const String &p_wpn_key = String(),
			double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false) const;

	// The whole per-frame body-pose write in one call: evaluate the pose
	// (eval_pose_overlay when classes+deltas are non-empty, eval_pose otherwise)
	// and write every bone's position/rotation/scale onto p_skeleton, including
	// the BN17 zero-scale collapse branch. Exactly the loop NovaObjectModel ran
	// in GDScript — moved native because it executes per animated model per
	// render frame (bone-count boxed Transform3Ds + 3 cross-boundary calls per
	// bone from script dominated the present pass).
	void pose_skeleton(Skeleton3D *p_skeleton, const String &p_key,
			double p_playhead_seconds, int p_variant,
			const PackedInt32Array &p_classes, const Array &p_deltas,
			const String &p_wpn_key = String(), double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false) const;
	void pose_skeleton_blended(Skeleton3D *p_skeleton,
			const String &p_source_key, double p_source_playhead_seconds,
			const String &p_target_key, double p_target_playhead_seconds,
			float p_weight, const PackedInt32Array &p_classes,
			const Array &p_deltas, const String &p_wpn_key = String(),
			double p_wpn_playhead_seconds = 0.0,
			bool p_collapse_right_hand = false) const;

	NovaSkeletalAnim() = default;
};

} // namespace godot

#endif // NOVA_SKELETAL_ANIM_H
