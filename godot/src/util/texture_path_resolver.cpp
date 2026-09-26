#include <base/resource_index/texture_candidates.h>
#include <runtime/renderer/material_texture.h>
#include "util/texture_path_resolver.h"

#include "util/data_format.h"
#include "util/pcx_texture_bridge.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/image_texture3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opennova {

namespace {

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
			index.emplace(to_std(entry.to_lower()), entry);
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
	const std::string key = to_std(dir);
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
// The DDS-by-magic then TGA-by-extension decode both raw-bytes loaders run.
// True = the bytes were claimed by one of the two routes (r_texture is null
// when that route's decode failed); false = neither route applies.
bool decode_dds_or_tga(const godot::String &ext, const godot::PackedByteArray &bytes,
		godot::Ref<godot::Texture2D> &r_texture) {
	if (godot::bytes_look_like_dds(bytes)) {
		godot::Ref<godot::Image> image;
		image.instantiate();
		if (image->load_dds_from_buffer(bytes) != godot::OK) {
			r_texture = godot::Ref<godot::Texture2D>();
			return true;
		}
		r_texture = texture_from_image(image);
		return true;
	}

	if ((ext == "tga" || ext == "mdt" || ext == "dds") && !godot::bytes_look_like_dds(bytes)) {
		if (bytes.size() < 18) {
			r_texture = godot::Ref<godot::Texture2D>();
			return true;
		}
		godot::Ref<godot::Image> image;
		image.instantiate();
		if (image->load_tga_from_buffer(bytes) != godot::OK) {
			r_texture = godot::Ref<godot::Texture2D>();
			return true;
		}
		r_texture = texture_from_image(image);
		return true;
	}
	return false;
}

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
	// with "DDS " must decode as DDS. decode_dds_or_tga is the one decoder both
	// raw-bytes loaders share (the extension-gated version rendered these
	// object textures white).
	godot::Ref<godot::Texture2D> claimed;
	if (decode_dds_or_tga(ext, bytes, claimed)) {
		return claimed;
	}

	godot::Ref<godot::Image> image;
	image.instantiate();
	if (image->load(path) != godot::OK) {
		return godot::Ref<godot::Texture2D>();
	}
	return texture_from_image(image);
}

} // namespace

// Candidate FILENAMES (not full paths) in probe order: the engine's rule
// (base/resource_index/texture_candidates.h), which opennova-3di shares.
std::vector<godot::String> texture_candidate_filenames(const godot::String &filename) {
	std::vector<godot::String> candidates;
	for (const std::string &name : opennova::texture_candidate_filenames(to_std(filename)))
		candidates.push_back(godot::String::utf8(name.c_str()));
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
		auto it = index.find(to_std(file.to_lower()));
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
		// for .tga — the only path that survives export (raw .tga sources are not
		// packed); a candidate ResourceLoader cannot see is skipped.
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
		auto it = index.find(to_std(file.to_lower()));
		if (it != index.end()) {
			const godot::String resolved = dir.path_join(it->second);
			const std::string tex_key = to_std(resolved);
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
	// decompresses the BC payload before generating mipmaps (decode_dds_or_tga).
	godot::Ref<godot::Texture2D> claimed;
	if (decode_dds_or_tga(ext, bytes, claimed)) {
		return claimed;
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

namespace {
// The textures whose mip chain came from their DDS file (the resolver's own
// table). ObjectIDs are never reused within a session, so the table outlives
// cache clears: a material bound before a clear keeps a correct answer.
std::unordered_set<uint64_t> g_authored_mip_chains;
} // namespace

float material_texture_max_lod(const godot::Ref<godot::Texture> &texture) {
	constexpr float kUnbounded = 1000.0f;
	const godot::Ref<godot::Texture2D> texture_2d = texture;
	if (texture_2d.is_null() ||
			g_authored_mip_chains.count(texture_2d->get_instance_id()) != 0) {
		return kUnbounded;
	}
	const uint32_t levels = renderer::pixel_texture_mip_levels(
			static_cast<uint32_t>(texture_2d->get_width()),
			static_cast<uint32_t>(texture_2d->get_height()));
	return levels == 0 ? kUnbounded : static_cast<float>(levels - 1);
}

godot::Ref<godot::Texture2D> load_material_image_from_bytes(
		renderer::MaterialImageDecoder decoder, const godot::PackedByteArray &bytes) {
	using renderer::MaterialImageDecoder;
	if (bytes.is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}
	switch (decoder) {
		case MaterialImageDecoder::Dds: {
			// D3DXCreateTextureFromFileInMemoryEx keeps the file's mip levels;
			// only a chain-less DDS gets generated ones.
			if (!godot::bytes_look_like_dds(bytes)) {
				return godot::Ref<godot::Texture2D>();
			}
			godot::Ref<godot::Image> image;
			image.instantiate();
			if (image->load_dds_from_buffer(bytes) != godot::OK || image->is_empty()) {
				return godot::Ref<godot::Texture2D>();
			}
			if (image->is_compressed() && image->decompress() != godot::OK) {
				return godot::Ref<godot::Texture2D>();
			}
			if (!image->has_mipmaps()) {
				image->generate_mipmaps();
			}
			godot::Ref<godot::ImageTexture> texture = godot::ImageTexture::create_from_image(image);
			if (texture.is_valid()) {
				g_authored_mip_chains.insert(texture->get_instance_id());
			}
			return texture;
		}
		case MaterialImageDecoder::Tga: {
			godot::Ref<godot::Texture2D> claimed;
			return decode_dds_or_tga("tga", bytes, claimed) ? claimed
					: godot::Ref<godot::Texture2D>();
		}
		case MaterialImageDecoder::Pcx:
			return texture_from_image(decode_pcx_image(bytes.ptr(), static_cast<size_t>(bytes.size())));
		case MaterialImageDecoder::None:
			break;
	}
	return godot::Ref<godot::Texture2D>();
}

namespace {
std::unordered_map<std::string, godot::Ref<godot::Texture>> g_material_cache;

godot::Ref<godot::Texture> upload_material_pixels(
        const renderer::MaterialTexturePixels &pixels, bool volume) {
    if (!pixels) return {};
    const size_t slice_size = size_t(pixels.width) * pixels.height * 4;
    godot::TypedArray<godot::Ref<godot::Image>> slices;
    for (uint32_t z = 0; z < pixels.depth; ++z) {
        godot::PackedByteArray data;
        data.resize(slice_size);
        std::copy_n(pixels.rgba.data() + z * slice_size, slice_size, data.ptrw());
        slices.push_back(godot::Image::create_from_data(pixels.width, pixels.height,
                false, godot::Image::FORMAT_RGBA8, data));
    }
    if (!volume) return texture_from_image(slices[0]);
    godot::Ref<godot::ImageTexture3D> texture;
    texture.instantiate();
    if (texture->create(godot::Image::FORMAT_RGBA8, pixels.width, pixels.height,
            pixels.depth, false, slices) != godot::OK) return {};
    return texture;
}
} // namespace

godot::Ref<godot::Texture> prepare_material_texture(
        const godot::Ref<godot::Texture2D> &source,
        const godot::String &name, uint8_t type) {
    using namespace opennova::renderer;
    const auto mode = material_texture_transform(type, name.utf8().get_data(), source.is_valid());
    if (mode == MaterialTextureTransform::Unchanged) return source;
    const std::string key = mode == MaterialTextureTransform::Checkerboard ? "material:checkerboard"
            : "material:" + std::to_string(type) + ":" + std::to_string(source->get_instance_id());
    const auto cached = g_material_cache.find(key);
    if (cached != g_material_cache.end()) return cached->second;
    MaterialTexturePixels pixels;
    if (mode == MaterialTextureTransform::Checkerboard) {
        pixels = {kMissingMaterialTextureSide, kMissingMaterialTextureSide, 1, missing_material_texture_rgba()};
    } else {
        godot::Ref<godot::Image> image = source->get_image();
        if (image.is_null()) return prepare_material_texture({}, name, type);
        if (image->is_compressed() && image->decompress() != godot::OK)
            return prepare_material_texture({}, name, type);
        image->convert(godot::Image::FORMAT_RGBA8);
        const godot::PackedByteArray rgba = image->get_data();
        const uint32_t width = image->get_width(), height = image->get_height();
        switch (mode) {
            case MaterialTextureTransform::NormalFromAlpha:
                pixels = {width, height, 1, normal_map_from_height_rgba(rgba.ptr(), width, height, 1.0f / 64.0f, 3, 2)};
                break;
            case MaterialTextureTransform::HorizonVolume:
                pixels = horizon_volume_from_height(rgba.ptr(), width, height); break;
            case MaterialTextureTransform::AmbientOcclusion:
                pixels = ambient_occlusion_from_height(rgba.ptr(), width, height); break;
            default: break;
        }
    }
    if (!pixels) return prepare_material_texture({}, name, type);
    const auto texture = upload_material_pixels(pixels, mode == MaterialTextureTransform::HorizonVolume);
    if (texture.is_null()) return prepare_material_texture({}, name, type);
    g_material_cache.emplace(key, texture);
    return texture;
}

godot::Ref<godot::Texture> prepare_material_chunk(const godot::PackedByteArray &bytes, uint8_t type) {
    const auto pixels = renderer::load_material_chunk(bytes.ptr(), bytes.size(), type);
    const auto texture = upload_material_pixels(pixels, type == 17);
    return texture.is_valid() ? texture : prepare_material_texture({}, {}, 0);
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
	auto it = index.find(to_std(name.to_lower()));
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

godot::Ref<godot::Texture> load_material_texture_from_dir(
        const godot::String &dir, const godot::String &name, uint8_t type) {
    if (type >= 16 && type <= 18) {
        const auto path = resolve_file_in_dir(dir, name);
        return prepare_material_chunk(path.is_empty() ? godot::PackedByteArray() :
                godot::FileAccess::get_file_as_bytes(path), type);
    }
    if (type < 4 || type > 7) {
        // A loose directory is the only source, so no archive entry competes
        // with a loose file and the DDS sibling rule decides alone.
        const std::string native = to_std(name);
        renderer::MaterialImageSource selected;
        if (type == 1) {
            selected = renderer::plain_material_image_source(native);
        } else {
            const std::string query = renderer::material_texture_query(native);
            selected = renderer::material_image_source(query, false,
                    !resolve_file_in_dir(dir, to_gd(renderer::material_dds_sibling(query))).is_empty());
        }
        const godot::String path = resolve_file_in_dir(dir, to_gd(selected.file));
        godot::Ref<godot::Texture2D> source;
        if (!path.is_empty() && selected.decoder != renderer::MaterialImageDecoder::None) {
            const std::string key = "material-image:" + to_std(path);
            auto cached = g_texture_cache.find(key);
            if (cached == g_texture_cache.end()) {
                godot::PackedByteArray bytes;
                godot::Ref<godot::Texture2D> loaded;
                if (is_resource_dir(dir)) {
                    loaded = godot::ResourceLoader::get_singleton()->load(path);
                } else if (godot::read_nova_payload_file(path, bytes)) {
                    loaded = load_material_image_from_bytes(selected.decoder, bytes);
                }
                cached = g_texture_cache.emplace(key, loaded).first;
            }
            source = cached->second;
        }
        return prepare_material_texture(source, name, type);
    }
    const bool loose_tga = !resolve_file_in_dir(dir, name).is_empty();
    const godot::String dds = name.get_basename() + ".dds";
    const std::string selected = renderer::normal_material_filename(name.utf8().get_data(),
            loose_tga, !resolve_file_in_dir(dir, dds).is_empty());
    const godot::String path = resolve_file_in_dir(dir, to_gd(selected));
    godot::Ref<godot::Texture2D> source;
    if (!path.is_empty()) {
        const std::string key = to_std(path);
        auto cached = g_texture_cache.find(key);
        if (cached == g_texture_cache.end()) {
            godot::Ref<godot::Texture2D> loaded = is_resource_dir(dir)
                ? godot::Ref<godot::Texture2D>(godot::ResourceLoader::get_singleton()->load(path))
                : load_existing_texture_path(path);
            cached = g_texture_cache.emplace(key, loaded).first;
        }
        source = cached->second;
    }
    return prepare_material_texture(source, name, type);
}

void clear_texture_resolver_caches() {
	g_dir_index_cache.clear();
	g_texture_cache.clear();
	g_material_cache.clear();
}

} // namespace opennova
