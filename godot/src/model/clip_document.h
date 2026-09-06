#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <formats/bad/bad.h>

namespace godot {

// One .bad clip: parsed from disk or bytes (the projector hands its build over
// as bytes too), written back through the from-scratch writer. The format
// reads and writes itself; Godot's resource system never sees it. Read-only
// beyond load/save: the editable form of a clip is the Animation the
// projector samples. Every getter answers in the file's own words (channel
// quaternions in the model frame, bone positions as stored, events per frame).
class ClipDocument : public RefCounted {
	GDCLASS(ClipDocument, RefCounted)

	opennova::bad::BadFile file_ = {};
	bool loaded_ = false;
	String source_path_;
	String last_error_;

	void _clear();

protected:
	static void _bind_methods();

public:
	~ClipDocument();

	Error load_from_path(const String &p_path);
	Error load_from_bytes(const PackedByteArray &p_bytes);
	Error save_to_path(const String &p_path);
	PackedByteArray to_bytes();

	bool is_loaded() const { return loaded_; }
	String get_source_path() const { return source_path_; }
	String get_last_error() const { return last_error_; }

	int get_fps() const;
	int get_frame_count() const;
	int get_flags() const;
	bool is_loop() const;
	bool has_translations() const;
	int get_bone_count() const;
	String get_bone_name(int p_bone) const;
	int get_bone_parent(int p_bone) const;
	Vector3 get_bone_position(int p_bone) const;
	int get_channel_key_count(int p_bone) const;
	Quaternion get_channel_rotation(int p_bone, int p_key) const;
	int get_channel_frame_length(int p_bone, int p_key) const;
	int get_event_count() const;
	Vector3 get_event_velocity(int p_key) const;
	float get_event_bottom(int p_key) const;
	float get_event_top(int p_key) const;
	int get_event_trigger(int p_key) const;
	Vector3 get_translation(int p_frame, int p_bone) const;

	// C++ consumers.
	const opennova::bad::BadFile *file() const { return loaded_ ? &file_ : nullptr; }
};

} // namespace godot
