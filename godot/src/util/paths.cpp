#include "util/paths.h"

#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void Paths::_bind_methods() {
	ClassDB::bind_static_method("Paths", D_METHOD("resolve_file", "dir", "name"), &Paths::resolve_file);
	ClassDB::bind_static_method("Paths", D_METHOD("list_files", "dir", "suffix"), &Paths::list_files);
}

String Paths::resolve_file(const String &dir, const String &name) {
	return opennova::resolve_file_in_dir(dir, name);
}

PackedStringArray Paths::list_files(const String &dir, const String &suffix) {
	PackedStringArray out;
	if (dir.is_empty()) {
		return out;
	}
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_null()) {
		return out;
	}
	const String suffix_lower = suffix.to_lower();
	da->list_dir_begin();
	String entry = da->get_next();
	while (!entry.is_empty()) {
		if (!da->current_is_dir() && entry.to_lower().ends_with(suffix_lower)) {
			out.push_back(dir.path_join(entry));
		}
		entry = da->get_next();
	}
	da->list_dir_end();
	return out;
}
