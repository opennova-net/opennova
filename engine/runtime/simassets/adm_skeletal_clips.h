// The engine-side skeletal clip set for one rig: the .adm's clip ring sampled
// against the ONE shared skeleton (the reset/bind .bad + the model's bone
// table), with the bind-local rests, accumulated rest globals, overlay
// classes, and the weapon-channel mask precomputed. The native counterpart of
// the shell adapter's skeletal evaluator, feeding the sim collision pose
// provider (ADR 0028).
//
// [orig: the rig-wide skeleton is the .adm slot-0 .bad, pinned once at entity
//  registration — AnimMap_RegisterEntity @0x40bb60; clip switches never
//  rebuild it, AnimMap_PlayAnimBySlot @0x40bda0; clip lookup is
//  AnimMap_FindSlotByName @0x40cfa0 (stricmp); every quoted token on a row is
//  a VARIANT of the same slot in file order — AnimMap_ParseConfigLine
//  @0x40cb60 / AnimMap_RegisterBoneNode @0x40c2d0.]
#ifndef OPENNOVA_SIMASSETS_ADM_SKELETAL_CLIPS_H
#define OPENNOVA_SIMASSETS_ADM_SKELETAL_CLIPS_H

#include <runtime/anim/anim_sample.h>
#include <runtime/anim/skeletal_pose.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace opennova {
class ResourceIndex;
}

namespace opennova::simassets {

class AdmSkeletalClips {
public:
	// A bone's precomputed rest as rotation rows (row-major, column-vector
	// convention) + origin — bind-local and the accumulated model-space
	// global the deformation divides by.
	struct RestTransform {
		float rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		anim::Vec3 origin;
	};

	// Load the whole rig through the mounted resource index: resolve the
	// reset/skeleton .bad (a key containing "reset" wins, else the first
	// non-empty value), sample every clip variant in file order
	// (continue-on-failure on missing .bads), and build the skeleton from the
	// model bone table (origins = parent-relative pivots, parents paired)
	// exactly like the adapter's loader. Returns loaded().
	bool load_from_adm(const ResourceIndex *index, const std::string &adm_name,
	                   const std::vector<anim::Vec3> &model_bone_origins,
	                   const std::vector<int> &model_bone_parents);
	void clear();

	bool loaded() const { return loaded_; }
	const std::string &adm_name() const { return adm_name_; }
	size_t bone_count() const { return bones_.size(); }
	const std::vector<anim::ClipBone> &bones() const { return bones_; }
	const std::vector<anim::PoseBone> &bind_pose() const { return bind_pose_; }
	const std::vector<int> &parents() const { return parents_; }
	const std::vector<uint8_t> &overlay_classes() const { return classes_; }
	// FK-safe: every parent index is -1 or a lower bone index, so the
	// accumulated rest globals (and their inverses) are valid. The collision
	// consumer declines skeletal posing on a rig that is not.
	bool fk_valid() const { return fk_valid_; }
	const std::vector<RestTransform> &rest_global() const { return rest_global_; }
	const std::vector<RestTransform> &rest_global_inverse() const {
		return rest_global_inverse_;
	}

	bool has_clip(const std::string &key) const;
	float clip_fps(const std::string &key, int variant = 0) const;

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
	// (primary sample), mirroring the adapter evaluator exactly. The BN17
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

	// The loaded clip records, public for diagnostics/tests (frame counts,
	// loop flags): the composed-pose path above is the consumer seam.
	struct LoadedClip {
		std::string key; // as authored (lookups fold)
		anim::Clip clip;
	};

	const LoadedClip *find_clip(const std::string &key) const;
	const LoadedClip *find_clip_variant(const std::string &key, int variant) const;

private:
	void splice_weapon_channel(std::vector<anim::PoseBone> &pose,
	                           const std::string &weapon_key,
	                           double weapon_seconds,
	                           const std::string &weapon_prev_key = std::string(),
	                           double weapon_prev_seconds = 0.0,
	                           float weapon_blend_weight = 1.0f,
	                           int weapon_variant = 0,
	                           int weapon_prev_variant = 0) const;
	void rebuild_clip_index();

	bool loaded_ = false;
	bool fk_valid_ = false;
	std::string adm_name_;
	std::vector<anim::ClipBone> bones_;
	std::vector<int> parents_;
	std::vector<uint8_t> classes_;      // aim-overlay class per bone (BN## tag)
	std::vector<uint8_t> weapon_mask_;  // weapon-channel mask per bone (BN## tag)
	bool any_weapon_mask_ = false;
	std::vector<anim::PoseBone> bind_pose_;       // bind-local rotation+origin
	std::vector<RestTransform> rest_global_;
	std::vector<RestTransform> rest_global_inverse_;
	std::vector<LoadedClip> clips_;
	// Case-folded key -> clips_ indices in .adm file order (the variant ring).
	std::unordered_map<std::string, std::vector<size_t>> clip_index_;
};

// Affine compose (b applied first) and full affine inverse over the
// rows+origin pair — shared by the rest accumulation above and the collision
// consumer's pose FK/deformation.
AdmSkeletalClips::RestTransform rest_mul(
		const AdmSkeletalClips::RestTransform &a,
		const AdmSkeletalClips::RestTransform &b);
AdmSkeletalClips::RestTransform rest_affine_inverse(
		const AdmSkeletalClips::RestTransform &t);

} // namespace opennova::simassets

#endif // OPENNOVA_SIMASSETS_ADM_SKELETAL_CLIPS_H
