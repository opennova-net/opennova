#ifndef OPENNOVA_CBIN_ASSET_LOOKUP_H
#define OPENNOVA_CBIN_ASSET_LOOKUP_H

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "fnt/nova_fnt_resource.h"
#include "util/nova_data_format.h"
#include "util/texture_path_resolver.h"

namespace godot {
namespace cbin_internal {

namespace {

inline Ref<Resource> load_font_path(const String &p_path) {
	if (p_path.is_empty()) {
		return Ref<Resource>();
	}

	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) {
		return Ref<Resource>();
	}

	Ref<NovaFntResource> font;
	font.instantiate();
	if (font->load_from_bytes(bytes) != OK) {
		return Ref<Resource>();
	}
	return font;
}

}  // namespace

// Find a Nova .fnt by basename in a single resource root directory.
inline Ref<Resource> find_font_by_name(const String &p_name, const String &p_base_dir) {
	if (p_name.is_empty() || p_base_dir.is_empty()) {
		return Ref<Resource>();
	}

	const String file_name = p_name.get_extension().to_lower() == "fnt" ? p_name : p_name + String(".fnt");
	const String direct_path = p_base_dir.path_join(file_name);
	if (FileAccess::file_exists(direct_path)) {
		return load_font_path(direct_path);
	}
	const String path = opennova::resolve_file_in_dir(p_base_dir, file_name);
	return path.is_empty() ? Ref<Resource>() : load_font_path(path);
}

// Resolve an image by filename in a single resource root directory. Extension
// and case fallbacks are delegated to the shared engine texture resolver.
inline Ref<Resource> find_texture_by_name(const String &p_name, const String &p_base_dir) {
	if (p_name.is_empty() || p_base_dir.is_empty()) {
		return Ref<Resource>();
	}
	return opennova::load_texture_from_dir(p_base_dir, p_name);
}

}  // namespace cbin_internal
}  // namespace godot

#endif  // OPENNOVA_CBIN_ASSET_LOOKUP_H
