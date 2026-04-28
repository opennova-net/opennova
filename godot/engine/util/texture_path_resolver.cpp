#include "texture_path_resolver.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include "util/pcx_texture_bridge.h"

#include <algorithm>
#include <vector>

namespace opennova {

namespace {

static constexpr const char *tex_ext_priority[] = {
	"dds", "DDS", "mdt", "MDT", "dds.tga", "DDS.TGA", "tga", "TGA", "pcx", "PCX",
	"png", "PNG", "jpg", "JPG", "jpeg", "JPEG", "bmp", "BMP"
};

bool candidate_exists(const godot::String &path) {
	if (path.begins_with("res://")) {
		return godot::ResourceLoader::get_singleton()->exists(path);
	}
	return godot::FileAccess::file_exists(path);
}

void append_unique(std::vector<godot::String> &items, const godot::String &value) {
	if (value.is_empty()) {
		return;
	}
	if (std::find(items.begin(), items.end(), value) == items.end()) {
		items.push_back(value);
	}
}

void append_stem_variants(std::vector<godot::String> &items, const godot::String &dir, const godot::String &stem,
		const char *ext) {
	const godot::String extension = godot::String(".") + ext;
	append_unique(items, dir.path_join(stem + extension));
	append_unique(items, dir.path_join(stem.to_lower() + extension));
	append_unique(items, dir.path_join(stem.to_upper() + extension));
	append_unique(items, dir.path_join(stem + godot::String("_O") + extension));
	append_unique(items, dir.path_join(stem + godot::String("_o") + extension));
	append_unique(items, dir.path_join(stem.to_lower() + godot::String("_o") + extension));
	append_unique(items, dir.path_join(stem.to_upper() + godot::String("_O") + extension));
}

godot::String texture_stem(const godot::String &filename) {
	const godot::String file = filename.get_file();
	const godot::String lower = file.to_lower();
	static constexpr const char *known_suffixes[] = {
		".dds.tga", ".tga", ".dds", ".mdt", ".pcx", ".png", ".jpg", ".jpeg", ".bmp"
	};
	for (const char *suffix : known_suffixes) {
		const godot::String suffix_string(suffix);
		if (lower.ends_with(suffix_string)) {
			return file.substr(0, file.length() - suffix_string.length());
		}
	}
	return file.get_basename();
}

godot::Ref<godot::Image> load_nova_image_file(const godot::String &path) {
	const godot::String lower = path.to_lower();
	godot::Ref<godot::Image> image;

	if (lower.ends_with(".pcx")) {
		godot::Ref<godot::FileAccess> file = godot::FileAccess::open(path, godot::FileAccess::READ);
		if (file.is_null()) {
			return image;
		}
		const godot::PackedByteArray bytes = file->get_buffer(file->get_length());
		file.unref();
		image = decode_pcx_image(bytes.ptr(), bytes.size());
	} else if (lower.ends_with(".tga") || lower.ends_with(".dds") || lower.ends_with(".mdt")) {
		godot::Ref<godot::FileAccess> file = godot::FileAccess::open(path, godot::FileAccess::READ);
		if (file.is_null()) {
			return image;
		}
		const godot::PackedByteArray bytes = file->get_buffer(file->get_length());
		file.unref();
		image.instantiate();
		if (image->load_tga_from_buffer(bytes) != godot::OK) {
			image.unref();
			return image;
		}
	} else {
		image.instantiate();
		if (image->load(path) != godot::OK) {
			image.unref();
			return image;
		}
	}

	if (image.is_null() || image->is_empty()) {
		image.unref();
		return image;
	}
	if (image->is_compressed()) {
		image->decompress();
	}
	image->generate_mipmaps();
	return image;
}

} // namespace

godot::String resolve_texture_path(const godot::String &dir, const godot::String &filename) {
	if (dir.is_empty() || filename.is_empty()) {
		return godot::String();
	}

	const godot::String file = filename.get_file();
	if (file.is_empty()) {
		return godot::String();
	}

	std::vector<godot::String> candidates;
	append_unique(candidates, dir.path_join(file));
	append_unique(candidates, dir.path_join(file.to_lower()));
	append_unique(candidates, dir.path_join(file.to_upper()));

	const godot::String stem = texture_stem(file);
	if (stem.is_empty()) {
		return godot::String();
	}

	for (const char *ext : tex_ext_priority) {
		append_stem_variants(candidates, dir, stem, ext);
	}

	for (const godot::String &candidate : candidates) {
		if (candidate_exists(candidate)) {
			return candidate;
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

	if (resolved.begins_with("res://") || resolved.begins_with("user://")) {
		godot::Ref<godot::Texture2D> texture = godot::ResourceLoader::get_singleton()->load(resolved);
		if (texture.is_valid()) {
			return texture;
		}
	}

	godot::Ref<godot::Image> image = load_nova_image_file(resolved);
	if (image.is_null() || image->is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}
	return godot::ImageTexture::create_from_image(image);
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
