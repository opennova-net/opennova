#include "object/nova_skeletal_anim.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <bad/bad.h>
#include <adm/adm.h>
#include <world/body_anim.h>

#include <cmath>
#include <utility>

using namespace godot;

namespace {

// The sampler already produces engine-native (Y-up) transforms -- the same space Godot
// and the original engine use -- so a bone's parent-local pose maps DIRECTLY to a Godot
// Transform3D with no change-of-basis. (The mesh carries the (-x,y,z) handedness flip;
// the Skin's global-rest-inverse bind keeps mesh and bones consistent.) Our Quat is
// w-first {w,x,y,z}; Godot's Quaternion ctor is (x,y,z,w).
Transform3D bone_local_to_godot(const opennova::anim::Quat &q, const opennova::anim::Vec3 &p) {
	return Transform3D(Basis(Quaternion(q.x, q.y, q.z, q.w)), Vector3(p.x, p.y, p.z));
}

// --- Skeleton BIND pose from the .bad BadBone bind matrix (parent-local) ----------------
// Exact port of the proven oscarmike adm_import_plugin.cpp (build_bone_data_from_bad):
// rest.origin = bone.position (raw); rest.basis = row-major-Basis(bone_mat * parent_mat^-1),
// orthonormalized. The .3di mesh is skinned to THIS bind, so the Skeleton3D rest must match it
// (not a sampled clip frame) -- otherwise the skin deforms ~identity at idle but collapses under
// large motion. [orig: build_world_bone_matrices @0x40c770 bind layer.]
struct Mat3 {
	float m[3][3] = {};
};

Mat3 bad_to_mat3(const float rot[9]) {
	Mat3 o{};
	o.m[0][0] = rot[0]; o.m[0][1] = rot[1]; o.m[0][2] = rot[2];
	o.m[1][0] = rot[3]; o.m[1][1] = rot[4]; o.m[1][2] = rot[5];
	o.m[2][0] = rot[6]; o.m[2][1] = rot[7]; o.m[2][2] = rot[8];
	return o;
}

bool mat3_invert(const Mat3 &m, Mat3 &out) {
	const float a00 = m.m[0][0], a01 = m.m[0][1], a02 = m.m[0][2];
	const float a10 = m.m[1][0], a11 = m.m[1][1], a12 = m.m[1][2];
	const float a20 = m.m[2][0], a21 = m.m[2][1], a22 = m.m[2][2];
	const float b01 = a22 * a11 - a12 * a21;
	const float b11 = -a22 * a10 + a12 * a20;
	const float b21 = a21 * a10 - a11 * a20;
	const float det = a00 * b01 + a01 * b11 + a02 * b21;
	if (std::fabs(det) < 1e-9f) {
		return false;
	}
	const float inv_det = 1.0f / det;
	out.m[0][0] = b01 * inv_det;
	out.m[0][1] = (-a22 * a01 + a02 * a21) * inv_det;
	out.m[0][2] = (a12 * a01 - a02 * a11) * inv_det;
	out.m[1][0] = b11 * inv_det;
	out.m[1][1] = (a22 * a00 - a02 * a20) * inv_det;
	out.m[1][2] = (-a12 * a00 + a02 * a10) * inv_det;
	out.m[2][0] = b21 * inv_det;
	out.m[2][1] = (-a21 * a00 + a01 * a20) * inv_det;
	out.m[2][2] = (a11 * a00 - a01 * a10) * inv_det;
	return true;
}

Mat3 mat3_mul(const Mat3 &a, const Mat3 &b) {
	Mat3 o{};
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) {
			o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
		}
	}
	return o;
}

Basis mat3_to_basis(const Mat3 &m) {
	return Basis(
			Vector3(m.m[0][0], m.m[0][1], m.m[0][2]),
			Vector3(m.m[1][0], m.m[1][1], m.m[1][2]),
			Vector3(m.m[2][0], m.m[2][1], m.m[2][2]));
}

Transform3D bind_rest_from_bad(const opennova::anim::ClipBone &bone, const opennova::anim::ClipBone *parent) {
	Transform3D rest;
	rest.origin = Vector3(bone.rest_position[0], bone.rest_position[1], bone.rest_position[2]);
	const Mat3 bone_mat = bad_to_mat3(bone.rest_rotation);
	if (parent == nullptr) {
		rest.basis = mat3_to_basis(bone_mat);
	} else {
		const Mat3 parent_mat = bad_to_mat3(parent->rest_rotation);
		Mat3 parent_inv{};
		mat3_invert(parent_mat, parent_inv);
		rest.basis = mat3_to_basis(mat3_mul(bone_mat, parent_inv));
	}
	if (rest.basis.determinant() == 0.0f) {
		rest.basis = Basis();
	} else {
		rest.basis = rest.basis.orthonormalized();
	}
	return rest;
}

}  // namespace

const NovaSkeletalAnim::LoadedClip *NovaSkeletalAnim::find_clip(const String &p_key) const {
	for (const LoadedClip &c : clips_) {
		if (c.key == p_key) {
			return &c;
		}
	}
	return nullptr;
}

bool NovaSkeletalAnim::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name) {
	bones_.clear();
	bind_local_.clear();
	clips_.clear();
	loaded_ = false;
	last_error_ = String();
	adm_name_ = p_adm_name;

	if (p_resource_root.is_null()) {
		last_error_ = "Resource root is null";
		return false;
	}

	const PackedByteArray adm_bytes = p_resource_root->read_file(p_adm_name);
	if (adm_bytes.is_empty()) {
		last_error_ = "Animation definition not found: " + p_adm_name;
		return false;
	}

	AdmFile adm;
	if (adm_parse_buffer(reinterpret_cast<const char *>(adm_bytes.ptr()), static_cast<size_t>(adm_bytes.size()), &adm) != 0) {
		last_error_ = "Failed to parse animation definition: " + p_adm_name;
		return false;
	}

	// Resolve the reset/bind clip's .bad (key contains "reset", else the first non-empty value).
	// ALL clips of a model share this ONE skeleton's bone offsets + bind pose; each clip's own
	// .bad may carry different/zero bone positions, so sampling must use the shared origins.
	auto resolve_bad = [](const String &value) -> String {
		String bad_name = value;
		if (!bad_name.to_lower().ends_with(".bad")) {
			bad_name += ".bad";
		}
		return bad_name;
	};
	String reset_value;
	for (size_t i = 0; i < adm.count; ++i) {
		const String value = String(adm.entries[i].value);
		if (value.is_empty()) {
			continue;
		}
		if (reset_value.is_empty()) {
			reset_value = value;
		}
		if (String(adm.entries[i].key).to_lower().contains("reset")) {
			reset_value = value;
			break;
		}
	}

	// Pass 1: parse the reset .bad -> canonical bones, shared rest origins, and the bind pose.
	std::vector<opennova::anim::Vec3> shared_rest;
	{
		const PackedByteArray bytes = reset_value.is_empty()
				? PackedByteArray()
				: p_resource_root->read_file(resolve_bad(reset_value));
		BadFile bf;
		if (bytes.is_empty() || bad_parse_buffer(bytes.ptr(), static_cast<size_t>(bytes.size()), &bf) != 0) {
			adm_free(&adm);
			last_error_ = "Reset animation not found/parseable for: " + p_adm_name;
			return false;
		}
		const opennova::anim::Clip reset_clip = opennova::anim::sample_clip(bf);
		bad_free(&bf);
		bones_ = reset_clip.bones;
		shared_rest.resize(bones_.size());
		bind_local_.resize(bones_.size());
		for (size_t i = 0; i < bones_.size(); ++i) {
			shared_rest[i] = {bones_[i].rest_position[0], bones_[i].rest_position[1], bones_[i].rest_position[2]};
			const int parent = bones_[i].parent_index;
			const opennova::anim::ClipBone *p =
					(parent >= 0 && static_cast<size_t>(parent) < bones_.size()) ? &bones_[parent] : nullptr;
			bind_local_[i] = bind_rest_from_bad(bones_[i], p);
		}
	}

	// Pass 2: sample every clip against the SHARED skeleton rest origins (not each clip's own).
	for (size_t i = 0; i < adm.count; ++i) {
		const String key = String(adm.entries[i].key);
		const String value = String(adm.entries[i].value);
		if (value.is_empty()) {
			continue;
		}
		const PackedByteArray bad_bytes = p_resource_root->read_file(resolve_bad(value));
		if (bad_bytes.is_empty()) {
			continue;  // continue-on-failure (matches the DCC importer's behaviour)
		}
		BadFile bf;
		if (bad_parse_buffer(bad_bytes.ptr(), static_cast<size_t>(bad_bytes.size()), &bf) != 0) {
			continue;
		}
		LoadedClip lc;
		lc.key = key;
		lc.clip = opennova::anim::sample_clip(bf, shared_rest);
		bad_free(&bf);
		clips_.push_back(std::move(lc));
	}

	adm_free(&adm);

	if (clips_.empty()) {
		last_error_ = "No animation clips could be loaded for: " + p_adm_name;
		return false;
	}

	loaded_ = true;
	return true;
}

String NovaSkeletalAnim::slot_to_key(int p_slot) const {
	const char *key = opennova::world::body_anim_adm_key(p_slot);
	if (key[0] != '\0') {
		const String s(key);
		if (find_clip(s) != nullptr) {
			return s;
		}
	}
	// Fallbacks so a model whose .adm lacks the requested key still poses sensibly.
	if (find_clip("anim_idle") != nullptr) {
		return String("anim_idle");
	}
	if (find_clip("anim_reset") != nullptr) {
		return String("anim_reset");
	}
	return String();
}

PackedStringArray NovaSkeletalAnim::get_clip_keys() const {
	PackedStringArray out;
	for (const LoadedClip &c : clips_) {
		out.push_back(c.key);
	}
	return out;
}

Array NovaSkeletalAnim::get_skeleton_bones() const {
	Array out;
	for (size_t i = 0; i < bones_.size(); ++i) {
		Dictionary d;
		d["name"] = String(bones_[i].name.c_str());
		d["parent_index"] = bones_[i].parent_index;
		d["rest"] = (i < bind_local_.size()) ? bind_local_[i] : Transform3D();
		out.push_back(d);
	}
	return out;
}

int NovaSkeletalAnim::get_clip_frame_count(const String &p_key) const {
	const LoadedClip *c = find_clip(p_key);
	return c != nullptr ? static_cast<int>(c->clip.frame_count) : 0;
}

float NovaSkeletalAnim::get_clip_fps(const String &p_key) const {
	const LoadedClip *c = find_clip(p_key);
	return c != nullptr ? static_cast<float>(c->clip.fps) : 0.0f;
}

float NovaSkeletalAnim::get_clip_length(const String &p_key) const {
	const LoadedClip *c = find_clip(p_key);
	if (c == nullptr || c->clip.fps == 0 || c->clip.frame_count == 0) {
		return 0.0f;
	}
	return static_cast<float>(c->clip.frame_count) / static_cast<float>(c->clip.fps);
}

bool NovaSkeletalAnim::is_clip_looping(const String &p_key) const {
	const LoadedClip *c = find_clip(p_key);
	return c != nullptr && c->clip.loops();
}

Array NovaSkeletalAnim::eval_pose(const String &p_key, double p_playhead_seconds) const {
	Array out;
	const LoadedClip *lc = find_clip(p_key);
	if (lc == nullptr || lc->clip.frame_count == 0) {
		// Unknown / empty clip: fall back to the bind pose.
		for (const Transform3D &t : bind_local_) {
			out.push_back(t);
		}
		return out;
	}

	const opennova::anim::Clip &clip = lc->clip;
	const int frame_count = static_cast<int>(clip.frame_count);
	const double fps = clip.fps > 0 ? static_cast<double>(clip.fps) : 30.0;
	double frame_time = p_playhead_seconds * fps;

	int a = 0;
	int b = 0;
	double frac = 0.0;
	if (clip.loops() && frame_count > 1) {
		double m = std::fmod(frame_time, static_cast<double>(frame_count));
		if (m < 0.0) {
			m += static_cast<double>(frame_count);
		}
		a = static_cast<int>(std::floor(m));
		frac = m - a;
		b = (a + 1) % frame_count;
	} else if (frame_time <= 0.0) {
		a = b = 0;
	} else if (frame_time >= frame_count - 1) {
		a = b = frame_count - 1;
	} else {
		a = static_cast<int>(std::floor(frame_time));
		frac = frame_time - a;
		b = a + 1;
	}

	const std::vector<opennova::anim::BoneSample> &fa = clip.frames[a];
	const std::vector<opennova::anim::BoneSample> &fb = clip.frames[b];
	const size_t bone_count = clip.bones.size();
	for (size_t i = 0; i < bone_count; ++i) {
		const opennova::anim::BoneSample &sa = fa[i];
		const opennova::anim::BoneSample &sb = fb[i];
		const Quaternion qa(sa.local_rotation.x, sa.local_rotation.y, sa.local_rotation.z, sa.local_rotation.w);
		const Quaternion qb(sb.local_rotation.x, sb.local_rotation.y, sb.local_rotation.z, sb.local_rotation.w);
		const Quaternion q = qa.slerp(qb, static_cast<real_t>(frac));
		const Vector3 pa(sa.local_position.x, sa.local_position.y, sa.local_position.z);
		const Vector3 pb(sb.local_position.x, sb.local_position.y, sb.local_position.z);
		const Vector3 p = pa.lerp(pb, static_cast<real_t>(frac));
		out.push_back(Transform3D(Basis(q), p));
	}
	return out;
}

void NovaSkeletalAnim::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "adm_name"), &NovaSkeletalAnim::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaSkeletalAnim::is_loaded);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaSkeletalAnim::get_last_error);
	ClassDB::bind_method(D_METHOD("get_adm_name"), &NovaSkeletalAnim::get_adm_name);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &NovaSkeletalAnim::get_bone_count);
	ClassDB::bind_method(D_METHOD("get_clip_keys"), &NovaSkeletalAnim::get_clip_keys);
	ClassDB::bind_method(D_METHOD("get_skeleton_bones"), &NovaSkeletalAnim::get_skeleton_bones);
	ClassDB::bind_method(D_METHOD("slot_to_key", "slot"), &NovaSkeletalAnim::slot_to_key);
	ClassDB::bind_method(D_METHOD("has_clip", "key"), &NovaSkeletalAnim::has_clip);
	ClassDB::bind_method(D_METHOD("get_clip_frame_count", "key"), &NovaSkeletalAnim::get_clip_frame_count);
	ClassDB::bind_method(D_METHOD("get_clip_fps", "key"), &NovaSkeletalAnim::get_clip_fps);
	ClassDB::bind_method(D_METHOD("get_clip_length", "key"), &NovaSkeletalAnim::get_clip_length);
	ClassDB::bind_method(D_METHOD("is_clip_looping", "key"), &NovaSkeletalAnim::is_clip_looping);
	ClassDB::bind_method(D_METHOD("eval_pose", "key", "playhead_seconds"), &NovaSkeletalAnim::eval_pose);
}
