#pragma once

// Shared helpers for reading raw NovaLogic data files: payload decode
// (vfs_decode) and content sniffing used across the adapter's format code.

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

bool decode_nova_payload_bytes(PackedByteArray &p_bytes);
bool read_nova_payload_file(const String &p_path, PackedByteArray &r_bytes);

// DDS container check by magic ("DDS "), shared by the texture loaders so a
// mis-extensioned file is classified by content, not by name.
bool bytes_look_like_dds(const PackedByteArray &p_bytes);

} // namespace godot
