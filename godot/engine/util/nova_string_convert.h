#pragma once

#include <godot_cpp/variant/string.hpp>

#include <string>

namespace opennova {

// Plain UTF-8 godot::String <-> std::string bridges, shared by the binding
// modules that shuttle text between libs/ structs and Godot. NOT for
// retail-encoded text: use nova_cp1252.h at the format or glyph boundary.

inline godot::String to_gd(const std::string &s) {
	return godot::String::utf8(s.c_str(), static_cast<int>(s.length()));
}

inline std::string to_std(const godot::String &s) {
	const godot::CharString utf8 = s.utf8();
	return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

} // namespace opennova
