#pragma once

// GDScript-facing entry point to the one shared file-resolution primitive
// (opennova::resolve_file_in_dir). Lets GDScript (veg_assets, the editor
// document) resolve/list files in a directory case-insensitively instead of
// each hand-rolling its own DirAccess scan.

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class NovaPaths : public Object {
	GDCLASS(NovaPaths, Object)

public:
	// Case-insensitively resolve `name` within `dir`; returns the real on-disk
	// path, or "" if absent.
	static String resolve_file(const String &dir, const String &name);

	// List files in `dir` whose name ends with `suffix` (case-insensitive),
	// returning their real-case paths (joined with `dir`).
	static PackedStringArray list_files(const String &dir, const String &suffix);

protected:
	static void _bind_methods();
};

} // namespace godot
