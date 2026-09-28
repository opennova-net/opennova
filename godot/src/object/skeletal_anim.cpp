#include "object/skeletal_anim.h"

#include "resource_index/resource_root.h"
#include "util/string_convert.h"

#include <array>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/anim/aim_overlay.h> // the torso-bend overlay [orig: @0x4b1290]
#include <runtime/anim/skeletal_pose.h>

#include <utility>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

constexpr int kRightHandBoneIndex = 16;

// Entity_BuildBoneTransformMatrices applies the BN17 special row after every
// ordinary channel/overlay branch, including the fallback taken when overlay
// inputs are unavailable. Keep the zero-scale pose in one helper so a future
// early return cannot leave the baked personal weapon at the wrist. Preserve
// the sampled local origin: presentation must collapse at the animated joint,
// not drag partially weighted vertices toward world origin.
void apply_right_hand_local_collapse(Array &pose, bool collapse) {
	if (!collapse || pose.size() <= kRightHandBoneIndex) return;
	const Transform3D hand = pose[kRightHandBoneIndex];
	pose[kRightHandBoneIndex] =
			Transform3D(Basis(Vector3(), Vector3(), Vector3()), hand.origin);
}

// The sampler already produces engine-native (Y-up) transforms -- the same space Godot
// and the original engine use -- so a bone's parent-local pose maps DIRECTLY to a Godot
// Transform3D with no change-of-basis. (The mesh carries the (-x,y,z) handedness flip;
// the Skin's global-rest-inverse bind keeps mesh and bones consistent.) Our Quat is
// w-first {w,x,y,z}; Godot's Quaternion ctor is (x,y,z,w).
Transform3D bone_local_to_godot(const opennova::anim::Quat &q, const opennova::anim::Vec3 &p) {
	return Transform3D(Basis(Quaternion(q.x, q.y, q.z, q.w)), Vector3(p.x, p.y, p.z));
}

Array pose_to_array(const std::vector<opennova::anim::PoseBone> &p_pose) {
	Array out;
	for (const opennova::anim::PoseBone &pb : p_pose) {
		out.push_back(bone_local_to_godot(pb.rotation, pb.origin));
	}
	return out;
}

// Skeleton BIND pose from the .bad BadBone bind matrix (parent-local): the
// witnessed transpose/guard/orthonormalize is anim::bind_rest_local
// (engine/runtime/anim); this boxes its row-major result into the Transform3D
// the Skeleton3D rest consumes. The .3di mesh is skinned to THIS bind, so the
// rest must match it (not a sampled clip frame) -- otherwise the skin deforms
// ~identity at idle but collapses under large motion.
Transform3D bind_rest_to_godot(const opennova::anim::SkeletalClips::RestTransform &rest) {
	const float *rows = rest.rows;
	Basis basis;
	basis.rows[0] = Vector3(rows[0], rows[1], rows[2]);
	basis.rows[1] = Vector3(rows[3], rows[4], rows[5]);
	basis.rows[2] = Vector3(rows[6], rows[7], rows[8]);
	return Transform3D(basis, Vector3(rest.origin.x, rest.origin.y, rest.origin.z));
}

std::vector<opennova::anim::Vec3> to_model_origins(const PackedVector3Array &p_origins) {
	std::vector<opennova::anim::Vec3> out;
	out.reserve(static_cast<size_t>(p_origins.size()));
	for (int i = 0; i < p_origins.size(); ++i) {
		const Vector3 v = p_origins[i];
		out.push_back({static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
	}
	return out;
}

std::vector<int> to_model_parents(const PackedInt32Array &p_parents) {
	std::vector<int> out;
	out.reserve(static_cast<size_t>(p_parents.size()));
	for (int i = 0; i < p_parents.size(); ++i) {
		out.push_back(p_parents[i]);
	}
	return out;
}

}  // namespace

const opennova::anim::SkeletalClips &SkeletalAnim::rig() const {
	static const opennova::anim::SkeletalClips empty;
	return rig_ ? *rig_ : empty;
}

const SkeletalAnim::LoadedClip *SkeletalAnim::find_clip(const String &key) const {
	return rig().find_clip(opennova::to_std(key));
}

const SkeletalAnim::LoadedClip *SkeletalAnim::find_clip_variant(const String &key, int variant) const {
	return rig().find_clip_variant(opennova::to_std(key), variant);
}

bool SkeletalAnim::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root,
		const String &p_adm_name, const PackedVector3Array &p_model_bone_origins,
		const PackedInt32Array &p_model_bone_parents) {
	rig_.reset();
	last_error_ = String();
	if (p_resource_root.is_null()) {
		last_error_ = "Resource root is null";
		return false;
	}
	rig_ = p_resource_root->native_assets().skeletal_rig(opennova::to_std(p_adm_name),
			to_model_origins(p_model_bone_origins), to_model_parents(p_model_bone_parents));
	if (!rig_) last_error_ = "Animation rig missing or invalid: " + p_adm_name;
	return rig_ != nullptr;
}

bool SkeletalAnim::load_from_bad_files(const Ref<ResourceRoot> &p_resource_root,
		const String &p_skeleton_bad, const Dictionary &p_key_to_bad,
		const PackedVector3Array &p_model_bone_origins,
		const PackedInt32Array &p_model_bone_parents) {
	rig_.reset();
	last_error_ = String();
	if (p_resource_root.is_null()) {
		last_error_ = "Resource root is null";
		return false;
	}
	std::vector<std::pair<std::string, std::string>> clips;
	const Array keys = p_key_to_bad.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		const String name = p_key_to_bad[keys[i]];
		if (!key.is_empty() && !name.is_empty())
			clips.emplace_back(opennova::to_std(key), opennova::to_std(name));
	}
	rig_ = p_resource_root->native_assets().skeletal_rig_from_files(
			opennova::to_std(p_skeleton_bad), clips,
			to_model_origins(p_model_bone_origins), to_model_parents(p_model_bone_parents));
	if (!rig_) last_error_ = "Animation rig missing or invalid: " + p_skeleton_bad;
	return rig_ != nullptr;
}

String SkeletalAnim::slot_to_key(int p_slot) const {
	return String(rig().slot_to_key(p_slot).c_str());
}

PackedStringArray SkeletalAnim::get_clip_keys() const {
	PackedStringArray out;
	for (const LoadedClip &c : rig().clips()) {
		out.push_back(String(c.key.c_str()));
	}
	return out;
}

Array SkeletalAnim::get_skeleton_bones() const {
	Array out;
	for (size_t i = 0; i < rig().bones().size(); ++i) {
		Dictionary d;
		d["name"] = String(rig().bones()[i].name.c_str());
		d["parent_index"] = rig().bones()[i].parent_index;
		d["rest"] = bind_rest_to_godot(rig().bind_local()[i]);
		out.push_back(d);
	}
	return out;
}

int SkeletalAnim::get_clip_variant_count(const String &p_key) const {
	return rig().clip_variant_count(opennova::to_std(p_key));
}

PackedFloat32Array SkeletalAnim::get_clip_variant_lengths(const String &p_key) const {
	PackedFloat32Array out;
	for (float seconds : rig().clip_variant_lengths(opennova::to_std(p_key))) out.push_back(seconds);
	return out;
}

int SkeletalAnim::get_clip_frame_count(const String &p_key, int p_variant) const {
	const LoadedClip *c = find_clip_variant(p_key, p_variant);
	return c != nullptr ? static_cast<int>(c->clip.frame_count) : 0;
}

double SkeletalAnim::get_clip_phase_seconds(const String &p_key, int p_ticks,
		int p_variant, int p_armed_boundary) const {
	return rig().clip_seconds_at_tick(opennova::to_std(p_key), p_ticks, p_variant,
			p_armed_boundary);
}

float SkeletalAnim::get_clip_fps(const String &p_key, int p_variant) const {
	return rig().clip_fps(opennova::to_std(p_key), p_variant);
}

float SkeletalAnim::get_clip_length(const String &p_key, int p_variant) const {
	return rig().clip_length(opennova::to_std(p_key), p_variant);
}

bool SkeletalAnim::is_clip_looping(const String &p_key, int p_variant) const {
	const LoadedClip *c = find_clip_variant(p_key, p_variant);
	return c != nullptr && c->clip.loops();
}

Array SkeletalAnim::eval_pose(const String &p_key, double p_playhead_seconds,
		int p_variant) const {
	std::vector<opennova::anim::PoseBone> pose;
	rig().eval_pose(opennova::to_std(p_key), p_playhead_seconds, p_variant, pose);
	return pose_to_array(pose);
}

Array SkeletalAnim::eval_pose_blended(const String &p_source_key,
		double p_source_playhead_seconds, const String &p_target_key,
		double p_target_playhead_seconds, float p_weight,
		int p_source_variant, int p_target_variant) const {
	std::vector<opennova::anim::PoseBone> pose;
	rig().eval_pose_blended(opennova::to_std(p_source_key), p_source_playhead_seconds,
			opennova::to_std(p_target_key), p_target_playhead_seconds,
			p_weight, pose, p_source_variant, p_target_variant);
	return pose_to_array(pose);
}

PackedInt32Array SkeletalAnim::get_overlay_classes() const {
	// The BN## parse and the 19-entry class table live engine-side
	// (anim::overlay_class_for_bone_name); the model bone order IS the BN order
	// (§14.2), but parsing the tag keeps husk/accessory variants correct
	// without positional trust.
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(rig().bones().size()));
	for (size_t i = 0; i < rig().bones().size(); ++i) {
		out[static_cast<int64_t>(i)] =
				rig().overlay_classes()[i];
	}
	return out;
}

Array SkeletalAnim::eval_pose_overlay_deltas(const String &p_key, double p_playhead_seconds,
		const PackedInt32Array &p_classes, const Basis *p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand, const String &p_wpn_prev_key,
		double p_wpn_prev_playhead_seconds, float p_wpn_weight,
		int p_wpn_variant, int p_wpn_prev_variant, int p_variant) const {
	std::vector<opennova::anim::PoseBone> pose;
	rig().eval_pose(opennova::to_std(p_key), p_playhead_seconds, p_variant, pose);
	return apply_pose_overlay(std::move(pose),
			p_classes, p_deltas, p_wpn_key, p_wpn_playhead_seconds,
			p_collapse_right_hand, p_wpn_prev_key, p_wpn_prev_playhead_seconds,
			p_wpn_weight, p_wpn_variant, p_wpn_prev_variant);
}

Array SkeletalAnim::eval_pose_blended_overlay_deltas(
		const String &p_source_key, double p_source_playhead_seconds,
		const String &p_target_key, double p_target_playhead_seconds,
		float p_weight, const PackedInt32Array &p_classes,
		const Basis *p_deltas, const String &p_wpn_key,
		double p_wpn_playhead_seconds, bool p_collapse_right_hand,
		const String &p_wpn_prev_key, double p_wpn_prev_playhead_seconds,
		float p_wpn_weight, int p_wpn_variant, int p_wpn_prev_variant,
		int p_source_variant, int p_target_variant) const {
	std::vector<opennova::anim::PoseBone> pose;
	rig().eval_pose_blended(opennova::to_std(p_source_key), p_source_playhead_seconds,
			opennova::to_std(p_target_key), p_target_playhead_seconds, p_weight, pose,
			p_source_variant, p_target_variant);
	return apply_pose_overlay(std::move(pose),
			p_classes, p_deltas, p_wpn_key, p_wpn_playhead_seconds,
			p_collapse_right_hand, p_wpn_prev_key, p_wpn_prev_playhead_seconds,
			p_wpn_weight, p_wpn_variant, p_wpn_prev_variant);
}

Array SkeletalAnim::apply_pose_overlay(std::vector<opennova::anim::PoseBone> native_pose,
		const PackedInt32Array &p_classes, const Basis *p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand, const String &p_wpn_prev_key,
		double p_wpn_prev_playhead_seconds, float p_wpn_weight,
		int p_wpn_variant, int p_wpn_prev_variant) const {
	std::vector<uint8_t> classes;
	classes.reserve(static_cast<size_t>(p_classes.size()));
	for (int i = 0; i < p_classes.size(); ++i) {
		const int c = p_classes[i];
		classes.push_back(static_cast<uint8_t>(
				c >= 0 && c < int(opennova::anim::kOverlayClassCount) ? c : 0));
	}
	opennova::anim::Quat deltas[opennova::anim::kOverlayClassCount];
	if (p_deltas) {
		for (size_t c = 0; c < opennova::anim::kOverlayClassCount; ++c) {
			const Quaternion q = p_deltas[c].get_rotation_quaternion();
			deltas[c] = {float(q.w), float(q.x), float(q.y), float(q.z)};
		}
	}
	rig().apply_pose_overlay(native_pose, p_deltas ? deltas : nullptr, classes,
			opennova::to_std(p_wpn_key), p_wpn_playhead_seconds,
			opennova::to_std(p_wpn_prev_key), p_wpn_prev_playhead_seconds,
			p_wpn_weight, p_wpn_variant, p_wpn_prev_variant);
	Array pose = pose_to_array(native_pose);
	apply_right_hand_local_collapse(pose, p_collapse_right_hand);
	return pose;
}

void SkeletalAnim::pose_skeleton_deltas(Skeleton3D *p_skeleton, const String &p_key,
		double p_playhead_seconds, int p_variant,
		const PackedInt32Array &p_classes, const Basis *p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand, const String &p_wpn_prev_key,
		double p_wpn_prev_playhead_seconds, float p_wpn_weight,
		int p_wpn_variant, int p_wpn_prev_variant) const {
	// Branch mirror of ObjectModel.advance_body_animation: overlay inputs
	// present -> the composed overlay pose, else the plain clip pose.
	Array pose;
	if (p_deltas != nullptr && !p_classes.is_empty()) {
		pose = eval_pose_overlay_deltas(p_key, p_playhead_seconds, p_classes, p_deltas,
				p_wpn_key, p_wpn_playhead_seconds, p_collapse_right_hand,
				p_wpn_prev_key, p_wpn_prev_playhead_seconds, p_wpn_weight,
				p_wpn_variant, p_wpn_prev_variant, p_variant);
	} else {
		pose = eval_pose(p_key, p_playhead_seconds, p_variant);
	}
	write_pose_to_skeleton(p_skeleton, pose, p_collapse_right_hand);
}

void SkeletalAnim::pose_skeleton_blended(Skeleton3D *p_skeleton,
		const String &p_source_key, double p_source_playhead_seconds,
		const String &p_target_key, double p_target_playhead_seconds,
		float p_weight, const PackedInt32Array &p_classes,
		const Basis *p_deltas, const String &p_wpn_key,
		double p_wpn_playhead_seconds, bool p_collapse_right_hand,
		const String &p_wpn_prev_key, double p_wpn_prev_playhead_seconds,
		float p_wpn_weight, int p_wpn_variant, int p_wpn_prev_variant,
		int p_source_variant, int p_target_variant) const {
	Array pose;
	if (p_deltas != nullptr && !p_classes.is_empty()) {
		pose = eval_pose_blended_overlay_deltas(
				p_source_key, p_source_playhead_seconds,
				p_target_key, p_target_playhead_seconds, p_weight,
				p_classes, p_deltas, p_wpn_key,
				p_wpn_playhead_seconds, p_collapse_right_hand,
				p_wpn_prev_key, p_wpn_prev_playhead_seconds, p_wpn_weight,
				p_wpn_variant, p_wpn_prev_variant, p_source_variant,
				p_target_variant);
	} else {
		pose = eval_pose_blended(
				p_source_key, p_source_playhead_seconds,
				p_target_key, p_target_playhead_seconds, p_weight,
				p_source_variant, p_target_variant);
	}
	write_pose_to_skeleton(p_skeleton, pose, p_collapse_right_hand);
}

namespace {
// Array (GDScript) -> the 9-slot Basis table the pointer forms read. Returns
// nullptr for an empty Array (no overlay); a missing/non-Basis slot is identity.
const Basis *deltas_from_array(const Array &p_deltas, std::array<Basis, 9> &out) {
	if (p_deltas.is_empty()) return nullptr;
	for (int i = 0; i < 9; ++i) {
		out[static_cast<size_t>(i)] = Basis();
		if (i < p_deltas.size() && p_deltas[i].get_type() == Variant::BASIS)
			out[static_cast<size_t>(i)] = static_cast<Basis>(p_deltas[i]);
	}
	return out.data();
}
} // namespace

Array SkeletalAnim::eval_pose_overlay(const String &p_key, double p_playhead_seconds,
		const PackedInt32Array &p_classes, const Array &p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand, const String &p_wpn_prev_key,
		double p_wpn_prev_playhead_seconds, float p_wpn_weight,
		int p_wpn_variant, int p_wpn_prev_variant) const {
	std::array<Basis, 9> table;
	return eval_pose_overlay_deltas(p_key, p_playhead_seconds, p_classes,
			deltas_from_array(p_deltas, table), p_wpn_key, p_wpn_playhead_seconds,
			p_collapse_right_hand, p_wpn_prev_key, p_wpn_prev_playhead_seconds,
			p_wpn_weight, p_wpn_variant, p_wpn_prev_variant);
}

Array SkeletalAnim::eval_pose_blended_overlay(
		const String &p_source_key, double p_source_playhead_seconds,
		const String &p_target_key, double p_target_playhead_seconds,
		float p_weight, const PackedInt32Array &p_classes,
		const Array &p_deltas, const String &p_wpn_key,
		double p_wpn_playhead_seconds, bool p_collapse_right_hand,
		const String &p_wpn_prev_key, double p_wpn_prev_playhead_seconds,
		float p_wpn_weight, int p_wpn_variant, int p_wpn_prev_variant) const {
	std::array<Basis, 9> table;
	return eval_pose_blended_overlay_deltas(p_source_key, p_source_playhead_seconds,
			p_target_key, p_target_playhead_seconds, p_weight, p_classes,
			deltas_from_array(p_deltas, table), p_wpn_key, p_wpn_playhead_seconds,
			p_collapse_right_hand, p_wpn_prev_key, p_wpn_prev_playhead_seconds,
			p_wpn_weight, p_wpn_variant, p_wpn_prev_variant);
}

void SkeletalAnim::pose_skeleton(Skeleton3D *p_skeleton, const String &p_key,
		double p_playhead_seconds, int p_variant,
		const PackedInt32Array &p_classes, const Array &p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand, const String &p_wpn_prev_key,
		double p_wpn_prev_playhead_seconds, float p_wpn_weight,
		int p_wpn_variant, int p_wpn_prev_variant) const {
	std::array<Basis, 9> table;
	pose_skeleton_deltas(p_skeleton, p_key, p_playhead_seconds, p_variant, p_classes,
			deltas_from_array(p_deltas, table), p_wpn_key, p_wpn_playhead_seconds,
			p_collapse_right_hand, p_wpn_prev_key, p_wpn_prev_playhead_seconds,
			p_wpn_weight, p_wpn_variant, p_wpn_prev_variant);
}

void SkeletalAnim::write_pose_to_skeleton(Skeleton3D *p_skeleton,
		const Array &p_pose, bool p_collapse_right_hand) const {
	if (p_skeleton == nullptr) {
		return;
	}
	const int count = MIN(static_cast<int>(p_pose.size()),
			static_cast<int>(p_skeleton->get_bone_count()));
	for (int i = 0; i < count; ++i) {
		const Transform3D t = p_pose[i];
		if (p_collapse_right_hand && i == 16) {
			// eval_pose_overlay preserves BN17's sampled parent-local joint origin
			// while clearing its basis. Apply that origin directly and collapse
			// scale there (zero scale keeps mixed-weight triangles at the actor
			// instead of spanning to world zero). Also covers the no-overlay
			// eval_pose fallback.
			p_skeleton->set_bone_pose_position(i, t.origin);
			p_skeleton->set_bone_pose_rotation(i, Quaternion());
			p_skeleton->set_bone_pose_scale(i, Vector3(0, 0, 0));
		} else {
			p_skeleton->set_bone_pose_position(i, t.origin);
			p_skeleton->set_bone_pose_rotation(i, t.basis.get_rotation_quaternion());
			p_skeleton->set_bone_pose_scale(i, t.basis.get_scale());
		}
	}
}

void SkeletalAnim::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "adm_name", "model_bone_origins", "model_bone_parents"), &SkeletalAnim::load_from_resource_root, DEFVAL(PackedVector3Array()), DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("load_from_bad_files", "resource_root", "skeleton_bad", "key_to_bad", "model_bone_origins", "model_bone_parents"), &SkeletalAnim::load_from_bad_files, DEFVAL(PackedVector3Array()), DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("is_loaded"), &SkeletalAnim::is_loaded);
	ClassDB::bind_method(D_METHOD("get_last_error"), &SkeletalAnim::get_last_error);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &SkeletalAnim::get_bone_count);
	ClassDB::bind_method(D_METHOD("get_clip_keys"), &SkeletalAnim::get_clip_keys);
	ClassDB::bind_method(D_METHOD("get_skeleton_bones"), &SkeletalAnim::get_skeleton_bones);
	ClassDB::bind_method(D_METHOD("slot_to_key", "slot"), &SkeletalAnim::slot_to_key);
	ClassDB::bind_method(D_METHOD("has_clip", "key"), &SkeletalAnim::has_clip);
	ClassDB::bind_method(D_METHOD("get_clip_variant_count", "key"), &SkeletalAnim::get_clip_variant_count);
	ClassDB::bind_method(D_METHOD("get_clip_variant_lengths", "key"), &SkeletalAnim::get_clip_variant_lengths);
	ClassDB::bind_method(D_METHOD("get_clip_frame_count", "key", "variant"), &SkeletalAnim::get_clip_frame_count, DEFVAL(0));
	ClassDB::bind_method(
			D_METHOD("get_clip_phase_seconds", "key", "ticks", "variant", "armed_boundary"),
			&SkeletalAnim::get_clip_phase_seconds, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_clip_fps", "key", "variant"), &SkeletalAnim::get_clip_fps, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_clip_length", "key", "variant"), &SkeletalAnim::get_clip_length, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("is_clip_looping", "key", "variant"), &SkeletalAnim::is_clip_looping, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("eval_pose", "key", "playhead_seconds", "variant"), &SkeletalAnim::eval_pose, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("eval_pose_blended", "source_key", "source_playhead_seconds", "target_key", "target_playhead_seconds", "weight", "source_variant", "target_variant"), &SkeletalAnim::eval_pose_blended, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_overlay_classes"), &SkeletalAnim::get_overlay_classes);
	ClassDB::bind_method(D_METHOD("eval_pose_overlay", "key", "playhead_seconds", "classes", "deltas", "wpn_key", "wpn_playhead_seconds", "collapse_right_hand", "wpn_prev_key", "wpn_prev_playhead_seconds", "wpn_weight", "wpn_variant", "wpn_prev_variant"), &SkeletalAnim::eval_pose_overlay, DEFVAL(String()), DEFVAL(0.0), DEFVAL(false), DEFVAL(String()), DEFVAL(0.0), DEFVAL(1.0f), DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("eval_pose_blended_overlay", "source_key", "source_playhead_seconds", "target_key", "target_playhead_seconds", "weight", "classes", "deltas", "wpn_key", "wpn_playhead_seconds", "collapse_right_hand", "wpn_prev_key", "wpn_prev_playhead_seconds", "wpn_weight", "wpn_variant", "wpn_prev_variant"), &SkeletalAnim::eval_pose_blended_overlay, DEFVAL(String()), DEFVAL(0.0), DEFVAL(false), DEFVAL(String()), DEFVAL(0.0), DEFVAL(1.0f), DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("pose_skeleton", "skeleton", "key", "playhead_seconds", "variant", "classes", "deltas", "wpn_key", "wpn_playhead_seconds", "collapse_right_hand", "wpn_prev_key", "wpn_prev_playhead_seconds", "wpn_weight", "wpn_variant", "wpn_prev_variant"), &SkeletalAnim::pose_skeleton, DEFVAL(String()), DEFVAL(0.0), DEFVAL(false), DEFVAL(String()), DEFVAL(0.0), DEFVAL(1.0f), DEFVAL(0), DEFVAL(0));
}
