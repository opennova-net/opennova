#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <resource_index/resource_index.h>

namespace godot {

class NovaResourceIndex : public RefCounted {
	GDCLASS(NovaResourceIndex, RefCounted)

	opennova::ResourceIndex index_;

	static String to_native_path(const String &path);
	static Dictionary file_entry_to_dictionary(const opennova::ResourceFileEntry &entry);

protected:
	static void _bind_methods();

public:
	Error scan(const String &path);
	void clear();
	Array get_resource_files(const String &kind) const;
	String get_root_dir() const;
	String get_last_error() const;
};

} // namespace godot
