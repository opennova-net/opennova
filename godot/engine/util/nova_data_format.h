#pragma once

// Minimal Resource wrapper for raw NovaLogic data files.
// Used by the individual format loaders (CPT, BAD, DEF) to make
// these files visible to Godot's resource/export system.

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

bool decode_nova_payload_bytes(PackedByteArray &p_bytes);
bool read_nova_payload_file(const String &p_path, PackedByteArray &r_bytes);

// DDS container check by magic ("DDS "), shared by the texture loaders so a
// mis-extensioned file is classified by content, not by name.
bool bytes_look_like_dds(const PackedByteArray &p_bytes);

class NovaDataFile : public Resource {
	GDCLASS(NovaDataFile, Resource)

	PackedByteArray data;

protected:
	static void _bind_methods();

public:
	void set_data(const PackedByteArray &p_data);
	PackedByteArray get_data() const;
};

} // namespace godot
