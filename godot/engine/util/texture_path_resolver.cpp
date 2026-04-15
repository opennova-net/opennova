#include "texture_path_resolver.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

namespace opennova {

namespace {

static constexpr const char *tex_ext_priority[] = {
	"tga", "TGA", "dds", "DDS", "mdt", "MDT", "pcx", "PCX",
	"png", "PNG", "jpg", "JPG", "jpeg", "JPEG", "bmp", "BMP"
};

} // namespace

godot::String resolve_texture_path(const godot::String &dir, const godot::String &filename) {
	if (filename.is_empty()) {
		return godot::String();
	}

	const godot::String stem = filename.get_file().get_basename();
	if (stem.is_empty()) {
		return godot::String();
	}

	const godot::String stem_lower = stem.to_lower();
	const godot::String stem_upper = stem.to_upper();
	const godot::String stems[] = {stem, stem_lower, stem_upper};

	const bool is_res_path = dir.begins_with("res://");
	for (const char *ext : tex_ext_priority) {
		for (const godot::String &candidate_stem : stems) {
			const godot::String candidate = dir.path_join(candidate_stem + godot::String(".") + ext);
			if (is_res_path) {
				if (godot::ResourceLoader::get_singleton()->exists(candidate)) {
					return candidate;
				}
			} else if (godot::FileAccess::file_exists(candidate)) {
				return candidate;
			}
		}
	}

	return godot::String();
}

godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename) {
	if (filename.is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}

	const godot::String resolved = resolve_texture_path(dir, filename);
	if (resolved.is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}

	return godot::ResourceLoader::get_singleton()->load(resolved);
}

godot::String resolve_asset_path(const godot::String &dir, const godot::String &name, const char *ext) {
	if (name.is_empty()) {
		return godot::String();
	}

	godot::String stem = name.get_file().get_basename();
	if (stem.is_empty()) {
		stem = name;
	}

	const godot::String stem_lower = stem.to_lower();
	const godot::String stem_upper = stem.to_upper();
	godot::String stem_cap = stem_lower;
	if (!stem_cap.is_empty()) {
		stem_cap = stem_cap.substr(0, 1).to_upper() + stem_cap.substr(1);
	}

	const godot::String stems[] = {stem, stem_lower, stem_upper, stem_cap};
	for (const godot::String &candidate_stem : stems) {
		const godot::String candidate = dir.path_join(candidate_stem + godot::String(".") + ext);
		if (godot::ResourceLoader::get_singleton()->exists(candidate)) {
			return candidate;
		}

		const godot::String upper_candidate = dir.path_join(candidate_stem + godot::String(".") + godot::String(ext).to_upper());
		if (godot::ResourceLoader::get_singleton()->exists(upper_candidate)) {
			return upper_candidate;
		}
	}

	return godot::String();
}

} // namespace opennova
