#include "texture_path_resolver.h"

#include "util/pcx_texture_bridge.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace opennova {

namespace {

// NovaLogic assets reference textures by name with inconsistent case and an
// extension that may not match what is on disk (e.g. a .trn says "trntile10.tga"
// while the file is "TRNTILE10.TGA"), so we must try multiple stem casings and
// extensions. Godot's res:// paths are case-sensitive internally, hence the
// permutations below — do NOT "simplify" this back to a single exact lookup.
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

	// NovaLogic outline/overlay textures append an "_O" suffix to the base name.
	std::vector<godot::String> base_stems = stems;
	for (const godot::String &base : base_stems) {
		if (!base.to_lower().ends_with("_o")) {
			append_unique(stems, base + godot::String("_O"));
			append_unique(stems, base + godot::String("_o"));
		}
	}
	return stems;
}

// Candidate FILENAMES (not full paths) to try, in priority order: the requested
// name as-is first, then every stem casing x extension.
std::vector<godot::String> texture_candidate_filenames(const godot::String &filename) {
	std::vector<godot::String> candidates;
	const godot::String file = filename.get_file();
	if (file.is_empty()) {
		return candidates;
	}

	append_unique(candidates, file);
	for (const godot::String &stem : texture_stems(filename)) {
		for (const char *ext : tex_ext_priority) {
			append_unique(candidates, stem + godot::String(".") + ext);
		}
	}
	return candidates;
}

bool is_resource_dir(const godot::String &dir) {
	return dir.begins_with("res://") || dir.begins_with("uid://");
}

// List an absolute/external directory ONCE into a lower-cased filename -> real
// filename index, so callers probe in memory instead of re-enumerating the
// directory per candidate.
std::unordered_map<std::string, godot::String> build_lowercase_dir_index(const godot::String &dir) {
	std::unordered_map<std::string, godot::String> index;
	godot::Ref<godot::DirAccess> dir_access = godot::DirAccess::open(dir);
	if (dir_access.is_null()) {
		return index;
	}
	dir_access->list_dir_begin();
	godot::String entry = dir_access->get_next();
	while (!entry.is_empty()) {
		if (!dir_access->current_is_dir()) {
			index.emplace(std::string(entry.to_lower().utf8().get_data()), entry);
		}
		entry = dir_access->get_next();
	}
	dir_access->list_dir_end();
	return index;
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

// Decode a texture from raw on-disk bytes. Used ONLY for absolute/external paths
// (original game assets) — res:// assets go through ResourceLoader instead, which
// also resolves the imported .ctex that replaces the raw source in exported PCKs.
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

	const std::vector<godot::String> candidates = texture_candidate_filenames(filename);

	if (is_resource_dir(dir)) {
		// res:// — probe ResourceLoader per case-variant. This matches imported
		// textures via their .import/.uid remap (so it works in exported PCKs where
		// the raw source is stripped) without enumerating the directory.
		godot::ResourceLoader *loader = godot::ResourceLoader::get_singleton();
		for (const godot::String &file : candidates) {
			const godot::String path = dir.path_join(file);
			if (loader->exists(path)) {
				return path;
			}
		}
		return godot::String();
	}

	// Absolute / external — single directory listing, in-memory case-insensitive match.
	const std::unordered_map<std::string, godot::String> index = build_lowercase_dir_index(dir);
	if (index.empty()) {
		return godot::String();
	}
	for (const godot::String &file : candidates) {
		auto it = index.find(std::string(file.to_lower().utf8().get_data()));
		if (it != index.end()) {
			return dir.path_join(it->second);
		}
	}
	return godot::String();
}

godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename) {
	if (filename.is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}

	// Try each candidate in priority order and keep searching when one resolves
	// but fails to decode (e.g. a truncated/garbage same-stem file shadowing a
	// valid sibling), matching the original loop-and-fallback behavior.
	const std::vector<godot::String> candidates = texture_candidate_filenames(filename);

	if (is_resource_dir(dir)) {
		// res://: ResourceLoader resolves the imported CompressedTexture2D (.ctex)
		// for .tga and routes .pcx/.mdt through ResourceFormatLoaderNovaTexture —
		// the only path that survives export (raw .tga sources are not packed).
		godot::ResourceLoader *loader = godot::ResourceLoader::get_singleton();
		for (const godot::String &file : candidates) {
			const godot::String path = dir.path_join(file);
			if (loader->exists(path)) {
				godot::Ref<godot::Texture2D> tex = loader->load(path);
				if (tex.is_valid()) {
					return tex;
				}
			}
		}
		return godot::Ref<godot::Texture2D>();
	}

	// Absolute/external original-asset path: ResourceLoader only handles res://,
	// so decode the raw bytes ourselves. One directory listing, in-memory probe.
	const std::unordered_map<std::string, godot::String> index = build_lowercase_dir_index(dir);
	if (index.empty()) {
		return godot::Ref<godot::Texture2D>();
	}
	for (const godot::String &file : candidates) {
		auto it = index.find(std::string(file.to_lower().utf8().get_data()));
		if (it != index.end()) {
			godot::Ref<godot::Texture2D> tex = load_existing_texture_path(dir.path_join(it->second));
			if (tex.is_valid()) {
				return tex;
			}
		}
	}
	return godot::Ref<godot::Texture2D>();
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
