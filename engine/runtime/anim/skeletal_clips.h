// The engine-side skeletal clip set for one rig: the .adm's clip ring sampled
// against the ONE shared skeleton (the reset/bind .bad + the model's bone
// table), with the bind-local rests, accumulated rest globals, overlay
// classes, and the weapon-channel mask precomputed. One evaluator serves both
// world collision poses and Godot skeletons (ADR 0044).
//
// [orig: the rig-wide skeleton is the .adm slot-0 .bad, pinned once at entity
//  registration — AnimMap_RegisterEntity @0x40bb60; it is also the bind whose
//  flags & 2 gates every clip's translations, AnimChannel_ComputeBoneMatrices
//  @0x410da0 @0x410de7; clip switches never
//  rebuild it, AnimMap_PlayAnimBySlot @0x40bda0; clip lookup is
//  AnimMap_FindSlotByName @0x40cfa0 (stricmp); every quoted token on a row is
//  a VARIANT of the same slot in file order — AnimMap_ParseConfigLine
//  @0x40cb60 / AnimMap_RegisterBoneNode @0x40c2d0.]
#pragma once

#include <runtime/anim/anim_sample.h>
#include <runtime/anim/rig_files.h>
#include <runtime/anim/skeletal_pose.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::anim {

class SkeletalClips {
public:
	// A bone's precomputed rest as rotation rows (row-major, column-vector
	// convention) + origin — bind-local and the accumulated model-space
	// global the deformation divides by.
	struct RestTransform {
		float rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		anim::Vec3 origin;
	};

	// Load the whole rig through `files` (the mounted store, or an embedder's own): bind it to the
	// table's reset clip, retail's slot-0 head (the last variant that loads of
	// the last row whose key past its first five characters is "reset", any
	// case; a table with none does not load), sample every clip variant of a
	// row naming one of the 252 slots in file order (a row naming none
	// registers nothing; a .bad that does not load registers failsafe.bad in its
	// place, else nothing: adm_token_clip) against that bind, and build
	// the skeleton from the model bone table (origins = parent-relative pivots,
	// parents paired) with one loader for simulation and presentation. Returns
	// loaded().
	bool load_from_adm(const RigFiles *files, const std::string &adm_name,
	                   const std::vector<anim::Vec3> &model_bone_origins,
	                   const std::vector<int> &model_bone_parents);
	bool load_from_files(const RigFiles *files, const std::string &skeleton_bad,
						 const std::vector<std::pair<std::string, std::string>> &clips,
						 const std::vector<anim::Vec3> &model_bone_origins = {},
						 const std::vector<int> &model_bone_parents = {});
	void clear();

	bool loaded() const { return loaded_; }
	const std::string &adm_name() const { return adm_name_; }
	size_t bone_count() const { return bones_.size(); }
	const std::vector<anim::ClipBone> &bones() const { return bones_; }
	const std::vector<int> &parents() const { return parents_; }
	const std::vector<uint8_t> &overlay_classes() const { return classes_; }
	// FK-safe: every parent index is -1 or a lower bone index, so the
	// accumulated rest globals (and their inverses) are valid. The collision
	// consumer declines skeletal posing on a rig that is not.
	bool fk_valid() const { return fk_valid_; }
	const std::vector<RestTransform> &bind_local() const { return bind_local_; }
	const std::vector<RestTransform> &rest_global() const { return rest_global_; }
	const std::vector<RestTransform> &rest_global_inverse() const {
		return rest_global_inverse_;
	}

	bool has_clip(const std::string &key) const;
	// Resolve the canonical AI body slot, then this rig's idle/reset fallbacks.
	std::string slot_to_key(int slot) const;
	int clip_variant_count(const std::string &key) const;
	std::vector<float> clip_variant_lengths(const std::string &key) const;
	float clip_length(const std::string &key, int variant = 0) const;
	float clip_fps(const std::string &key, int variant = 0) const;
	// `armed_boundary` is the tick an armed end-notify parks a loop on (the
	// channel's boundary when its *_phase_parked() holds), -1 unarmed: that tick
	// samples the parked last frame [orig: AnimChannel_AdvancePlayback
	// @0x40B1A2..0x40B1B1].
	double clip_seconds_at_tick(const std::string &key, int32_t ticks, int variant = 0,
	                            int32_t armed_boundary = -1) const;

	// The raw stage evaluations, public for diagnostics/tests: a plain clip
	// sample (bind-pose fallback on unknown/empty keys) and the retail-window
	// blend the composed pose builds on.
	void eval_pose(const std::string &key, double playhead_seconds, int variant,
	               std::vector<anim::PoseBone> &r_pose) const;
	// source_variant / target_variant select each channel's served ring entry
	// (0-based file order, wrapped modulo the count — the +68 play latch).
	void eval_pose_blended(const std::string &source_key, double source_seconds,
	                       const std::string &target_key, double target_seconds,
	                       float weight, std::vector<anim::PoseBone> &r_pose,
	                       int source_variant = 0, int target_variant = 0) const;

	// The composed witnessed pose: primary sample (optionally blended from the
	// source key over the retail window) -> weapon-channel mask override ->
	// aim overlay on top. deltas are the nine node-frame class rotations.
	// Unknown keys fall back to anim_reset (blend legs) or the bind pose
	// (primary sample), mirroring the binding evaluator exactly. The BN17
	// collision collapse stays with the consumer (it zeroes the matrix row).
	// [orig: @0x4b14a7..@0x4b16a7 run before the per-bone overlay loop;
	//  world-wac-ai-re.md §14.8.6]
	bool eval_composed_pose(const std::string &primary_key,
	                        double primary_seconds, bool blended,
	                        const std::string &source_key, double source_seconds,
	                        float blend_weight,
	                        const anim::Quat deltas[],
	                        const std::string &weapon_key, double weapon_seconds,
	                        std::vector<anim::PoseBone> &r_pose,
	                        const std::string &weapon_prev_key = std::string(),
	                        double weapon_prev_seconds = 0.0,
	                        float weapon_blend_weight = 1.0f,
	                        int weapon_variant = 0, int weapon_prev_variant = 0,
	                        int primary_variant = 0, int source_variant = 0) const;

	// Where a loaded clip comes from: the table's row (its index among the .adm's
	// entries, file order), the token of that row (its index among the row's clips)
	// and the file that token names. load_from_files numbers its pairs as rows of
	// one token each.
	struct ClipSource {
		size_t entry = 0;
		size_t token = 0;
		std::string file;
	};

	// The loaded clip records, public for diagnostics/tests (frame counts,
	// loop flags): the composed-pose path above is the consumer seam.
	struct LoadedClip {
		std::string key; // as authored (lookups fold)
		ClipSource source;
		anim::Clip clip;
	};

	const std::vector<LoadedClip> &clips() const { return clips_; }
	const LoadedClip *find_clip(const std::string &key) const;
	const LoadedClip *find_clip_variant(const std::string &key, int variant) const;
	// The source of the clip a key's variant serves (wrapped as find_clip_variant
	// wraps); null when the key has none.
	const ClipSource *find_clip_source(const std::string &key, int variant) const;
	// The other way: the key and variant a row's token registered as; -1 (key
	// untouched) when it registered nothing, its key naming no slot or its file
	// not loading.
	int variant_of(size_t entry, size_t token, std::string &key) const;

	// Compose the channels over an already sampled pose: the weapon-channel
	// splice, then the aim overlay on top, in the witnessed order
	// [orig: @0x4b14a7..@0x4b16a7 run before the per-bone overlay loop].
	// Presentation passes its typed overlay-class rows (the SkeletalAnim
	// binding's get_overlay_classes); authoritative posing passes this rig's
	// own overlay_classes(). Null deltas or a class row shorter than the pose
	// skip the overlay; the weapon channel still splices.
	void apply_pose_overlay(std::vector<anim::PoseBone> &pose,
			const anim::Quat *deltas, const std::vector<uint8_t> &classes,
			const std::string &weapon_key, double weapon_seconds,
			const std::string &weapon_prev_key = std::string(),
			double weapon_prev_seconds = 0.0, float weapon_weight = 1.0f,
			int weapon_variant = 0, int weapon_prev_variant = 0) const;

private:
	struct ClipRequest {
		std::string key;
		ClipSource source;
	};
	// `table_tokens`: the requests are a table's tokens, which take failsafe.bad in
	// place of a file that does not load (adm_token_clip).
	bool load_clips(const RigFiles *files, const std::string &skeleton_bad,
	                const std::vector<ClipRequest> &requests, bool table_tokens,
	                const std::vector<anim::Vec3> &model_bone_origins,
	                const std::vector<int> &model_bone_parents);
	void rebuild_clip_index();

	// The upper-body WEAPON channel: sample weapon_key at ITS OWN playhead and hard-override
	// the mask bones' WORLD rotations (clavicles/arms/forearms/neck/head/hands — the
	// anim::kWeaponChannelMaskBones set by BN## index), then re-localize the complete
	// mixed hierarchy. Origins keep the primary pose's (the shared skeleton owns the
	// pivots). [orig: the mask override in Entity_BuildBoneTransformMatrices
	// @0x4b14db/@0x4b16a7; world-wac-ai-re.md §14.8.6]
	//
	// An EMPTY weapon_key means the §14.8.6 gate is off — no override at all. A key with
	// no matching clip is DIFFERENT: registration backfills every absent anim_<name>
	// with entry 0, so a missing key plays the RESET clip rather than no-opping
	// [orig: the unrolled backfill loops @0x40bc24 / @0x40bd2e, see
	// docs/world/world-wac-ai-re.md §14.8.1].
	//
	// weapon_prev_key/playhead/weight carry the secondary channel's own cross-fade — the
	// weapon layer re-inits through the SAME AnimMap_UpdateEntity body as the primary,
	// so it takes the same blend window [orig: AnimMap_UpdateDualChannels @0x40b8c0
	// -> @0x40b5f0 and AnimChannel_BlendTwoChannels @0x410740, see
	// docs/world/world-wac-ai-re.md §14.8.7]. Empty prev = no blend.
	void splice_weapon_channel(std::vector<anim::PoseBone> &pose,
	                           const std::string &weapon_key,
	                           double weapon_seconds,
	                           const std::string &weapon_prev_key = std::string(),
	                           double weapon_prev_seconds = 0.0,
	                           float weapon_blend_weight = 1.0f,
	                           int weapon_variant = 0,
	                           int weapon_prev_variant = 0) const;

	bool loaded_ = false;
	bool fk_valid_ = false;
	std::string adm_name_;
	std::vector<anim::ClipBone> bones_;
	std::vector<int> parents_;
	std::vector<uint8_t> classes_;      // aim-overlay class per bone (BN## tag)
	std::vector<uint8_t> weapon_mask_;  // weapon-channel mask per bone (BN## tag)
	bool any_weapon_mask_ = false;
	std::vector<anim::PoseBone> bind_pose_;       // bind-local rotation+origin
	std::vector<RestTransform> bind_local_;
	std::vector<RestTransform> rest_global_;
	std::vector<RestTransform> rest_global_inverse_;
	std::vector<LoadedClip> clips_;
	// Case-folded key -> clips_ indices in .adm file order (the variant ring).
	std::unordered_map<std::string, std::vector<size_t>> clip_index_;
};

// Affine compose (b applied first) and full affine inverse over the
// rows+origin pair — shared by the rest accumulation above and the collision
// consumer's pose FK/deformation.
SkeletalClips::RestTransform rest_mul(
		const SkeletalClips::RestTransform &a,
		const SkeletalClips::RestTransform &b);
SkeletalClips::RestTransform rest_affine_inverse(
		const SkeletalClips::RestTransform &t);

} // namespace opennova::anim
