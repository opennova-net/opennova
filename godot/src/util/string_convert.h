#pragma once

#include <base/io/cp1252.h>

#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <string>

namespace opennova {

// Plain UTF-8 godot::String <-> std::string bridges, shared by the binding
// modules that shuttle text between engine/ structs and Godot. NOT for
// retail-encoded text: use cp1252.h at the format or glyph boundary, or
// cp1252_to_gd below for retail wire text (server browser / LAN rows).

inline godot::String to_gd(const std::string &s) {
	return godot::String::utf8(s.c_str(), static_cast<int>(s.length()));
}

inline std::string to_std(const godot::String &s) {
	const godot::CharString utf8 = s.utf8();
	return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

// Retail-encoded (Windows-1252) bytes -> godot::String. The NovaWorld GSB rows
// and the LAN discovery replies carry the advertised server name in the single-byte
// codepage of the machine that typed it; reading those bytes as UTF-8 drops a
// 0xA9 (the copyright sign) as an invalid sequence and trips Godot's UTF-8
// warning.
inline godot::String cp1252_to_gd(const std::string &s) {
	godot::String out;
	for (const char c : s) {
		out += cp1252_decode_byte(static_cast<std::uint8_t>(c));
	}
	return out;
}

} // namespace opennova
