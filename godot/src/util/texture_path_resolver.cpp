#include "texture_path_resolver.h"

#include "util/nova_data_format.h"
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

// NovaLogic assets reference textures by name with inconsistent case (e.g. a
// .trn says "trntile10.tga" while the file is "TRNTILE10.TGA") and an extension
// that may not match what is on disk, so we try the name + extension fallbacks.
// Case is handled by the case-insensitive directory match (build_lowercase_dir_index),
// so the extension list needs no upper-case variants.
static constexpr const char *tex_ext_priority[] = {
	"tga", "dds", "dds.tga", "mdt", "pcx", "png", "jpg", "jpeg", "bmp"
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

bool is_texture_extension(const godot::String &extension) {
	const godot::String lower = extension.to_lower();
	if (lower.is_empty()) {
		return false;
	}
	for (const char *ext : tex_ext_priority) {
		const godot::String candidate(ext);
		if (candidate.find(".") == -1 && candidate == lower) {
			return true;
		}
	}
	return false;
}

void append_collapsed_texture_filenames(std::vector<godot::String> &items, const godot::String &filename) {
	godot::String collapsed = filename;
	if (!is_texture_extension(collapsed.get_extension())) {
		return;
	}

	while (true) {
		collapsed = collapsed.get_basename();
		if (!is_texture_extension(collapsed.get_extension())) {
			return;
		}
		append_unique(items, collapsed);
	}
}

std::vector<godot::String> texture_stems(const godot::String &filename) {
	std::vector<godot::String> stems;
	const godot::String stem = filename.get_file().get_basename();
	if (stem.is_empty()) {
		return stems;
	}

	append_unique(stems, stem);
	// NovaLogic outline/overlay textures append an "_O" suffix to the base name.
	// (Case is normalized by the directory match, so no _o/_O or stem-case dupes.)
	if (!stem.to_lower().ends_with("_o")) {
		append_unique(stems, stem + godot::String("_O"));
	}
	return stems;
}

bool is_resource_dir(const godot::String &dir) {
	return dir.begins_with("res://") || dir.begins_with("uid://");
}

// Per-session resolver caches (MAIN-THREAD ONLY; cleared on resource-dir change via
// opennova::clear_texture_resolver_caches()). The original code re-enumerated the
// whole asset directory on EVERY texture probe and re-decoded shared textures each
// time; for a directory of thousands of files placing hundreds of objects that is the
// dominant load cost. Keyed by dir / resolved-path so a different directory is a
// separate entry.
std::unordered_map<std::string, std::unordered_map<std::string, godot::String>> g_dir_index_cache;
std::unordered_map<std::string, godot::Ref<godot::Texture2D>> g_texture_cache;

// List an absolute/external directory into a lower-cased filename -> real filename
// index. Called once per directory by get_lowercase_dir_index(), which caches it.
std::unordered_map<std::string, godot::String> build_lowercase_dir_index_uncached(const godot::String &dir) {
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

// Cached lower-cased filename index for `dir`: enumerate the directory once, then
// reuse for every subsequent probe (callers do in-memory hash lookups instead of
// re-listing the directory). Returns a const reference to avoid copying the map.
const std::unordered_map<std::string, godot::String> &get_lowercase_dir_index(const godot::String &dir) {
	const std::string key(dir.utf8().get_data());
	auto it = g_dir_index_cache.find(key);
	if (it != g_dir_index_cache.end()) {
		return it->second;
	}
	auto inserted = g_dir_index_cache.emplace(key, build_lowercase_dir_index_uncached(dir));
	return inserted.first->second;
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

	godot::PackedByteArray bytes;
	if (!godot::read_nova_payload_file(path, bytes)) {
		return godot::Ref<godot::Texture2D>();
	}

	if (ext == "pcx") {
		return texture_from_image(decode_pcx_image(bytes.ptr(), bytes.size()));
	}

	// DDS (DXT/BC) payload by MAGIC, not extension: NovaLogic ships compressed
	// textures under .tga (and .mdt) names, so a "KPier2.TGA" whose bytes begin
	// with "DDS " must decode as DDS. Mirrors load_texture_from_bytes() below so the
	// two raw-bytes decoders stay identical (the extension-gated version rendered
	// these object textures white).
	if (godot::bytes_look_like_dds(bytes)) {
		godot::Ref<godot::Image> image;
		image.instantiate();
		if (image->load_dds_from_buffer(bytes) != godot::OK) {
			return godot::Ref<godot::Texture2D>();
		}
		return texture_from_image(image);
	}

	if ((ext == "tga" || ext == "mdt" || ext == "dds") && !godot::bytes_look_like_dds(bytes)) {
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

// Candidate FILENAMES (not full paths) to try, in priority order: the requested
// name as-is first, exact inner texture filenames exposed by compound extensions,
// then every existing stem x extension fallback.
std::vector<godot::String> texture_candidate_filenames(const godot::String &filename) {
	std::vector<godot::String> candidates;
	const godot::String file = filename.get_file();
	if (file.is_empty()) {
		return candidates;
	}

	append_unique(candidates, file);
	append_collapsed_texture_filenames(candidates, file);
	for (const godot::String &stem : texture_stems(filename)) {
		for (const char *ext : tex_ext_priority) {
			append_unique(candidates, stem + godot::String(".") + ext);
		}
	}
	return candidates;
}

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

	// Absolute / external — cached directory listing, in-memory case-insensitive match.
	const std::unordered_map<std::string, godot::String> &index = get_lowercase_dir_index(dir);
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

	// Absolute/external original-asset path: ResourceLoader only handles res://, so
	// decode the raw bytes ourselves. Cached directory listing + decoded-texture cache
	// so a shared texture is enumerated/decoded/uploaded once, not per material.
	const std::unordered_map<std::string, godot::String> &index = get_lowercase_dir_index(dir);
	if (index.empty()) {
		return godot::Ref<godot::Texture2D>();
	}
	for (const godot::String &file : candidates) {
		auto it = index.find(std::string(file.to_lower().utf8().get_data()));
		if (it != index.end()) {
			const godot::String resolved = dir.path_join(it->second);
			const std::string tex_key(resolved.utf8().get_data());
			auto cached = g_texture_cache.find(tex_key);
			godot::Ref<godot::Texture2D> tex;
			if (cached != g_texture_cache.end()) {
				tex = cached->second;
			} else {
				tex = load_existing_texture_path(resolved);
				// Cache the null too: a truncated/garbage file should not be re-read on
				// every probe; the loop still falls through to the next candidate.
				g_texture_cache.emplace(tex_key, tex);
			}
			if (tex.is_valid()) {
				return tex;
			}
		}
	}
	return godot::Ref<godot::Texture2D>();
}

godot::Ref<godot::Texture2D> load_texture_from_bytes(const godot::String &filename, const godot::PackedByteArray &bytes) {
	if (filename.is_empty() || bytes.is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}

	const godot::String ext = filename.get_extension().to_lower();
	if (ext == "pcx") {
		return texture_from_image(decode_pcx_image(bytes.ptr(), static_cast<size_t>(bytes.size())));
	}

	// True DDS (DXT/BC payload): Godot 4.6 decodes it straight from the buffer, so a DDS
	// that exists only inside a .pff (no filesystem path) still loads. Keyed on the magic,
	// not the extension, so a mis-named entry still routes here. texture_from_image()
	// decompresses the BC payload before generating mipmaps.
	if (godot::bytes_look_like_dds(bytes)) {
		godot::Ref<godot::Image> image;
		image.instantiate();
		if (image->load_dds_from_buffer(bytes) != godot::OK) {
			return godot::Ref<godot::Texture2D>();
		}
		return texture_from_image(image);
	}

	if ((ext == "tga" || ext == "mdt" || ext == "dds") && !godot::bytes_look_like_dds(bytes)) {
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

	if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp") {
		godot::Ref<godot::Image> image;
		image.instantiate();
		godot::Error error = godot::FAILED;
		if (ext == "png") {
			error = image->load_png_from_buffer(bytes);
		} else if (ext == "jpg" || ext == "jpeg") {
			error = image->load_jpg_from_buffer(bytes);
		} else {
			error = image->load_bmp_from_buffer(bytes);
		}
		if (error != godot::OK) {
			return godot::Ref<godot::Texture2D>();
		}
		return texture_from_image(image);
	}

	return godot::Ref<godot::Texture2D>();
}

godot::String resolve_file_in_dir(const godot::String &dir, const godot::String &name) {
	if (dir.is_empty() || name.is_empty()) {
		return godot::String();
	}
	if (is_resource_dir(dir)) {
		const godot::String path = dir.path_join(name);
		return godot::ResourceLoader::get_singleton()->exists(path) ? path : godot::String();
	}
	const std::unordered_map<std::string, godot::String> &index = get_lowercase_dir_index(dir);
	auto it = index.find(std::string(name.to_lower().utf8().get_data()));
	return it != index.end() ? dir.path_join(it->second) : godot::String();
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

void clear_texture_resolver_caches() {
	g_dir_index_cache.clear();
	g_texture_cache.clear();
}

} // namespace opennova
