#include "model/clip_projector.h"

#include "model/model_frames.h"
#include "util/string_convert.h"

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <formats/bad/bad.h>
#include <formats/bad/bad_write.h>
#include <formats/threedi/threedi_scene_names.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace godot;
using namespace opennova::bad;
using namespace opennova::threedi;

namespace {

// The transform of `p_node` relative to `p_ancestor`, composed from the
// ancestor down (the model exporter's rule, so a clip's pivots are the
// model's).
Transform3D relative_transform(const Node3D *p_node, const Node *p_ancestor) {
	std::vector<const Node3D *> chain;
	const Node *cursor = p_node;
	while (cursor != nullptr && cursor != p_ancestor) {
		const Node3D *spatial = Object::cast_to<Node3D>(cursor);
		if (spatial != nullptr) chain.push_back(spatial);
		cursor = cursor->get_parent();
	}
	Transform3D out;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		out = out * (*it)->get_transform();
	}
	return out;
}

Skeleton3D *find_skeleton(Node *p_node) {
	for (int i = 0; i < p_node->get_child_count(); ++i) {
		Node *child = p_node->get_child(i);
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(child);
		if (skeleton != nullptr) return skeleton;
		skeleton = find_skeleton(child);
		if (skeleton != nullptr) return skeleton;
	}
	return nullptr;
}

// The presentation frame is the model frame mirrored on x; a rotation crosses
// by conjugation with that mirror (a proper rotation again).
Basis model_basis_from_presentation(const Basis &p_basis) {
	const Basis mirror(Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1));
	return mirror * p_basis * mirror;
}

Vector3 model_vec(const Vector3 &p_presentation) {
	return vec3_from_build(model_from_presentation(p_presentation));
}

struct Row {
	int bone = -1;          // Skeleton3D bone index
	int parent = -1;        // row index of the parent, -1 for the root
	Transform3D global_rest; // in model-root space (presentation frame)
	Vector3 rel;             // parent-relative pivot, presentation frame
};

} // namespace

void ClipProjector::_bind_methods() {
	ClassDB::bind_method(D_METHOD("project", "model_root", "animation", "spec", "fps", "ground_bone"),
			&ClipProjector::project);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ClipProjector::get_last_error);
}

Ref<ClipDocument> ClipProjector::project(Node3D *p_model_root, const Ref<Animation> &p_animation,
		const Ref<ClipSpec> &p_spec, int p_fps, int p_ground_bone) {
	last_error_ = "";
	if (p_model_root == nullptr) {
		last_error_ = "no model root";
		return Ref<ClipDocument>();
	}
	if (p_spec.is_null()) {
		last_error_ = "no clip spec";
		return Ref<ClipDocument>();
	}
	if (p_fps <= 0) {
		last_error_ = "the frame rate must be positive";
		return Ref<ClipDocument>();
	}
	Skeleton3D *skeleton = find_skeleton(p_model_root);
	if (skeleton == nullptr) {
		last_error_ = "no Skeleton3D under the model root";
		return Ref<ClipDocument>();
	}

	// The rows: BN## bones, contiguous from BN01 (the model exporter's rule).
	std::map<int, int> row_to_bone;
	for (int b = 0; b < skeleton->get_bone_count(); ++b) {
		int index = 0;
		if (!threedi_scene_parse_bone_name(opennova::to_std(skeleton->get_bone_name(b)), index)) {
			last_error_ = "bone '" + skeleton->get_bone_name(b) + "' is not named BN##";
			return Ref<ClipDocument>();
		}
		if (row_to_bone.count(index) != 0) {
			last_error_ = "two bones name BN" + String::num_int64(index + 1).pad_zeros(2);
			return Ref<ClipDocument>();
		}
		row_to_bone[index] = b;
	}
	if (row_to_bone.empty()) {
		last_error_ = "the skeleton has no bones";
		return Ref<ClipDocument>();
	}
	std::vector<Row> rows;
	std::vector<int> bone_to_row(static_cast<size_t>(skeleton->get_bone_count()), -1);
	for (const auto &entry : row_to_bone) {
		if (entry.first != static_cast<int>(rows.size())) {
			last_error_ = "bones are not contiguous from BN01 (missing BN" +
					String::num_int64(static_cast<int64_t>(rows.size()) + 1).pad_zeros(2) + ")";
			return Ref<ClipDocument>();
		}
		Row row;
		row.bone = entry.second;
		bone_to_row[static_cast<size_t>(entry.second)] = static_cast<int>(rows.size());
		rows.push_back(row);
	}
	const Transform3D skeleton_global = relative_transform(skeleton, p_model_root);
	for (Row &row : rows) {
		row.global_rest = skeleton_global * skeleton->get_bone_global_rest(row.bone);
		const int parent_bone = skeleton->get_bone_parent(row.bone);
		if (parent_bone < 0) {
			row.parent = -1;
			row.rel = row.global_rest.origin;
		} else {
			row.parent = bone_to_row[static_cast<size_t>(parent_bone)];
			const Transform3D parent_global = skeleton->get_bone_global_rest(parent_bone);
			row.rel = skeleton_global.basis.xform(parent_global.basis.xform(skeleton->get_bone_rest(row.bone).origin));
		}
	}
	const size_t row_count = rows.size();
	int ground_row = p_ground_bone;
	if (ground_row < 0 || static_cast<size_t>(ground_row) >= row_count) {
		ground_row = static_cast<int>(row_count) - 1;
	}

	// The tracks: rotation and position tracks keyed by the bone they name.
	std::vector<int> rotation_track(row_count, -1);
	std::vector<int> position_track(row_count, -1);
	uint32_t frame_count = 0;
	if (p_animation.is_valid()) {
		for (int t = 0; t < p_animation->get_track_count(); ++t) {
			const Animation::TrackType type = p_animation->track_get_type(t);
			if (type != Animation::TYPE_ROTATION_3D && type != Animation::TYPE_POSITION_3D) continue;
			const NodePath path = p_animation->track_get_path(t);
			if (path.get_subname_count() < 1) continue;
			const int bone = skeleton->find_bone(String(path.get_subname(path.get_subname_count() - 1)));
			if (bone < 0) {
				last_error_ = "track '" + String(path) + "' names a bone the rig does not have";
				return Ref<ClipDocument>();
			}
			const int row = bone_to_row[static_cast<size_t>(bone)];
			if (type == Animation::TYPE_ROTATION_3D) {
				rotation_track[static_cast<size_t>(row)] = t;
			} else {
				position_track[static_cast<size_t>(row)] = t;
			}
		}
		const double length = p_animation->get_length();
		frame_count = static_cast<uint32_t>(std::lround(length * p_fps));
		if (frame_count == 0) frame_count = 1;
	} else {
		frame_count = static_cast<uint32_t>(p_spec->get_hold_frames() > 0 ? p_spec->get_hold_frames() : 1);
	}
	const uint32_t key_count = frame_count + 1;

	// Sample every key tick: the posed rig, its model-space rotations relative
	// to the rest, and any departure from the pivots' forward kinematics.
	std::vector<std::vector<BadQuaternion>> channels(row_count, std::vector<BadQuaternion>(key_count));
	std::vector<std::vector<Vector3>> translations(key_count, std::vector<Vector3>(row_count));
	std::vector<float> bottoms(key_count, 0.0f);
	std::vector<float> tops(key_count, 0.0f);
	bool translated = false;
	std::vector<Transform3D> pose_global(static_cast<size_t>(skeleton->get_bone_count()));
	for (uint32_t k = 0; k < key_count; ++k) {
		double t = static_cast<double>(k) / p_fps;
		if (p_animation.is_valid() && t > p_animation->get_length()) t = p_animation->get_length();
		// Parent-local poses in skeleton order, then the chain down from the roots.
		for (int b = 0; b < skeleton->get_bone_count(); ++b) {
			const int row = bone_to_row[static_cast<size_t>(b)];
			const Transform3D rest = skeleton->get_bone_rest(b);
			Quaternion rotation = rest.basis.get_rotation_quaternion();
			Vector3 position = rest.origin;
			if (row >= 0 && rotation_track[static_cast<size_t>(row)] >= 0) {
				rotation = p_animation->rotation_track_interpolate(rotation_track[static_cast<size_t>(row)], t);
			}
			if (row >= 0 && position_track[static_cast<size_t>(row)] >= 0) {
				position = p_animation->position_track_interpolate(position_track[static_cast<size_t>(row)], t);
			}
			pose_global[static_cast<size_t>(b)] = Transform3D(Basis(rotation), position);
		}
		std::vector<bool> resolved(pose_global.size(), false);
		for (int b = 0; b < skeleton->get_bone_count(); ++b) {
			// Resolve the parent chain first (Godot does not order bones parent-first).
			std::vector<int> chain;
			int cursor = b;
			while (cursor >= 0 && !resolved[static_cast<size_t>(cursor)]) {
				chain.push_back(cursor);
				cursor = skeleton->get_bone_parent(cursor);
			}
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				const int parent = skeleton->get_bone_parent(*it);
				if (parent >= 0) {
					pose_global[static_cast<size_t>(*it)] =
							pose_global[static_cast<size_t>(parent)] * pose_global[static_cast<size_t>(*it)];
				}
				resolved[static_cast<size_t>(*it)] = true;
			}
		}
		std::vector<Vector3> fk_model(row_count);
		std::vector<Basis> rotation_model(row_count);
		float top = -1.0e30f;
		for (size_t r = 0; r < row_count; ++r) {
			const Row &row = rows[r];
			const Transform3D global = skeleton_global * pose_global[static_cast<size_t>(row.bone)];
			const Basis relative = global.basis * row.global_rest.basis.inverse();
			rotation_model[r] = model_basis_from_presentation(relative);
			const Quaternion q = rotation_model[r].get_rotation_quaternion().normalized();
			channels[r][k] = BadQuaternion{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z),
				static_cast<float>(q.w)};
			// The forward kinematics the runtime runs over the pivots: root at its
			// pivot, a child at the parent's position plus the parent's rotation
			// applied to its parent-relative pivot.
			const Vector3 rel_model = model_vec(row.rel);
			if (row.parent < 0) {
				fk_model[r] = rel_model;
			} else {
				fk_model[r] = fk_model[static_cast<size_t>(row.parent)] +
						rotation_model[static_cast<size_t>(row.parent)].xform(rel_model);
			}
			const Vector3 actual_model = model_vec(global.origin);
			translations[k][r] = actual_model - fk_model[r];
			if (translations[k][r].length() > 1.0e-4f) translated = true;
			if (static_cast<int>(r) != ground_row && actual_model.y > top) top = actual_model.y;
		}
		const float ground = model_vec(skeleton_global.xform(pose_global[static_cast<size_t>(rows[static_cast<size_t>(ground_row)].bone)].origin)).y;
		const float root_height = model_vec(skeleton_global.xform(pose_global[static_cast<size_t>(rows[0].bone)].origin)).y;
		bottoms[k] = p_spec->get_capsule_bottom() >= 0.0f ? p_spec->get_capsule_bottom() : root_height - ground;
		tops[k] = p_spec->get_capsule_top() >= 0.0f ? p_spec->get_capsule_top() : top - ground;
	}

	// The file: bones (identity bind, pivots as positions), channels, events,
	// translations when a bone left its pivots.
	std::vector<BadBone> bones(row_count);
	for (size_t r = 0; r < row_count; ++r) {
		BadBone &bone = bones[r];
		std::memset(&bone, 0, sizeof(bone));
		const std::string name = opennova::to_std(skeleton->get_bone_name(rows[r].bone));
		std::strncpy(bone.name, name.c_str(), sizeof(bone.name) - 1);
		bone.parent_index = rows[r].parent;
		bone.length = 0.0f;
		bone.position[0] = rows[r].rel.x;
		bone.position[1] = rows[r].rel.y;
		bone.position[2] = rows[r].rel.z;
		for (int i = 0; i < 9; ++i) bone.rotation[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	}
	std::vector<uint16_t> frame_lengths(key_count, 1);
	std::vector<BadChannel> bad_channels(row_count);
	for (size_t r = 0; r < row_count; ++r) {
		bad_channels[r].frame_count = key_count;
		bad_channels[r].frame_lengths = frame_lengths.data();
		bad_channels[r].rotations = channels[r].data();
	}
	std::vector<BadEvent> events(key_count);
	const PackedInt32Array triggers = p_spec->get_triggers();
	for (uint32_t k = 0; k < key_count; ++k) {
		BadEvent &event = events[k];
		event.velocity[0] = p_spec->get_lateral_speed() / static_cast<float>(p_fps);
		event.velocity[1] = p_spec->get_vertical_speed() / static_cast<float>(p_fps);
		event.velocity[2] = p_spec->get_forward_speed() / static_cast<float>(p_fps);
		event.bottom = bottoms[k];
		event.top = tops[k];
		event.trigger = static_cast<int>(k) < triggers.size() ? triggers[static_cast<int>(k)] : 0;
	}
	std::vector<float> translation_words;
	if (translated) {
		translation_words.resize(static_cast<size_t>(frame_count) * row_count * 3);
		for (uint32_t f = 0; f < frame_count; ++f) {
			for (size_t r = 0; r < row_count; ++r) {
				float *w = &translation_words[(static_cast<size_t>(f) * row_count + r) * 3];
				w[0] = translations[f][r].x;
				w[1] = translations[f][r].y;
				w[2] = translations[f][r].z;
			}
		}
	}
	BadFile file = {};
	file.version = 1;
	file.header_size = 80;
	file.fps = static_cast<uint32_t>(p_fps);
	file.frame_count = frame_count;
	file.flags = (p_spec->get_loop() ? 1u : 0u) | (translated ? 2u : 0u);
	file.bone_count = static_cast<uint32_t>(row_count);
	file.bones = bones.data();
	file.num_bones = row_count;
	file.channels = bad_channels.data();
	file.num_channels = row_count;
	file.events = events.data();
	file.num_events = key_count;
	if (translated) {
		file.translations = reinterpret_cast<float (*)[3]>(translation_words.data());
		file.num_translations = static_cast<size_t>(frame_count) * row_count;
	}
	std::vector<uint8_t> bytes;
	if (bad_write_buffer(&file, bytes) != 0) {
		last_error_ = "the clip cannot be written";
		return Ref<ClipDocument>();
	}
	PackedByteArray packed;
	packed.resize(static_cast<int64_t>(bytes.size()));
	std::memcpy(packed.ptrw(), bytes.data(), bytes.size());
	Ref<ClipDocument> document;
	document.instantiate();
	if (document->load_from_bytes(packed) != OK) {
		last_error_ = "the written clip does not parse: " + document->get_last_error();
		return Ref<ClipDocument>();
	}
	return document;
}
