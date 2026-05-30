#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class NovaResourceRoot : public RefCounted {
	GDCLASS(NovaResourceRoot, RefCounted)

	String root_dir_;
	String last_error_;

	static bool has_virtual_scheme(const String &path);
	static String to_native_path(const String &path);
	static String normalize_dir(const String &path);
	static String lookup_name(const String &name);

protected:
	static void _bind_methods();

public:
	static bool is_valid_root(const String &path);

	Error set_root_dir(const String &path);
	String get_root_dir() const;
	String get_last_error() const;
	void clear();

	String resolve_file(const String &name);
	PackedStringArray list_files(const String &suffix = String()) const;
	Ref<Texture2D> load_texture(const String &name) const;
	Ref<Resource> load_font(const String &name) const;
};

} // namespace godot
