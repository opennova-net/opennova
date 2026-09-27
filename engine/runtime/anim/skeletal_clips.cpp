// Shared skeletal loading and evaluation for simulation and presentation
// (ADR 0044). Mutable playheads remain with the consuming entity.
#include <runtime/anim/skeletal_clips.h>

#include <formats/adm/adm.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/anim/aim_overlay.h>
#include <formats/bad/bad.h>
#include <base/io/strutil.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/body_anim.h>

#include <algorithm>
#include <string_view>
#include <utility>

using namespace opennova::adm;
using namespace opennova::bad;

namespace opennova::anim {

// Affine compose in the binding's Transform3D semantics: rows are the
// column-vector rotation matrix; out = a ∘ b (b applied first).
SkeletalClips::RestTransform rest_mul(
		const SkeletalClips::RestTransform &a,
		const SkeletalClips::RestTransform &b) {
	SkeletalClips::RestTransform o;
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) {
			o.rows[r * 3 + c] = a.rows[r * 3 + 0] * b.rows[0 * 3 + c] +
					a.rows[r * 3 + 1] * b.rows[1 * 3 + c] +
					a.rows[r * 3 + 2] * b.rows[2 * 3 + c];
		}
	}
	o.origin.x = a.rows[0] * b.origin.x + a.rows[1] * b.origin.y +
			a.rows[2] * b.origin.z + a.origin.x;
	o.origin.y = a.rows[3] * b.origin.x + a.rows[4] * b.origin.y +
			a.rows[5] * b.origin.z + a.origin.y;
	o.origin.z = a.rows[6] * b.origin.x + a.rows[7] * b.origin.y +
			a.rows[8] * b.origin.z + a.origin.z;
	return o;
}

// Full affine inverse (cofactor basis inverse + rotated-negated origin) — the
// binding used Transform3D::affine_inverse on the accumulated rest. The rest
// chain is orthonormal by construction, so the determinant never vanishes;
// a degenerate input falls back to identity defensively.
SkeletalClips::RestTransform rest_affine_inverse(
		const SkeletalClips::RestTransform &t) {
	SkeletalClips::RestTransform o;
	const float a00 = t.rows[0], a01 = t.rows[1], a02 = t.rows[2];
	const float a10 = t.rows[3], a11 = t.rows[4], a12 = t.rows[5];
	const float a20 = t.rows[6], a21 = t.rows[7], a22 = t.rows[8];
	const float b01 = a22 * a11 - a12 * a21;
	const float b11 = -a22 * a10 + a12 * a20;
	const float b21 = a21 * a10 - a11 * a20;
	const float det = a00 * b01 + a01 * b11 + a02 * b21;
	if (det == 0.0f) {
		o.origin = anim::Vec3{-t.origin.x, -t.origin.y, -t.origin.z};
		return o;
	}
	const float inv_det = 1.0f / det;
	o.rows[0] = b01 * inv_det;
	o.rows[1] = (-a22 * a01 + a02 * a21) * inv_det;
	o.rows[2] = (a12 * a01 - a02 * a11) * inv_det;
	o.rows[3] = b11 * inv_det;
	o.rows[4] = (a22 * a00 - a02 * a20) * inv_det;
	o.rows[5] = (-a12 * a00 + a02 * a10) * inv_det;
	o.rows[6] = b21 * inv_det;
	o.rows[7] = (-a21 * a00 + a01 * a20) * inv_det;
	o.rows[8] = (a11 * a00 - a01 * a10) * inv_det;
	o.origin.x = -(o.rows[0] * t.origin.x + o.rows[1] * t.origin.y +
			o.rows[2] * t.origin.z);
	o.origin.y = -(o.rows[3] * t.origin.x + o.rows[4] * t.origin.y +
			o.rows[5] * t.origin.z);
	o.origin.z = -(o.rows[6] * t.origin.x + o.rows[7] * t.origin.y +
			o.rows[8] * t.origin.z);
	return o;
}

void SkeletalClips::clear() {
	loaded_ = false;
	fk_valid_ = false;
	adm_name_.clear();
	bones_.clear();
	parents_.clear();
	classes_.clear();
	weapon_mask_.clear();
	any_weapon_mask_ = false;
	bind_pose_.clear();
	bind_local_.clear();
	rest_global_.clear();
	rest_global_inverse_.clear();
	clips_.clear();
	clip_index_.clear();
}

bool SkeletalClips::load_from_adm(
		const assets::AssetStore *assets, const std::string &adm_name,
		const std::vector<anim::Vec3> &model_bone_origins,
		const std::vector<int> &model_bone_parents) {
	clear();
	if (!assets || adm_name.empty()) return false;
	const auto map = assets->animation_map(adm_name);
	if (!map) return false;
	// The rig's bind is slot 0's head once the table is read. A row names slot 0
	// when its key past the first five characters is "reset", in any case (slot
	// name 0 @0x7c3264). Each clip registered there REPLACES the head instead of
	// joining a ring, and a variant whose .bad does not load registers nothing, so
	// the bind is the last variant that loads of the last such row: the head
	// AnimMap_RegisterEntity pins into channel+44.
	// [orig: AnimMap_FindSlotByName @0x40cfa0 (stricmp on key + 5);
	//  AnimMap_ParseConfigLine @0x40cb60, the null test @0x40cbe7;
	//  AnimMap_RegisterBoneNode @0x40c2d0, slot 0 @0x40c365 -> the head store
	//  @0x40c38b; AnimMap_RegisterEntity @0x40bb60, the pin @0x40bbe3]
	std::string reset_value;
	for (size_t i = 0; i < map->count; ++i) {
		const auto &entry = map->entries[i];
		if (!adm_key_names_slot(entry.key, "reset")) continue;
		for (size_t v = 0; v < entry.variant_count; ++v)
			if (entry.variants[v][0] != '\0' && assets->bone_animation(entry.variants[v]))
				reset_value = entry.variants[v];
	}
	// A table with no slot-0 clip never binds: its load reads slot 0's null head
	// unchecked, and registration frees the channel it allocated, so no entity
	// animates through it. [orig: AnimMap_LoadAdmFile @0x40cc40, the read
	// @0x40ce11..0x40ce16; AnimMap_RegisterEntity @0x40bb60, the free @0x40bbc4]
	if (reset_value.empty()) return false;
	std::vector<std::pair<std::string, std::string>> clips;
	// Every authored token registers a variant, including repeated files, on the
	// slot its row's key names past the first five characters, so a row keyed
	// `ANIM_IDLE` or `xxxx_idle` registers under the `anim_idle` every lookup
	// spells; a key naming none of the 252 slots registers nothing.
	// [orig: AnimMap_ParseConfigLine @0x40cb60 -> AnimMap_FindSlotByName
	// @0x40cfa0, the found-slot gate @0x40cba4; AnimMap_RegisterBoneNode @0x40c2d0]
	for (size_t i = 0; i < map->count; ++i) {
		const auto &entry = map->entries[i];
		if (adm_slot_index(entry.key) < 0) continue;
		const std::string key = adm_slot_key(entry.key);
		for (size_t v = 0; v < entry.variant_count; ++v)
			if (entry.variants[v] && entry.variants[v][0])
				clips.emplace_back(key, entry.variants[v]);
	}
	if (!load_from_files(assets, reset_value, clips, model_bone_origins, model_bone_parents))
		return false;
	adm_name_ = adm_name;
	return true;
}

bool SkeletalClips::load_from_files(
		const assets::AssetStore *assets, const std::string &skeleton_bad,
		const std::vector<std::pair<std::string, std::string>> &clip_bads,
		const std::vector<anim::Vec3> &model_bone_origins,
		const std::vector<int> &model_bone_parents) {
	clear();
	if (!assets) return false;
	const auto skeleton = assets->bone_animation(skeleton_bad);
	if (!skeleton) return false;
	const BadFile &skeleton_bf = *skeleton;
	adm_name_ = skeleton_bad;

	// Pass 1: the reset/skeleton .bad -> canonical bones, shared rest origins,
	// bind pose. Model-table mode (origins + parents paired): the .3di model's
	// bone table defines the rig — count and hierarchy from the model rows,
	// rest POSITIONS reconstructed from the model pivots + the reset .bad's
	// bind rotations (anim positions_from_model; net-re §5.40). The .bad's own
	// bone count/parents/positions are never read on that path.
	// [orig: BoneAnim_BuildWorldMatrices @0x40c400 bounds the FK by
	//  modelDef+52 and walks the modelDef+56 rows]
	const bool model_table = !model_bone_parents.empty() &&
			model_bone_parents.size() == model_bone_origins.size();
	std::vector<anim::Vec3> shared_rest;
	const std::vector<int> rig_parents =
			model_table ? model_bone_parents : std::vector<int>{};
	{
		const std::vector<anim::Vec3> table_positions = model_table
				? anim::positions_from_model(skeleton_bf, model_bone_parents,
						  model_bone_origins)
				: std::vector<anim::Vec3>{};
		const anim::Clip reset_clip = anim::sample_clip(
				skeleton_bf, table_positions,
				/*model_bind=*/false, nullptr, rig_parents);
		bones_ = reset_clip.bones;
		// Legacy positional override (no parents supplied): model-supplied
		// per-bone bind positions OVERRIDE the reset .bad's — BadBone.position
		// is a lossy export, the .3di carries the real pivots.
		const bool use_model =
				!model_table && model_bone_origins.size() == bones_.size();
		shared_rest.resize(bones_.size());
		bind_pose_.resize(bones_.size());
		bind_local_.resize(bones_.size());
		parents_.resize(bones_.size());
		classes_.resize(bones_.size());
		weapon_mask_.assign(bones_.size(), 0);
		rest_global_.resize(bones_.size());
		rest_global_inverse_.resize(bones_.size());
		fk_valid_ = true;
		for (size_t i = 0; i < bones_.size(); ++i) {
			if (use_model) {
				bones_[i].rest_position[0] = model_bone_origins[i].x;
				bones_[i].rest_position[1] = model_bone_origins[i].y;
				bones_[i].rest_position[2] = model_bone_origins[i].z;
			}
			// Model-table rows past the .bad's records carry no name;
			// synthesize stable ones (the binding's MDL<i> rule).
			if (bones_[i].name.empty()) {
				bones_[i].name = "MDL" + std::to_string(i);
			}
			shared_rest[i] = {bones_[i].rest_position[0],
					bones_[i].rest_position[1], bones_[i].rest_position[2]};
			const int parent = bones_[i].parent_index;
			parents_[i] = parent;
			const anim::ClipBone *p =
					(parent >= 0 && static_cast<size_t>(parent) < bones_.size())
							? &bones_[parent]
							: nullptr;
			RestTransform bind;
			anim::bind_rest_local(bones_[i], p, bind.rows, bind.origin);
			bind_local_[i] = bind;
			bind_pose_[i].rotation = anim::mat3_to_quat(bind.rows);
			bind_pose_[i].origin = bind.origin;
			classes_[i] = anim::overlay_class_for_bone_name(bones_[i].name);
			const int model_index =
					anim::model_bone_index_from_name(bones_[i].name);
			if (model_index >= 0 &&
					anim::weapon_channel_masks_bone(model_index)) {
				weapon_mask_[i] = 1;
				any_weapon_mask_ = true;
			}
			// The FK/rest accumulation assumes topological parent order — the
			// same validation the binding's collision consumer applied.
			if (parent < -1 || parent >= static_cast<int>(i)) {
				fk_valid_ = false;
			}
			if (fk_valid_) {
				rest_global_[i] = parent >= 0
						? rest_mul(rest_global_[static_cast<size_t>(parent)],
								  bind)
						: bind;
				rest_global_inverse_[i] = rest_affine_inverse(rest_global_[i]);
			}
		}
	}

	// Pass 2: sample every clip against the SHARED skeleton rest origins (not
	// each clip's own) [orig: AnimMap_RegisterEntity @0x40bb60 pins the rig
	// skeleton once; AnimMap_PlayAnimBySlot @0x40bda0 never rebuilds it]. The
	// skeleton .bad is also the bind whose flag gates each clip's translations
	// [orig: AnimChannel_ComputeBoneMatrices @0x410da0, @0x410de7].
	for (const auto &kv : clip_bads) {
		if (kv.first.empty() || kv.second.empty()) continue;
		const auto file = assets->bone_animation(kv.second);
		if (!file) continue;
		LoadedClip lc;
		lc.key = kv.first;
		lc.clip = anim::sample_clip(*file, shared_rest, false, &skeleton_bf, rig_parents);
		clips_.push_back(std::move(lc));
	}

	if (clips_.empty()) {
		rebuild_clip_index();
		return false;
	}
	rebuild_clip_index();
	loaded_ = true;
	return true;
}

void SkeletalClips::rebuild_clip_index() {
	clip_index_.clear();
	for (size_t i = 0; i < clips_.size(); ++i) {
		clip_index_[strutil::to_lower(clips_[i].key)].push_back(i);
	}
}

const SkeletalClips::LoadedClip *SkeletalClips::find_clip(
		const std::string &key) const {
	// Case-insensitive: weapon.def ACTION rows author ANIM_WPN_* uppercase
	// while the .adm stores anim_wpn_* lowercase.
	// [orig: AnimMap_FindSlotByName @ 0x40cfa0 — stricmp]
	const auto it = clip_index_.find(strutil::to_lower(key));
	if (it == clip_index_.end() || it->second.empty()) {
		return nullptr;
	}
	return &clips_[it->second.front()];
}

const SkeletalClips::LoadedClip *SkeletalClips::find_clip_variant(
		const std::string &key, int variant) const {
	if (variant <= 0) {
		return find_clip(key);
	}
	// Variants registered under one key stay in .adm file order — the wrapped
	// serve walks the ring.
	const auto it = clip_index_.find(strutil::to_lower(key));
	if (it == clip_index_.end() || it->second.empty()) {
		return nullptr;
	}
	return &clips_[it->second[static_cast<size_t>(variant) % it->second.size()]];
}

bool SkeletalClips::has_clip(const std::string &key) const {
	return find_clip(key) != nullptr;
}

std::string SkeletalClips::slot_to_key(int slot) const {
	const char *key = world::body_anim_adm_key(slot);
	if (key[0] != '\0' && has_clip(key)) return key;
	// A model whose .adm lacks the requested key still poses sensibly.
	if (has_clip("anim_idle")) return "anim_idle";
	if (has_clip("anim_reset")) return "anim_reset";
	return {};
}

int SkeletalClips::clip_variant_count(const std::string &key) const {
	const auto found = clip_index_.find(strutil::to_lower(key));
	return found == clip_index_.end() ? 0 : static_cast<int>(found->second.size());
}

std::vector<float> SkeletalClips::clip_variant_lengths(const std::string &key) const {
	std::vector<float> out;
	const auto found = clip_index_.find(strutil::to_lower(key));
	if (found == clip_index_.end()) return out;
	out.reserve(found->second.size());
	for (size_t index : found->second) {
		const auto &clip = clips_[index].clip;
		out.push_back(clip.fps > 0 && clip.frame_count > 0
				? static_cast<float>(clip.frame_count) / static_cast<float>(clip.fps) : 0.0f);
	}
	return out;
}

float SkeletalClips::clip_length(const std::string &key, int variant) const {
	const LoadedClip *c = find_clip_variant(key, variant);
	if (c == nullptr || c->clip.fps == 0 || c->clip.frame_count == 0) return 0.0f;
	return static_cast<float>(c->clip.frame_count) / static_cast<float>(c->clip.fps);
}

double SkeletalClips::clip_seconds_at_tick(const std::string &key,
                                               int32_t ticks, int variant) const {
	const LoadedClip *clip = find_clip_variant(key, variant);
	return clip ? clip->clip.playback().seconds_at(ticks) : 0.0;
}

float SkeletalClips::clip_fps(const std::string &key, int variant) const {
	const LoadedClip *c = find_clip_variant(key, variant);
	return c != nullptr ? static_cast<float>(c->clip.fps) : 0.0f;
}

void SkeletalClips::eval_pose(const std::string &key,
		double playhead_seconds, int variant,
		std::vector<anim::PoseBone> &r_pose) const {
	const LoadedClip *lc = find_clip_variant(key, variant);
	if (lc == nullptr || lc->clip.frame_count == 0) {
		// Unknown / empty clip: fall back to the bind pose.
		r_pose = bind_pose_;
		return;
	}
	anim::eval_clip_pose(lc->clip, playhead_seconds, r_pose);
}

void SkeletalClips::eval_pose_blended(const std::string &p_source_key,
		double source_seconds, const std::string &p_target_key,
		double target_seconds, float weight,
		std::vector<anim::PoseBone> &r_pose,
		int source_variant, int target_variant) const {
	std::string source_key = p_source_key;
	std::string target_key = p_target_key;
	const std::string reset_key("anim_reset");
	const bool have_reset = find_clip(reset_key) != nullptr;
	if (find_clip(source_key) == nullptr && have_reset) {
		source_key = reset_key;
	}
	if (find_clip(target_key) == nullptr && have_reset) {
		target_key = reset_key;
	}
	const bool source_valid = find_clip(source_key) != nullptr;
	const bool target_valid = find_clip(target_key) != nullptr;
	if (!source_valid) {
		eval_pose(target_key, target_seconds, target_variant, r_pose);
		return;
	}
	if (!target_valid) {
		eval_pose(source_key, source_seconds, source_variant, r_pose);
		return;
	}
	const float w = std::clamp(weight, 0.0f, 1.0f);
	if (w <= 0.0f) {
		eval_pose(source_key, source_seconds, source_variant, r_pose);
		return;
	}
	if (w >= 1.0f) {
		eval_pose(target_key, target_seconds, target_variant, r_pose);
		return;
	}
	// Semantic states can map to the same BAD (including missing states that
	// both bind RESET) while retaining independent channel playheads. They
	// must still blend; key equality alone is not a valid single-sample
	// shortcut.
	std::vector<anim::PoseBone> source;
	std::vector<anim::PoseBone> target;
	eval_pose(source_key, source_seconds, source_variant, source);
	eval_pose(target_key, target_seconds, target_variant, target);
	anim::blend_poses(source, target, w, r_pose);
}

void SkeletalClips::splice_weapon_channel(std::vector<anim::PoseBone> &pose,
		const std::string &weapon_key, double weapon_seconds,
		const std::string &weapon_prev_key, double weapon_prev_seconds,
		float weapon_blend_weight, int weapon_variant, int weapon_prev_variant) const {
	// Hard override of the mask bones' WORLD rotations with the weapon
	// channel's clip at its own playhead, then re-localize the complete mixed
	// hierarchy. The primary pose keeps every local origin (the shared
	// skeleton/model pivots own translation).
	// [orig: mask @0x4b14db, second AnimChannel_ComputeBoneMatrices @0x4b16a7;
	//  world-wac-ai-re.md §14.8.6]
	// Empty key = the §14.8.6 gate is off; that is the ONLY no-splice case. A key
	// whose clip is absent binds RESET at registration rather than no-opping
	// [orig: the backfill loops @0x40bc24 / @0x40bd2e; §14.8.1] — eval_pose_blended
	// owns that fallback, and routing through it also gives the secondary channel
	// its own cross-fade window, as the shared AnimMap_UpdateEntity body does
	// [orig: @0x40b8c0 -> @0x40b5f0].
	if (weapon_key.empty() || !any_weapon_mask_) {
		return;
	}
	std::vector<anim::PoseBone> wpose;
	eval_pose_blended(weapon_prev_key.empty() ? weapon_key : weapon_prev_key,
			weapon_prev_seconds, weapon_key, weapon_seconds, weapon_blend_weight,
			wpose, weapon_prev_key.empty() ? weapon_variant : weapon_prev_variant,
			weapon_variant);
	const size_t n = pose.size();
	if (wpose.size() != n || n != bones_.size()) {
		return;
	}
	std::vector<anim::Quat> primary_rot(n);
	std::vector<anim::Quat> weapon_rot(n);
	for (size_t i = 0; i < n; ++i) {
		primary_rot[i] = pose[i].rotation;
		weapon_rot[i] = wpose[i].rotation;
	}
	anim::splice_weapon_channel_rotations(
			parents_, weapon_mask_.data(), weapon_rot, primary_rot);
	for (size_t i = 0; i < n; ++i) {
		pose[i].rotation = primary_rot[i];
	}
}

bool SkeletalClips::eval_composed_pose(const std::string &primary_key,
		double primary_seconds, bool blended, const std::string &source_key,
		double source_seconds, float blend_weight, const anim::Quat deltas[],
		const std::string &weapon_key, double weapon_seconds,
		std::vector<anim::PoseBone> &r_pose, const std::string &weapon_prev_key,
		double weapon_prev_seconds, float weapon_blend_weight, int weapon_variant,
		int weapon_prev_variant, int primary_variant, int source_variant) const {
	if (blended) {
		eval_pose_blended(source_key, source_seconds, primary_key,
				primary_seconds, blend_weight, r_pose, source_variant, primary_variant);
	} else {
		eval_pose(primary_key, primary_seconds, primary_variant, r_pose);
	}
	apply_pose_overlay(r_pose, deltas, classes_, weapon_key, weapon_seconds,
			weapon_prev_key, weapon_prev_seconds, weapon_blend_weight,
			weapon_variant, weapon_prev_variant);
	return !r_pose.empty();
}

void SkeletalClips::apply_pose_overlay(std::vector<anim::PoseBone> &pose,
		const anim::Quat *deltas, const std::vector<uint8_t> &classes,
		const std::string &weapon_key, double weapon_seconds,
		const std::string &weapon_prev_key, double weapon_prev_seconds,
		float weapon_weight, int weapon_variant, int weapon_prev_variant) const {
	// The witnessed order: primary sample -> weapon-channel mask override ->
	// aim overlay on top [orig: @0x4b14a7..@0x4b16a7].
	splice_weapon_channel(pose, weapon_key, weapon_seconds, weapon_prev_key,
			weapon_prev_seconds, weapon_weight, weapon_variant, weapon_prev_variant);
	const size_t n = pose.size();
	if (!n || n != bones_.size() || !deltas || classes.size() < n) return;
	std::vector<anim::Quat> rotations(n);
	for (size_t i = 0; i < n; ++i) rotations[i] = pose[i].rotation;
	anim::apply_aim_overlay(parents_, deltas, classes.data(), rotations);
	for (size_t i = 0; i < n; ++i) pose[i].rotation = rotations[i];
}

} // namespace opennova::anim
