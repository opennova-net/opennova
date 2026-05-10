#ifndef OPENNOVA_CBIN_ASSET_LOOKUP_H
#define OPENNOVA_CBIN_ASSET_LOOKUP_H

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {
namespace cbin_internal {

// Find font resource by name (case-insensitive).
// Looks in res://assets/fonts/{name}.fnt
inline Ref<Resource> find_font_by_name(const String &p_name) {
	if (p_name.is_empty()) {
		return Ref<Resource>();
	}

	// Try exact case first.
	String path = "res://assets/fonts/" + p_name + ".fnt";
	if (ResourceLoader::get_singleton()->exists(path)) {
		return ResourceLoader::get_singleton()->load(path);
	}

	// Try case-insensitive search.
	Ref<DirAccess> dir = DirAccess::open("res://assets/fonts");
	if (!dir.is_valid()) {
		return Ref<Resource>();
	}

	String lower_name = p_name.to_lower();
	dir->list_dir_begin();
	String filename = dir->get_next();
	while (!filename.is_empty()) {
		if (!dir->current_is_dir() && filename.get_extension().to_lower() == "fnt") {
			if (filename.get_basename().to_lower() == lower_name) {
				dir->list_dir_end();
				path = "res://assets/fonts/" + filename;
				return ResourceLoader::get_singleton()->load(path);
			}
		}
		filename = dir->get_next();
	}
	dir->list_dir_end();
	return Ref<Resource>();
}

// Find texture resource by name (case-insensitive).
// Looks in p_base_path/{name} (defaults to res://assets/textures/)
// When the exact filename is not found, also tries common image extensions
// (.png, .pcx, .tga, .jpg, .bmp) for the same basename, so callers do not
// need to know the on-disk format in advance.

namespace {

// Scan p_dir (already open) for a file whose name matches p_lower_name
// case-insensitively. Returns the matched filename, or empty string.
// Resets the directory listing on each call.
inline String scan_dir_for_name(Ref<DirAccess> p_dir, const String &p_lower_name) {
	p_dir->list_dir_begin();
	String filename = p_dir->get_next();
	while (!filename.is_empty()) {
		if (!p_dir->current_is_dir() && filename.to_lower() == p_lower_name) {
			p_dir->list_dir_end();
			return filename;
		}
		filename = p_dir->get_next();
	}
	p_dir->list_dir_end();
	return String();
}

}  // namespace

inline Ref<Resource> find_texture_by_name(const String &p_name,
                                           const String &p_base_path = "res://assets/textures/") {
	if (p_name.is_empty()) {
		return Ref<Resource>();
	}

	// Try exact path first.
	String path = p_base_path + p_name;
	if (ResourceLoader::get_singleton()->exists(path)) {
		return ResourceLoader::get_singleton()->load(path);
	}

	// Open the directory for case-insensitive scanning.
	String dir_path = p_base_path;
	if (dir_path.ends_with("/")) {
		dir_path = dir_path.substr(0, dir_path.length() - 1);
	}

	Ref<DirAccess> dir = DirAccess::open(dir_path);
	if (!dir.is_valid()) {
		return Ref<Resource>();
	}

	// Case-insensitive scan for the exact filename as given.
	String lower_name = p_name.to_lower();
	String matched = scan_dir_for_name(dir, lower_name);
	if (!matched.is_empty()) {
		String candidate = p_base_path + matched;
		if (ResourceLoader::get_singleton()->exists(candidate)) {
			return ResourceLoader::get_singleton()->load(candidate);
		}
	}

	// Fallback: try common image extensions for the same basename.
	// Useful when the .kda references "cr1.png" but the file on disk is "cr1.pcx"
	// (or vice versa) — the format loader is the resolution layer.
	static const char *kAltExts[] = {".png", ".pcx", ".tga", ".jpg", ".bmp"};
	String base_name = p_name.get_basename();
	for (const char *ext : kAltExts) {
		String alt = (base_name + ext).to_lower();
		if (alt == lower_name) {
			continue;  // Already tried this one above.
		}
		matched = scan_dir_for_name(dir, alt);
		if (!matched.is_empty()) {
			String candidate = p_base_path + matched;
			if (ResourceLoader::get_singleton()->exists(candidate)) {
				return ResourceLoader::get_singleton()->load(candidate);
			}
		}
	}

	return Ref<Resource>();
}

}  // namespace cbin_internal
}  // namespace godot

#endif  // OPENNOVA_CBIN_ASSET_LOOKUP_H
