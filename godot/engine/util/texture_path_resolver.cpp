#include "texture_path_resolver.h"

#include "util/pcx_texture_bridge.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <vector>

namespace opennova {

namespace {

static constexpr const char *tex_ext_priority[] = {
	"tga", "TGA", "dds", "DDS", "dds.tga", "DDS.TGA", "mdt", "MDT", "pcx", "PCX",
	"png", "PNG", "jpg", "JPG", "jpeg", "JPEG", "bmp", "BMP"
};

void append_unique(std::vector<godot::String> &items, const godot::String &value) {
	if (value.is_empty()) {
		return;
	}
	for (const godot::String &item : items) {
		if (item == value) {
			return;
		}
	}
	items.push_back(value);
}

std::vector<godot::String> texture_stems(const godot::String &filename) {
	std::vector<godot::String> stems;
	const godot::String stem = filename.get_file().get_basename();
	if (stem.is_empty()) {
		return stems;
	}

	append_unique(stems, stem);
	append_unique(stems, stem.to_lower());
	append_unique(stems, stem.to_upper());

	std::vector<godot::String> base_stems = stems;
	for (const godot::String &base : base_stems) {
		if (!base.to_lower().ends_with("_o")) {
			append_unique(stems, base + godot::String("_O"));
			append_unique(stems, base + godot::String("_o"));
		}
	}
	return stems;
}

std::vector<godot::String> texture_candidate_paths(const godot::String &dir, const godot::String &filename) {
	std::vector<godot::String> candidates;
	const godot::String file = filename.get_file();
	if (file.is_empty()) {
		return candidates;
	}

	append_unique(candidates, dir.path_join(file));
	for (const godot::String &stem : texture_stems(filename)) {
		for (const char *ext : tex_ext_priority) {
			append_unique(candidates, dir.path_join(stem + godot::String(".") + ext));
		}
	}
	return candidates;
}

godot::String existing_path_with_disk_case(const godot::String &path) {
	const godot::String dir = path.get_base_dir();
	const godot::String file = path.get_file();
	const godot::String file_lower = file.to_lower();

	godot::Ref<godot::DirAccess> dir_access = godot::DirAccess::open(dir);
	if (dir_access.is_valid()) {
		dir_access->list_dir_begin();
		godot::String entry = dir_access->get_next();
		while (!entry.is_empty()) {
			if (!dir_access->current_is_dir() && entry.to_lower() == file_lower) {
				dir_access->list_dir_end();
				return dir.path_join(entry);
			}
			entry = dir_access->get_next();
		}
		dir_access->list_dir_end();
	}

	if (path.begins_with("res://")) {
		if (godot::ResourceLoader::get_singleton()->exists(path)) {
			return path;
		}
	} else if (godot::FileAccess::file_exists(path)) {
		return path;
	}

	return godot::String();
}

bool bytes_look_like_dds(const godot::PackedByteArray &bytes) {
	return bytes.size() >= 4 &&
			bytes[0] == 'D' &&
			bytes[1] == 'D' &&
			bytes[2] == 'S' &&
			bytes[3] == ' ';
}

godot::Ref<godot::Texture2D> texture_from_image(godot::Ref<godot::Image> image) {
	if (image.is_null() || image->is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}
	if (image->is_compressed()) {
		image->decompress();
	}
	image->generate_mipmaps();
	return godot::ImageTexture::create_from_image(image);
}

godot::Ref<godot::Texture2D> load_existing_texture_path(const godot::String &path) {
	const godot::String ext = path.get_extension().to_lower();

	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(path, godot::FileAccess::READ);
	if (file.is_null()) {
		return godot::Ref<godot::Texture2D>();
	}
	godot::PackedByteArray bytes = file->get_buffer(file->get_length());
	file.unref();

	if (ext == "pcx") {
		return texture_from_image(decode_pcx_image(bytes.ptr(), bytes.size()));
	}

	if (ext == "dds" && bytes_look_like_dds(bytes)) {
		return godot::ResourceLoader::get_singleton()->load(path, "ImageTexture", godot::ResourceLoader::CACHE_MODE_IGNORE);
	}

	if ((ext == "tga" || ext == "mdt" || ext == "dds") && !bytes_look_like_dds(bytes)) {
		if (bytes.size() < 18) {
			return godot::Ref<godot::Texture2D>();
		}
		godot::Ref<godot::Image> image;
		image.instantiate();
		if (image->load_tga_from_buffer(bytes) != godot::OK) {
			return godot::Ref<godot::Texture2D>();
		}
		return texture_from_image(image);
	}

	godot::Ref<godot::Image> image;
	image.instantiate();
	if (image->load(path) != godot::OK) {
		return godot::Ref<godot::Texture2D>();
	}
	return texture_from_image(image);
}

} // namespace

godot::String resolve_texture_path(const godot::String &dir, const godot::String &filename) {
	if (filename.is_empty()) {
		return godot::String();
	}

	for (const godot::String &candidate : texture_candidate_paths(dir, filename)) {
		const godot::String existing = existing_path_with_disk_case(candidate);
		if (!existing.is_empty()) {
			return existing;
		}
	}

	return godot::String();
}

godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename) {
	if (filename.is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}

	for (const godot::String &candidate : texture_candidate_paths(dir, filename)) {
		const godot::String existing = existing_path_with_disk_case(candidate);
		if (existing.is_empty()) {
			continue;
		}
		godot::Ref<godot::Texture2D> texture = load_existing_texture_path(existing);
		if (texture.is_valid()) {
			return texture;
		}
	}
	return godot::Ref<godot::Texture2D>();
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

godot::String resolve_sidecar_path(const godot::String &dir, const godot::String &filename, const char *ext) {
	if (filename.is_empty()) {
		return godot::String();
	}

	const godot::String direct = dir.path_join(filename);
	if (godot::FileAccess::file_exists(direct)) {
		return direct;
	}

	const godot::String requested_ext = godot::String(ext).to_lower();
	const godot::String file_dir = filename.get_base_dir();
	const godot::String file_name = filename.get_file();
	godot::String stem = file_name;
	if (file_name.get_extension().to_lower() == requested_ext) {
		stem = file_name.get_basename();
	}
	if (stem.is_empty()) {
		stem = file_name;
	}

	const godot::String stem_lower = stem.to_lower();
	const godot::String stem_upper = stem.to_upper();
	const godot::String stems[] = {stem, stem_lower, stem_upper};
	const godot::String exts[] = {requested_ext, requested_ext.to_upper()};
	const godot::String search_dir = file_dir.is_empty() ? dir : dir.path_join(file_dir);
	for (const godot::String &candidate_stem : stems) {
		for (const godot::String &candidate_ext : exts) {
			const godot::String candidate = search_dir.path_join(candidate_stem + godot::String(".") + candidate_ext);
			if (godot::FileAccess::file_exists(candidate)) {
				return candidate;
			}
		}
	}

	return godot::String();
}

} // namespace opennova
