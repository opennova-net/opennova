#include "object/nova_skeletal_anim.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <bad/bad.h>

#include <adm/adm.h>
#include <anim/aim_overlay.h> // the torso-bend overlay [orig: @0x4b1290]
#include <world/body_anim.h>

#include <cmath>
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

std::string NovaSkeletalAnim::fold_clip_key(const String &p_key) {
	const CharString utf8 = p_key.utf8();
	std::string out(utf8.get_data(), static_cast<size_t>(utf8.length()));
	for (char &ch : out) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = static_cast<char>(ch - 'A' + 'a');
		}
	}
	return out;
}

void NovaSkeletalAnim::rebuild_clip_index() {
	clip_index_.clear();
	for (size_t i = 0; i < clips_.size(); ++i) {
		clip_index_[fold_clip_key(clips_[i].key)].push_back(i);
	}
}

const NovaSkeletalAnim::LoadedClip *NovaSkeletalAnim::find_clip(const String &p_key) const {
	// Case-insensitive: JOTAC-era weapon.def ACTION rows author ANIM_WPN_* uppercase
	// while the .adm stores anim_wpn_* lowercase — an exact match starves the FSM
	// bake's clip lengths (every 'auto' delay collapsed to 0) and has_anim.
	// [orig: AnimMap_FindSlotByName @ 0x40cfa0 — stricmp]. Served from the
	// case-folded index: the present pass resolves clips per animated model per
	// render frame, so a linear nocasecmp scan here was a frame cost.
	const auto it = clip_index_.find(fold_clip_key(p_key));
	if (it == clip_index_.end() || it->second.empty()) {
		return nullptr;
	}
	return &clips_[it->second.front()];
}

const NovaSkeletalAnim::LoadedClip *NovaSkeletalAnim::find_clip_variant(
		const String &p_key, int p_variant) const {
	if (p_variant <= 0) {
		return find_clip(p_key);
	}
	// Variants registered under one key stay in .adm file order — the index keeps
	// the same-key run in insertion order, so the wrapped serve is unchanged.
	const auto it = clip_index_.find(fold_clip_key(p_key));
	if (it == clip_index_.end() || it->second.empty()) {
		return nullptr;
	}
	return &clips_[it->second[static_cast<size_t>(p_variant) % it->second.size()]];
}

bool NovaSkeletalAnim::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name,
		const PackedVector3Array &p_model_bone_origins, const PackedInt32Array &p_model_bone_parents) {
	bones_.clear();
	bind_local_.clear();
	clips_.clear();
	clip_index_.clear();  // indices dangle the moment clips_ is cleared
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

	// Read the reset/skeleton .bad + each listed clip .bad, then build via the shared core.
	const PackedByteArray reset_bytes = reset_value.is_empty()
			? PackedByteArray()
			: p_resource_root->read_file(resolve_bad(reset_value));
	if (reset_bytes.is_empty()) {
		adm_free(&adm);
		last_error_ = "Reset animation not found for: " + p_adm_name;
		return false;
	}
	std::vector<std::pair<String, PackedByteArray>> clip_bads;
	for (size_t i = 0; i < adm.count; ++i) {
		const String key = String(adm.entries[i].key);
		// EVERY quoted token on the row is a VARIANT of the same slot, registered in
		// file order (the authored duplication is the rotation weighting — REVVY's
		// reload "m4_1r" "m4_1r" "m4_1r2" plays r twice per r2 cycle)
		// [orig: AnimMap_ParseConfigLine @ 0x40cb60 loops the tokens;
		//  AnimMap_RegisterBoneNode @ 0x40c2d0 links each into the slot ring].
		const size_t vcount = adm.entries[i].value_count > 0 ? adm.entries[i].value_count : 1;
		for (size_t v = 0; v < vcount; ++v) {
			const String value = v < adm.entries[i].value_count
					? String(adm.entries[i].values[v])
					: String(adm.entries[i].value);
			if (value.is_empty()) {
				continue;
			}
			const PackedByteArray bad_bytes = p_resource_root->read_file(resolve_bad(value));
			if (bad_bytes.is_empty()) {
				continue;  // continue-on-failure (matches the DCC importer's behaviour)
			}
			clip_bads.emplace_back(key, bad_bytes);
		}
	}
	adm_free(&adm);

	if (!build_from_bad_bytes(reset_bytes, clip_bads, to_model_origins(p_model_bone_origins),
			to_model_parents(p_model_bone_parents))) {
		// Carry the .adm name into the core's generic error for context.
		if (!last_error_.is_empty() && last_error_.find(p_adm_name) < 0) {
			last_error_ += " for: " + p_adm_name;
		}
		return false;
	}
	return true;
}

bool NovaSkeletalAnim::load_from_bad_files(const Ref<NovaResourceRoot> &p_resource_root,
		const String &p_skeleton_bad, const Dictionary &p_key_to_bad,
		const PackedVector3Array &p_model_bone_origins, const PackedInt32Array &p_model_bone_parents) {
	bones_.clear();
	bind_local_.clear();
	clips_.clear();
	clip_index_.clear();  // indices dangle the moment clips_ is cleared
	loaded_ = false;
	last_error_ = String();
	adm_name_ = p_skeleton_bad;  // diagnostic label (there is no .adm on this path)

	if (p_resource_root.is_null()) {
		last_error_ = "Resource root is null";
		return false;
	}

	const PackedByteArray reset_bytes = p_resource_root->read_file(p_skeleton_bad);
	if (reset_bytes.is_empty()) {
		last_error_ = "Skeleton .bad not found: " + p_skeleton_bad;
		return false;
	}

	std::vector<std::pair<String, PackedByteArray>> clip_bads;
	const Array keys = p_key_to_bad.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		const String bad_name = p_key_to_bad[keys[i]];
		if (key.is_empty() || bad_name.is_empty()) {
			continue;
		}
		const PackedByteArray bytes = p_resource_root->read_file(bad_name);
		if (bytes.is_empty()) {
			continue;  // continue-on-missing-clip (matches the .adm path)
		}
		clip_bads.emplace_back(key, bytes);
	}

	return build_from_bad_bytes(reset_bytes, clip_bads, to_model_origins(p_model_bone_origins),
			to_model_parents(p_model_bone_parents));
}

bool NovaSkeletalAnim::build_from_bad_bytes(const PackedByteArray &p_reset_bytes,
		const std::vector<std::pair<String, PackedByteArray>> &p_clip_bads,
		const std::vector<opennova::anim::Vec3> &p_model_origins,
		const std::vector<int> &p_model_parents) {
	// Pass 1: parse the reset/skeleton .bad -> canonical bones, shared rest origins, bind pose.
	// ALL clips share this ONE skeleton's bone offsets + bind pose; each clip's own .bad may carry
	// different/zero bone positions, so sampling must use the shared origins.
	//
	// Model-table mode (origins + parents paired): the .3di model's bone table defines the rig --
	// count and hierarchy from the model rows, and the skeleton's rest POSITIONS reconstructed
	// from the model pivots + the reset .bad's bind rotations via the corpus-exact export
	// relation (anim_sample.h positions_from_model; docs/net/novaworld-net-re.md section 5.40).
	// The .bad's own bone count/parents/positions are never read -- BadBone.position is a lossy
	// export (12 of 43 JO viewmodel rigs ship zeroed/stale values; retail renders them all
	// because its runtime rig reads the model table [orig: BoneAnim_BuildWorldMatrices @0x40c400
	// bounds the FK by modelDef+52 and walks the modelDef+56 rows]). The clips then run the SAME
	// rest-carrying composition as every other rig (channels absolute over the bind-rotation
	// rests), which is the proven-equivalent factorization of the original's composed builders:
	// on rigs whose shipped positions are healthy this path is bit-for-bit the .bad-driven one.
	// A rig whose .bad and model disagree in bone count (AKM_1st: 46 bones, 45 parts) sizes from
	// the model, extra channels never sampled, extra model rows taking bone 0's composed matrix
	// [orig: the padding loop @0x40c5a1].
	const bool model_table =
			!p_model_parents.empty() && p_model_parents.size() == p_model_origins.size();
	if (!p_model_parents.empty() && !model_table) {
		UtilityFunctions::push_warning(vformat("NovaSkeletalAnim: %s: model bone parents size %d != origins size %d; model table ignored",
				adm_name_, static_cast<int>(p_model_parents.size()), static_cast<int>(p_model_origins.size())));
	}
	std::vector<opennova::anim::Vec3> shared_rest;
	std::vector<int> rig_parents = model_table ? p_model_parents : std::vector<int>{};
	BadFile skeleton_bf = {};
	{
		if (p_reset_bytes.is_empty() ||
				bad_parse_buffer(p_reset_bytes.ptr(), static_cast<size_t>(p_reset_bytes.size()), &skeleton_bf) != 0) {
			last_error_ = "Reset/skeleton animation not parseable";
			return false;
		}
		// Model-table mode: rebuild the skeleton's rest positions from data that never rots --
		// the model pivots and the reset .bad's bind rotations.
		const std::vector<opennova::anim::Vec3> table_positions = model_table
				? opennova::anim::positions_from_model(skeleton_bf, p_model_parents, p_model_origins)
				: std::vector<opennova::anim::Vec3>{};
		const opennova::anim::Clip reset_clip = opennova::anim::sample_clip(
				skeleton_bf, table_positions,
				/*model_bind=*/false, nullptr, rig_parents);
		bones_ = reset_clip.bones;
		// Legacy positional override (no parents supplied): when the model supplies per-bone bind
		// positions (one per bone), they OVERRIDE the reset .bad's bone positions -- the .bad's
		// BadBone.position is a lossy export (~half the corpus triplicates X into all 3 slots,
		// destroying Y/Z), while the .3di model carries the real pivots (parent-relative,
		// model-frame). In model-table mode sample_clip already sourced count/hierarchy from the
		// model and positions from the reconstruction above.
		const bool use_model = !model_table && p_model_origins.size() == bones_.size();
		if (!model_table && !p_model_origins.empty() && !use_model) {
			UtilityFunctions::push_warning(vformat("NovaSkeletalAnim: %s: model bone origins size %d != bone count %d; falling back to .bad positions",
					adm_name_, static_cast<int>(p_model_origins.size()), static_cast<int>(bones_.size())));
		}
		shared_rest.resize(bones_.size());
		bind_local_.resize(bones_.size());
		for (size_t i = 0; i < bones_.size(); ++i) {
			if (use_model) {
				bones_[i].rest_position[0] = p_model_origins[i].x;
				bones_[i].rest_position[1] = p_model_origins[i].y;
				bones_[i].rest_position[2] = p_model_origins[i].z;
			}
			// Model-table rows past the .bad's records carry no name; Skeleton3D needs unique
			// non-empty names, so synthesize stable ones.
			if (bones_[i].name.empty()) {
				bones_[i].name = "MDL" + std::to_string(i);
			}
			shared_rest[i] = {bones_[i].rest_position[0], bones_[i].rest_position[1], bones_[i].rest_position[2]};
			const int parent = bones_[i].parent_index;
			const opennova::anim::ClipBone *p =
					(parent >= 0 && static_cast<size_t>(parent) < bones_.size()) ? &bones_[parent] : nullptr;
			bind_local_[i] = bind_rest_from_bad(bones_[i], p);
		}
	}

	// Pass 2: sample every clip against the SHARED skeleton rest origins (not each clip's own) --
	// each clip's .bad may carry different/zero bone positions [orig: the rig-wide skeleton is
	// the .adm slot-0 .bad, pinned once at entity registration -- AnimMap_RegisterEntity
	// @0x40bb60; clip switches never rebuild it, AnimMap_PlayAnimBySlot @0x40bda0].
	for (const std::pair<String, PackedByteArray> &kv : p_clip_bads) {
		const PackedByteArray &bad_bytes = kv.second;
		BadFile bf;
		if (bad_bytes.is_empty() ||
				bad_parse_buffer(bad_bytes.ptr(), static_cast<size_t>(bad_bytes.size()), &bf) != 0) {
			continue;
		}
		LoadedClip lc;
		lc.key = kv.first;
		lc.clip = opennova::anim::sample_clip(bf, shared_rest, /*model_bind=*/false, nullptr, rig_parents);
		bad_free(&bf);
		clips_.push_back(std::move(lc));
	}
	bad_free(&skeleton_bf);

	if (clips_.empty()) {
		last_error_ = "No animation clips could be loaded";
		rebuild_clip_index();
		return false;
	}

	rebuild_clip_index();
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

int NovaSkeletalAnim::get_clip_variant_count(const String &p_key) const {
	int count = 0;
	for (const LoadedClip &c : clips_) {
		if (c.key.nocasecmp_to(p_key) == 0) {
			++count;
		}
	}
	return count;
}

PackedFloat32Array NovaSkeletalAnim::get_clip_variant_lengths(const String &p_key) const {
	PackedFloat32Array out;
	for (const LoadedClip &c : clips_) {
		if (c.key.nocasecmp_to(p_key) == 0) {
			out.push_back(c.clip.fps > 0 && c.clip.frame_count > 0
					? static_cast<float>(c.clip.frame_count) / static_cast<float>(c.clip.fps)
					: 0.0f);
		}
	}
	return out;
}

int NovaSkeletalAnim::get_clip_frame_count(const String &p_key, int p_variant) const {
	const LoadedClip *c = find_clip_variant(p_key, p_variant);
	return c != nullptr ? static_cast<int>(c->clip.frame_count) : 0;
}

float NovaSkeletalAnim::get_clip_fps(const String &p_key, int p_variant) const {
	const LoadedClip *c = find_clip_variant(p_key, p_variant);
	return c != nullptr ? static_cast<float>(c->clip.fps) : 0.0f;
}

float NovaSkeletalAnim::get_clip_length(const String &p_key, int p_variant) const {
	const LoadedClip *c = find_clip_variant(p_key, p_variant);
	if (c == nullptr || c->clip.fps == 0 || c->clip.frame_count == 0) {
		return 0.0f;
	}
	return static_cast<float>(c->clip.frame_count) / static_cast<float>(c->clip.fps);
}

bool NovaSkeletalAnim::is_clip_looping(const String &p_key, int p_variant) const {
	const LoadedClip *c = find_clip_variant(p_key, p_variant);
	return c != nullptr && c->clip.loops();
}

Array NovaSkeletalAnim::eval_pose(const String &p_key, double p_playhead_seconds,
		int p_variant) const {
	Array out;
	const LoadedClip *lc = find_clip_variant(p_key, p_variant);
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
	// The pose table holds frame_count + 1 keys (the header counts INTERVALS; the
	// sampler bakes every channel key). Loops cycle the frame_count interval windows
	// (the seam key ~= key 0; the original's loop-wrap window is unwalked); one-shots
	// play every window and HOLD the true final key — a one-frame clip is one full
	// window of motion, not a static pose. [orig: BoneAnim_FindKeyframeAtTime
	// @0x410220 — hold-last past the summed durations]
	const int last_pose = static_cast<int>(clip.frames.size()) - 1;
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
	} else if (frame_time >= last_pose) {
		a = b = last_pose;
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

Array NovaSkeletalAnim::eval_pose_blended(const String &p_source_key,
		double p_source_playhead_seconds, const String &p_target_key,
		double p_target_playhead_seconds, float p_weight) const {
	String source_key = p_source_key;
	String target_key = p_target_key;
	const String reset_key("anim_reset");
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
		return eval_pose(target_key, p_target_playhead_seconds);
	}
	if (!target_valid) {
		return eval_pose(source_key, p_source_playhead_seconds);
	}
	const float weight = CLAMP(p_weight, 0.0f, 1.0f);
	if (weight <= 0.0f) {
		return eval_pose(source_key, p_source_playhead_seconds);
	}
	if (weight >= 1.0f) {
		return eval_pose(target_key, p_target_playhead_seconds);
	}

	// Semantic states can map to the same BAD (including missing states that both
	// bind RESET) while retaining independent channel playheads. They must still
	// blend; key equality alone is not a valid single-sample shortcut.
	const Array source = eval_pose(source_key, p_source_playhead_seconds);
	const Array target = eval_pose(target_key, p_target_playhead_seconds);
	if (source.size() != target.size()) {
		return target;
	}
	Array out;
	out.resize(target.size());
	for (int i = 0; i < target.size(); ++i) {
		const Transform3D a = source[i];
		const Transform3D b = target[i];
		const Quaternion rotation =
				a.basis.get_rotation_quaternion().slerp(
						b.basis.get_rotation_quaternion(), weight);
		const Vector3 scale =
				a.basis.get_scale().lerp(b.basis.get_scale(), weight);
		out[i] = Transform3D(Basis(rotation).scaled(scale),
				a.origin.lerp(b.origin, weight));
	}
	return out;
}

PackedInt32Array NovaSkeletalAnim::get_overlay_classes() const {
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(bones_.size()));
	for (size_t i = 0; i < bones_.size(); ++i) {
		int cls = opennova::anim::kOverlayBody;
		// "BN01 Hips" -> bone index 0. The model bone order IS the BN order (§14.2), but
		// parsing the tag keeps husk/accessory variants correct without positional trust.
		const std::string &n = bones_[i].name;
		if (n.size() >= 4 && (n[0] == 'B' || n[0] == 'b') && (n[1] == 'N' || n[1] == 'n') &&
				n[2] >= '0' && n[2] <= '9' && n[3] >= '0' && n[3] <= '9') {
			const int bn = (n[2] - '0') * 10 + (n[3] - '0');
			if (bn >= 1 && bn <= 19) {
				cls = opennova::anim::kOverlayClassByBoneIndex[bn - 1];
			}
		}
		out[static_cast<int64_t>(i)] = cls;
	}
	return out;
}

void NovaSkeletalAnim::splice_weapon_channel(Array &p_pose, const String &p_wpn_key,
		double p_wpn_playhead_seconds) const {
	// Hard override of the mask bones' WORLD rotations with the weapon channel's clip at
	// its own playhead, then re-localize the complete mixed hierarchy. The primary pose
	// keeps every local origin (the shared skeleton/model pivots own translation).
	// [orig: mask @0x4b14db, second AnimChannel_ComputeBoneMatrices @0x4b16a7;
	// world-wac-ai-re.md §14.8.6]
	if (p_wpn_key.is_empty() || find_clip(p_wpn_key) == nullptr) {
		return;
	}
	const Array wpose = eval_pose(p_wpn_key, p_wpn_playhead_seconds);
	const int n = static_cast<int>(p_pose.size());
	if (wpose.size() != n || static_cast<size_t>(n) != bones_.size()) {
		return;
	}
	std::vector<int> parents(static_cast<size_t>(n));
	std::vector<uint8_t> mask(static_cast<size_t>(n), 0);
	std::vector<opennova::anim::Quat> primary_rot(static_cast<size_t>(n));
	std::vector<opennova::anim::Quat> weapon_rot(static_cast<size_t>(n));
	bool any_masked = false;
	for (int i = 0; i < n; ++i) {
		const Transform3D base = p_pose[i];
		const Transform3D w = wpose[i];
		const Quaternion bq = base.basis.get_rotation_quaternion();
		const Quaternion wq = w.basis.get_rotation_quaternion();
		parents[static_cast<size_t>(i)] = bones_[static_cast<size_t>(i)].parent_index;
		primary_rot[static_cast<size_t>(i)] = {
			static_cast<float>(bq.w), static_cast<float>(bq.x),
			static_cast<float>(bq.y), static_cast<float>(bq.z)
		};
		weapon_rot[static_cast<size_t>(i)] = {
			static_cast<float>(wq.w), static_cast<float>(wq.x),
			static_cast<float>(wq.y), static_cast<float>(wq.z)
		};
		// BN## tag -> model bone index, the same parse get_overlay_classes trusts.
		const std::string &bn = bones_[static_cast<size_t>(i)].name;
		if (bn.size() < 4 || (bn[0] != 'B' && bn[0] != 'b') || (bn[1] != 'N' && bn[1] != 'n') ||
				bn[2] < '0' || bn[2] > '9' || bn[3] < '0' || bn[3] > '9') {
			continue;
		}
		const int model_index = (bn[2] - '0') * 10 + (bn[3] - '0') - 1;
		if (opennova::anim::weapon_channel_masks_bone(model_index)) {
			mask[static_cast<size_t>(i)] = 1;
			any_masked = true;
		}
	}
	if (!any_masked) {
		return;
	}
	opennova::anim::splice_weapon_channel_rotations(
			parents, mask.data(), weapon_rot, primary_rot);
	for (int i = 0; i < n; ++i) {
		const Transform3D base = p_pose[i];
		const opennova::anim::Quat &q = primary_rot[static_cast<size_t>(i)];
		p_pose[i] = Transform3D(Basis(Quaternion(q.x, q.y, q.z, q.w)), base.origin);
	}
}

Array NovaSkeletalAnim::eval_pose_overlay(const String &p_key, double p_playhead_seconds,
		const PackedInt32Array &p_classes, const Array &p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand) const {
	return apply_pose_overlay(eval_pose(p_key, p_playhead_seconds),
			p_classes, p_deltas, p_wpn_key, p_wpn_playhead_seconds,
			p_collapse_right_hand);
}

Array NovaSkeletalAnim::eval_pose_blended_overlay(
		const String &p_source_key, double p_source_playhead_seconds,
		const String &p_target_key, double p_target_playhead_seconds,
		float p_weight, const PackedInt32Array &p_classes,
		const Array &p_deltas, const String &p_wpn_key,
		double p_wpn_playhead_seconds, bool p_collapse_right_hand) const {
	return apply_pose_overlay(eval_pose_blended(
					p_source_key, p_source_playhead_seconds,
					p_target_key, p_target_playhead_seconds, p_weight),
			p_classes, p_deltas, p_wpn_key, p_wpn_playhead_seconds,
			p_collapse_right_hand);
}

Array NovaSkeletalAnim::apply_pose_overlay(Array pose,
		const PackedInt32Array &p_classes, const Array &p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand) const {
	// The witnessed order: primary sample -> weapon-channel mask override -> the aim
	// overlay multiplies ON TOP of the composed pose [orig: @0x4b14a7..@0x4b16a7 run
	// before the per-bone overlay loop; world-wac-ai-re.md §14.8.6].
	splice_weapon_channel(pose, p_wpn_key, p_wpn_playhead_seconds);
	const int n = static_cast<int>(pose.size());
	if (n == 0 || static_cast<size_t>(n) != bones_.size() || p_classes.size() < n ||
			p_deltas.size() < static_cast<int>(opennova::anim::kOverlayClassCount)) {
		apply_right_hand_local_collapse(pose, p_collapse_right_hand);
		return pose;
	}

	opennova::anim::Quat deltas[opennova::anim::kOverlayClassCount];
	for (int c = 0; c < static_cast<int>(opennova::anim::kOverlayClassCount); ++c) {
		const Basis b = p_deltas[c];
		const Quaternion q = b.get_rotation_quaternion();
		deltas[c] = { static_cast<float>(q.w), static_cast<float>(q.x),
			static_cast<float>(q.y), static_cast<float>(q.z) };
	}
	std::vector<int> parents(bones_.size());
	std::vector<uint8_t> classes(bones_.size());
	std::vector<opennova::anim::Quat> rots(bones_.size());
	for (int i = 0; i < n; ++i) {
		parents[static_cast<size_t>(i)] = bones_[static_cast<size_t>(i)].parent_index;
		const int c = p_classes[i];
		classes[static_cast<size_t>(i)] = static_cast<uint8_t>(
				(c >= 0 && c < static_cast<int>(opennova::anim::kOverlayClassCount)) ? c : 0);
		const Transform3D t = pose[i];
		const Quaternion q = t.basis.get_rotation_quaternion();
		rots[static_cast<size_t>(i)] = { static_cast<float>(q.w), static_cast<float>(q.x),
			static_cast<float>(q.y), static_cast<float>(q.z) };
	}
	opennova::anim::apply_aim_overlay(parents, deltas, classes.data(), rots);
	for (int i = 0; i < n; ++i) {
		const Transform3D t = pose[i];
		const opennova::anim::Quat &q = rots[static_cast<size_t>(i)];
		pose[i] = Transform3D(Basis(Quaternion(q.x, q.y, q.z, q.w)), t.origin);
	}
	// Retail's final special row clips BN17 R Hand after channel composition,
	// overlay, and parent-pivot re-anchor. The personal weapon is baked into the
	// character mesh and weighted to model bone 16. The portable pose preserves
	// BN17's animated joint origin while zeroing its basis; the collision consumer
	// applies its witnessed final-row representation separately.
	// [orig: Entity_BuildBoneTransformMatrices @0x4b1290 special row;
	// world-wac-ai-re.md section 14.1.5]
	apply_right_hand_local_collapse(pose, p_collapse_right_hand);
	return pose;
}

void NovaSkeletalAnim::pose_skeleton(Skeleton3D *p_skeleton, const String &p_key,
		double p_playhead_seconds, int p_variant,
		const PackedInt32Array &p_classes, const Array &p_deltas,
		const String &p_wpn_key, double p_wpn_playhead_seconds,
		bool p_collapse_right_hand) const {
	// Branch mirror of NovaObjectModel.advance_body_animation: overlay inputs
	// present -> the composed overlay pose, else the plain clip pose.
	Array pose;
	if (!p_deltas.is_empty() && !p_classes.is_empty()) {
		pose = eval_pose_overlay(p_key, p_playhead_seconds, p_classes, p_deltas,
				p_wpn_key, p_wpn_playhead_seconds, p_collapse_right_hand);
	} else {
		pose = eval_pose(p_key, p_playhead_seconds, p_variant);
	}
	write_pose_to_skeleton(p_skeleton, pose, p_collapse_right_hand);
}

void NovaSkeletalAnim::pose_skeleton_blended(Skeleton3D *p_skeleton,
		const String &p_source_key, double p_source_playhead_seconds,
		const String &p_target_key, double p_target_playhead_seconds,
		float p_weight, const PackedInt32Array &p_classes,
		const Array &p_deltas, const String &p_wpn_key,
		double p_wpn_playhead_seconds, bool p_collapse_right_hand) const {
	Array pose;
	if (!p_deltas.is_empty() && !p_classes.is_empty()) {
		pose = eval_pose_blended_overlay(
				p_source_key, p_source_playhead_seconds,
				p_target_key, p_target_playhead_seconds, p_weight,
				p_classes, p_deltas, p_wpn_key,
				p_wpn_playhead_seconds, p_collapse_right_hand);
	} else {
		pose = eval_pose_blended(
				p_source_key, p_source_playhead_seconds,
				p_target_key, p_target_playhead_seconds, p_weight);
	}
	write_pose_to_skeleton(p_skeleton, pose, p_collapse_right_hand);
}

void NovaSkeletalAnim::write_pose_to_skeleton(Skeleton3D *p_skeleton,
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

void NovaSkeletalAnim::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "adm_name", "model_bone_origins", "model_bone_parents"), &NovaSkeletalAnim::load_from_resource_root, DEFVAL(PackedVector3Array()), DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("load_from_bad_files", "resource_root", "skeleton_bad", "key_to_bad", "model_bone_origins", "model_bone_parents"), &NovaSkeletalAnim::load_from_bad_files, DEFVAL(PackedVector3Array()), DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaSkeletalAnim::is_loaded);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaSkeletalAnim::get_last_error);
	ClassDB::bind_method(D_METHOD("get_adm_name"), &NovaSkeletalAnim::get_adm_name);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &NovaSkeletalAnim::get_bone_count);
	ClassDB::bind_method(D_METHOD("get_clip_keys"), &NovaSkeletalAnim::get_clip_keys);
	ClassDB::bind_method(D_METHOD("get_skeleton_bones"), &NovaSkeletalAnim::get_skeleton_bones);
	ClassDB::bind_method(D_METHOD("slot_to_key", "slot"), &NovaSkeletalAnim::slot_to_key);
	ClassDB::bind_method(D_METHOD("has_clip", "key"), &NovaSkeletalAnim::has_clip);
	ClassDB::bind_method(D_METHOD("get_clip_variant_count", "key"), &NovaSkeletalAnim::get_clip_variant_count);
	ClassDB::bind_method(D_METHOD("get_clip_variant_lengths", "key"), &NovaSkeletalAnim::get_clip_variant_lengths);
	ClassDB::bind_method(D_METHOD("get_clip_frame_count", "key", "variant"), &NovaSkeletalAnim::get_clip_frame_count, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_clip_fps", "key", "variant"), &NovaSkeletalAnim::get_clip_fps, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_clip_length", "key", "variant"), &NovaSkeletalAnim::get_clip_length, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("is_clip_looping", "key", "variant"), &NovaSkeletalAnim::is_clip_looping, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("eval_pose", "key", "playhead_seconds", "variant"), &NovaSkeletalAnim::eval_pose, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("eval_pose_blended", "source_key", "source_playhead_seconds", "target_key", "target_playhead_seconds", "weight"), &NovaSkeletalAnim::eval_pose_blended);
	ClassDB::bind_method(D_METHOD("get_overlay_classes"), &NovaSkeletalAnim::get_overlay_classes);
	ClassDB::bind_method(D_METHOD("eval_pose_overlay", "key", "playhead_seconds", "classes", "deltas", "wpn_key", "wpn_playhead_seconds", "collapse_right_hand"), &NovaSkeletalAnim::eval_pose_overlay, DEFVAL(String()), DEFVAL(0.0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("eval_pose_blended_overlay", "source_key", "source_playhead_seconds", "target_key", "target_playhead_seconds", "weight", "classes", "deltas", "wpn_key", "wpn_playhead_seconds", "collapse_right_hand"), &NovaSkeletalAnim::eval_pose_blended_overlay, DEFVAL(String()), DEFVAL(0.0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("pose_skeleton", "skeleton", "key", "playhead_seconds", "variant", "classes", "deltas", "wpn_key", "wpn_playhead_seconds", "collapse_right_hand"), &NovaSkeletalAnim::pose_skeleton, DEFVAL(String()), DEFVAL(0.0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("pose_skeleton_blended", "skeleton", "source_key", "source_playhead_seconds", "target_key", "target_playhead_seconds", "weight", "classes", "deltas", "wpn_key", "wpn_playhead_seconds", "collapse_right_hand"), &NovaSkeletalAnim::pose_skeleton_blended, DEFVAL(String()), DEFVAL(0.0), DEFVAL(false));
}
