#pragma once

// Shared helpers for reading raw NovaLogic data files: payload decode
// (vfs_decode) and content sniffing used across the adapter's format code.

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace godot {

// Engine bytes -> PackedByteArray, the one copy every binding that hands a
// std::vector<uint8_t> (a serialized document, a decoded payload, an RGBA8
// frame) to Godot makes. An empty input yields an empty array.
PackedByteArray to_packed_bytes(const uint8_t *p_data, size_t p_size);
inline PackedByteArray to_packed_bytes(const std::vector<uint8_t> &p_bytes) {
	return to_packed_bytes(p_bytes.data(), p_bytes.size());
}

bool decode_nova_payload_bytes(PackedByteArray &p_bytes);
bool read_nova_payload_file(const String &p_path, PackedByteArray &r_bytes);

// DDS container check by magic ("DDS "), shared by the texture loaders so a
// mis-extensioned file is classified by content, not by name.
bool bytes_look_like_dds(const PackedByteArray &p_bytes);

} // namespace godot
