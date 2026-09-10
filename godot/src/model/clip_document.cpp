#include "model/clip_document.h"

#include "util/string_convert.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <formats/bad/bad_write.h>

#include <string>
#include <vector>

using namespace godot;
using namespace opennova::bad;

namespace {

std::string native_path(const String &p_path) {
	String global = p_path;
	if (ProjectSettings::get_singleton() != nullptr) {
		global = ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	return opennova::to_std(global);
}

} // namespace

void ClipDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &ClipDocument::load_from_path);
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &ClipDocument::load_from_bytes);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &ClipDocument::save_to_path);
	ClassDB::bind_method(D_METHOD("to_bytes"), &ClipDocument::to_bytes);
	ClassDB::bind_method(D_METHOD("is_loaded"), &ClipDocument::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ClipDocument::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ClipDocument::get_last_error);
	ClassDB::bind_method(D_METHOD("get_fps"), &ClipDocument::get_fps);
	ClassDB::bind_method(D_METHOD("get_frame_count"), &ClipDocument::get_frame_count);
	ClassDB::bind_method(D_METHOD("get_flags"), &ClipDocument::get_flags);
	ClassDB::bind_method(D_METHOD("is_loop"), &ClipDocument::is_loop);
	ClassDB::bind_method(D_METHOD("has_translations"), &ClipDocument::has_translations);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &ClipDocument::get_bone_count);
	ClassDB::bind_method(D_METHOD("get_bone_name", "bone"), &ClipDocument::get_bone_name);
	ClassDB::bind_method(D_METHOD("get_bone_parent", "bone"), &ClipDocument::get_bone_parent);
	ClassDB::bind_method(D_METHOD("get_bone_position", "bone"), &ClipDocument::get_bone_position);
	ClassDB::bind_method(D_METHOD("get_channel_key_count", "bone"), &ClipDocument::get_channel_key_count);
	ClassDB::bind_method(D_METHOD("get_channel_rotation", "bone", "key"), &ClipDocument::get_channel_rotation);
	ClassDB::bind_method(D_METHOD("get_channel_frame_length", "bone", "key"),
			&ClipDocument::get_channel_frame_length);
	ClassDB::bind_method(D_METHOD("get_event_count"), &ClipDocument::get_event_count);
	ClassDB::bind_method(D_METHOD("get_event_velocity", "key"), &ClipDocument::get_event_velocity);
	ClassDB::bind_method(D_METHOD("get_event_bottom", "key"), &ClipDocument::get_event_bottom);
	ClassDB::bind_method(D_METHOD("get_event_top", "key"), &ClipDocument::get_event_top);
	ClassDB::bind_method(D_METHOD("get_event_trigger", "key"), &ClipDocument::get_event_trigger);
	ClassDB::bind_method(D_METHOD("get_translation", "frame", "bone"), &ClipDocument::get_translation);
}

ClipDocument::~ClipDocument() {
	_clear();
}

void ClipDocument::_clear() {
	if (loaded_) {
		bad_free(&file_);
	}
	file_ = {};
	loaded_ = false;
}

Error ClipDocument::load_from_path(const String &p_path) {
	last_error_ = "";
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(p_path);
	if (bytes.is_empty()) {
		last_error_ = "cannot read " + p_path;
		return ERR_FILE_CANT_READ;
	}
	const Error err = load_from_bytes(bytes);
	if (err == OK) {
		source_path_ = p_path;
	}
	return err;
}

Error ClipDocument::load_from_bytes(const PackedByteArray &p_bytes) {
	last_error_ = "";
	_clear();
	source_path_ = "";
	if (p_bytes.is_empty()) {
		last_error_ = "no bytes";
		return ERR_INVALID_DATA;
	}
	if (bad_parse_buffer(p_bytes.ptr(), static_cast<size_t>(p_bytes.size()), &file_) != 0) {
		file_ = {};
		last_error_ = "not a .bad clip";
		return ERR_FILE_CORRUPT;
	}
	loaded_ = true;
	return OK;
}

Error ClipDocument::save_to_path(const String &p_path) {
	last_error_ = "";
	if (!loaded_) {
		last_error_ = "nothing loaded";
		return ERR_UNCONFIGURED;
	}
	if (bad_write(native_path(p_path).c_str(), &file_) != 0) {
		last_error_ = "cannot write " + p_path;
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

PackedByteArray ClipDocument::to_bytes() {
	last_error_ = "";
	PackedByteArray out;
	if (!loaded_) {
		last_error_ = "nothing loaded";
		return out;
	}
	std::vector<uint8_t> bytes;
	if (bad_write_buffer(&file_, bytes) != 0) {
		last_error_ = "the clip cannot be written";
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

int ClipDocument::get_fps() const {
	return loaded_ ? static_cast<int>(file_.fps) : 0;
}

int ClipDocument::get_frame_count() const {
	return loaded_ ? static_cast<int>(file_.frame_count) : 0;
}

int ClipDocument::get_flags() const {
	return loaded_ ? static_cast<int>(file_.flags) : 0;
}

bool ClipDocument::is_loop() const {
	return loaded_ && (file_.flags & 1u) != 0;
}

bool ClipDocument::has_translations() const {
	return loaded_ && (file_.flags & 2u) != 0;
}

int ClipDocument::get_bone_count() const {
	return loaded_ ? static_cast<int>(file_.num_bones) : 0;
}

String ClipDocument::get_bone_name(int p_bone) const {
	if (!loaded_ || p_bone < 0 || static_cast<size_t>(p_bone) >= file_.num_bones) return String();
	return String(file_.bones[p_bone].name);
}

int ClipDocument::get_bone_parent(int p_bone) const {
	if (!loaded_ || p_bone < 0 || static_cast<size_t>(p_bone) >= file_.num_bones) return -1;
	return file_.bones[p_bone].parent_index;
}

Vector3 ClipDocument::get_bone_position(int p_bone) const {
	if (!loaded_ || p_bone < 0 || static_cast<size_t>(p_bone) >= file_.num_bones) return Vector3();
	const BadBone &b = file_.bones[p_bone];
	return Vector3(b.position[0], b.position[1], b.position[2]);
}

int ClipDocument::get_channel_key_count(int p_bone) const {
	if (!loaded_ || p_bone < 0 || static_cast<size_t>(p_bone) >= file_.num_channels) return 0;
	return static_cast<int>(file_.channels[p_bone].frame_count);
}

Quaternion ClipDocument::get_channel_rotation(int p_bone, int p_key) const {
	if (p_key < 0 || p_key >= get_channel_key_count(p_bone)) return Quaternion();
	const BadQuaternion &q = file_.channels[p_bone].rotations[p_key];
	return Quaternion(q.x, q.y, q.z, q.w);
}

int ClipDocument::get_channel_frame_length(int p_bone, int p_key) const {
	if (p_key < 0 || p_key >= get_channel_key_count(p_bone)) return 0;
	return file_.channels[p_bone].frame_lengths[p_key];
}

int ClipDocument::get_event_count() const {
	return loaded_ ? static_cast<int>(file_.num_events) : 0;
}

Vector3 ClipDocument::get_event_velocity(int p_key) const {
	if (p_key < 0 || p_key >= get_event_count()) return Vector3();
	const BadEvent &e = file_.events[p_key];
	return Vector3(e.velocity[0], e.velocity[1], e.velocity[2]);
}

float ClipDocument::get_event_bottom(int p_key) const {
	if (p_key < 0 || p_key >= get_event_count()) return 0.0f;
	return file_.events[p_key].bottom;
}

float ClipDocument::get_event_top(int p_key) const {
	if (p_key < 0 || p_key >= get_event_count()) return 0.0f;
	return file_.events[p_key].top;
}

int ClipDocument::get_event_trigger(int p_key) const {
	if (p_key < 0 || p_key >= get_event_count()) return 0;
	return file_.events[p_key].trigger;
}

Vector3 ClipDocument::get_translation(int p_frame, int p_bone) const {
	if (!loaded_ || file_.translations == nullptr || p_frame < 0 || p_bone < 0 ||
			static_cast<size_t>(p_bone) >= file_.num_bones) {
		return Vector3();
	}
	const size_t index = static_cast<size_t>(p_frame) * file_.num_bones + static_cast<size_t>(p_bone);
	if (index >= file_.num_translations) return Vector3();
	return Vector3(file_.translations[index][0], file_.translations[index][1], file_.translations[index][2]);
}
