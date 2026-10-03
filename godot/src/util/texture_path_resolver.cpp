#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga_read.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_load_rules.h>
#include "util/texture_path_resolver.h"

#include "util/data_format.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/image_texture3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <algorithm>
#include <cstring>
#include <initializer_list>
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

godot::Ref<godot::Image> rgba_image(int width, int height, const uint8_t *pixels, size_t size) {
	if (width <= 0 || height <= 0 || size < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
		return godot::Ref<godot::Image>();
	}
	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u));
	std::memcpy(data.ptrw(), pixels, static_cast<size_t>(data.size()));
	return godot::Image::create_from_data(width, height, false, godot::Image::FORMAT_RGBA8, data);
}

// The game's TGA reader straight into the image's own bytes (one buffer, no copy).
godot::Ref<godot::Image> read_tga(const godot::PackedByteArray &bytes, tga::TgaReaderForm form) {
	int width = 0, height = 0;
	std::string error;
	const uint8_t *data = bytes.ptr();
	const size_t size = static_cast<size_t>(bytes.size());
	if (!tga::tga_retail_size(data, size, width, height, error, form)) {
		return godot::Ref<godot::Image>();
	}
	godot::PackedByteArray pixels;
	pixels.resize(static_cast<int64_t>(width) * height * 4);
	if (!tga::tga_decode_retail_into(data, size, pixels.ptrw(), error, form)) {
		return godot::Ref<godot::Image>();
	}
	return godot::Image::create_from_data(width, height, false, godot::Image::FORMAT_RGBA8, pixels);
}

bool starts_with(const godot::PackedByteArray &bytes, std::initializer_list<uint8_t> magic) {
	if (static_cast<size_t>(bytes.size()) < magic.size()) {
		return false;
	}
	size_t i = 0;
	for (const uint8_t byte : magic) {
		if (bytes[static_cast<int64_t>(i++)] != byte) {
			return false;
		}
	}
	return true;
}

// Whether the bytes carry a TGA header D3DX's TGA codec would take: a colour-map type
// of 0 or 1, one of the image types it reads and a pixel depth it knows. A check before
// Godot's decoder so a file that is no TGA fails quietly, as the codec does.
bool looks_like_tga(const godot::PackedByteArray &bytes) {
	if (bytes.size() < 18) {
		return false;
	}
	const uint8_t map_type = bytes[1];
	const uint8_t image_type = bytes[2];
	const uint8_t depth = bytes[16];
	const bool type_ok = image_type == 1 || image_type == 2 || image_type == 3 ||
			image_type == 9 || image_type == 10 || image_type == 11;
	const bool depth_ok = depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32;
	return map_type <= 1 && type_ok && depth_ok;
}

// The "DDS" reader: D3DX decodes the bytes by their content, its codecs tried in turn
// (renderer::dds_reader_codec_order). Godot decodes BMP, DDS, JPEG, PNG and TGA; its
// TGA decoder honours the origin bit as D3DX's codec does.
godot::Ref<godot::Image> read_dds_by_content(const godot::PackedByteArray &bytes) {
	for (const renderer::DdsCodec codec : renderer::dds_reader_codec_order()) {
		godot::Ref<godot::Image> image;
		image.instantiate();
		godot::Error err = godot::ERR_FILE_UNRECOGNIZED;
		switch (codec) {
			case renderer::DdsCodec::Bmp:
				if (starts_with(bytes, { 'B', 'M' })) err = image->load_bmp_from_buffer(bytes);
				break;
			case renderer::DdsCodec::Dds:
				if (godot::bytes_look_like_dds(bytes)) err = image->load_dds_from_buffer(bytes);
				break;
			case renderer::DdsCodec::Jpeg:
				if (starts_with(bytes, { 0xFF, 0xD8, 0xFF })) err = image->load_jpg_from_buffer(bytes);
				break;
			case renderer::DdsCodec::Png:
				if (starts_with(bytes, { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A })) {
					err = image->load_png_from_buffer(bytes);
				}
				break;
			case renderer::DdsCodec::Tga:
				if (looks_like_tga(bytes)) err = image->load_tga_from_buffer(bytes);
				break;
			case renderer::DdsCodec::Ppm:
			case renderer::DdsCodec::Pfm:
			case renderer::DdsCodec::Hdr:
			case renderer::DdsCodec::Dib:
				break;
		}
		if (err == godot::OK && !image->is_empty()) {
			return image;
		}
	}
	return godot::Ref<godot::Image>();
}

// The reader alone: RGBA8 and null when the bytes do not decode. Only a DDS-reader
// image keeps a mip chain, the one its file carries.
godot::Ref<godot::Image> read_image(renderer::TextureReader reader, const godot::PackedByteArray &bytes) {
	if (bytes.is_empty()) {
		return godot::Ref<godot::Image>();
	}
	const uint8_t *data = bytes.ptr();
	const size_t size = static_cast<size_t>(bytes.size());
	std::string error;
	godot::Ref<godot::Image> image;
	switch (reader) {
		case renderer::TextureReader::Tga:
			return read_tga(bytes, tga::TgaReaderForm::Archive);
		case renderer::TextureReader::TgaParticleLoose:
			return read_tga(bytes, tga::TgaReaderForm::ParticleLoose);
		case renderer::TextureReader::Pcx: {
			RgbaImage decoded;
			if (!decode_pcx_menu_rgba(data, size, decoded, error)) {
				return godot::Ref<godot::Image>();
			}
			return rgba_image(decoded.width, decoded.height, decoded.pixels.data(), decoded.pixels.size());
		}
		case renderer::TextureReader::Dds:
			image = read_dds_by_content(bytes);
			break;
		case renderer::TextureReader::Png:
			image.instantiate();
			if (image->load_png_from_buffer(bytes) != godot::OK) {
				return godot::Ref<godot::Image>();
			}
			break;
		case renderer::TextureReader::None:
			return godot::Ref<godot::Image>();
	}
	if (image.is_null() || image->is_empty()) {
		return godot::Ref<godot::Image>();
	}
	if (image->is_compressed() && image->decompress() != godot::OK) {
		return godot::Ref<godot::Image>();
	}
	if (reader != renderer::TextureReader::Dds && image->has_mipmaps()) {
		image->clear_mipmaps();
	}
	if (image->get_format() != godot::Image::FORMAT_RGBA8) {
		image->convert(godot::Image::FORMAT_RGBA8);
	}
	return image;
}

} // namespace

godot::Ref<godot::Image> decode_texture_load(const renderer::TextureLoad &load,
		const godot::PackedByteArray &bytes) {
	godot::Ref<godot::Image> image = read_image(load.reader, bytes);
	if (image.is_null()) {
		return image;
	}
	if (load.transform == renderer::TextureLoadTransform::PaletteLuminanceAlpha) {
		// The colour stays the PCX reader's; the alpha is the 8-bit read's palette
		// luminance, kept only when that read succeeds (renderer::archive_texture_load).
		RgbaImage luminance;
		std::string error;
		if (decode_pcx_luminance_alpha(bytes.ptr(), static_cast<size_t>(bytes.size()), luminance, error) &&
				luminance.width == image->get_width() && luminance.height == image->get_height()) {
			godot::PackedByteArray data = image->get_data();
			uint8_t *pixels = data.ptrw();
			for (size_t i = 0; i + 3 < static_cast<size_t>(data.size()) && i + 3 < luminance.pixels.size(); i += 4) {
				pixels[i + 3] = luminance.pixels[i + 3];
			}
			image->set_data(image->get_width(), image->get_height(), false, godot::Image::FORMAT_RGBA8, data);
		}
	}
	if (load.transform == renderer::TextureLoadTransform::WhiteAlphaFromBlue || load.alpha_only) {
		if (image->has_mipmaps()) {
			image->clear_mipmaps();
		}
		godot::PackedByteArray data = image->get_data();
		renderer::apply_texture_load_transform(load.transform, load.alpha_only, data.ptrw(),
				static_cast<size_t>(data.size()) / 4u);
		image->set_data(image->get_width(), image->get_height(), false, godot::Image::FORMAT_RGBA8, data);
	}
	return image;
}

godot::Ref<godot::Image> load_texture_image(const std::vector<renderer::TextureLoad> &attempts,
		const TextureReadFn &read, bool *r_alpha_only) {
	if (r_alpha_only != nullptr) {
		*r_alpha_only = false;
	}
	if (!read) {
		return godot::Ref<godot::Image>();
	}
	for (const renderer::TextureLoad &load : attempts) {
		const godot::Ref<godot::Image> image = decode_texture_load(load, read(load));
		if (image.is_valid()) {
			if (r_alpha_only != nullptr) {
				*r_alpha_only = load.alpha_only;
			}
			return image;
		}
	}
	return godot::Ref<godot::Image>();
}

std::string texture_load_key(const std::vector<renderer::TextureLoad> &attempts) {
	std::string key;
	for (const renderer::TextureLoad &load : attempts) {
		key += std::to_string(static_cast<int>(load.source)) + "/" +
				std::to_string(static_cast<int>(load.reader)) + "/" +
				std::to_string(static_cast<int>(load.transform)) + "/" + (load.alpha_only ? "a" : "c") +
				"/" + to_std(to_gd(load.file).to_lower()) + "|";
	}
	return key;
}

godot::Ref<godot::Texture2D> texture_with_mipmaps(const godot::Ref<godot::Image> &image) {
	if (image.is_null() || image->is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}
	if (image->is_compressed()) {
		image->decompress();
	}
	// A DDS keeps the chain its file carries (D3DX's MipLevels 0).
	if (!image->has_mipmaps()) {
		image->generate_mipmaps();
	}
	return godot::ImageTexture::create_from_image(image);
}

godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename,
		renderer::TextureLoader loader) {
	if (filename.is_empty()) {
		return godot::Ref<godot::Texture2D>();
	}
	if (is_resource_dir(dir)) {
		// res:// holds the project's own imported textures, not retail data:
		// ResourceLoader resolves the imported texture, the only form that
		// survives export.
		const godot::String path = dir.path_join(filename);
		godot::ResourceLoader *resource_loader = godot::ResourceLoader::get_singleton();
		return resource_loader->exists(path) ? godot::Ref<godot::Texture2D>(resource_loader->load(path))
											  : godot::Ref<godot::Texture2D>();
	}
	// Absolute/external original-asset path: decode the raw bytes the retail way. A
	// loose directory is the only source, so no loose-first search competes; the
	// particle folder is the directory's "tga" folder.
	renderer::TextureFileQuery files;
	files.exists = [&dir](const std::string &name) {
		return !resolve_file_in_dir(dir, to_gd(name)).is_empty();
	};
	const std::vector<renderer::TextureLoad> attempts =
			renderer::texture_load_attempts(loader, to_std(filename), files);
	// Cached directory listing + decoded-texture cache, keyed by the files and readers
	// the loader resolved, so a shared texture is decoded and uploaded once.
	const std::string key = "dir:" + to_std(dir) + ":" + texture_load_key(attempts);
	const auto cached = g_texture_cache.find(key);
	if (cached != g_texture_cache.end()) {
		return cached->second;
	}
	const auto read = [&dir](const renderer::TextureLoad &load) {
		godot::PackedByteArray bytes;
		if (load.source == renderer::TextureFileSource::ParticleTextureDir) {
			const godot::String path = resolve_file_in_dir(dir.path_join("tga"), to_gd(load.file));
			return path.is_empty() ? godot::PackedByteArray() : godot::FileAccess::get_file_as_bytes(path);
		}
		const godot::String path = resolve_file_in_dir(dir, to_gd(load.file));
		if (path.is_empty() || !godot::read_nova_payload_file(path, bytes)) {
			return godot::PackedByteArray();
		}
		return bytes;
	};
	const godot::Ref<godot::Texture2D> texture = texture_with_mipmaps(load_texture_image(attempts, read));
	// Cache the null too: a missing or undecodable file is not re-read per probe.
	g_texture_cache.emplace(key, texture);
	return texture;
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
		const renderer::TextureLoad &load, const godot::PackedByteArray &bytes) {
	if (bytes.is_empty() || load.reader == renderer::TextureReader::None) {
		return godot::Ref<godot::Texture2D>();
	}
	if (load.reader != renderer::TextureReader::Dds) {
		return texture_with_mipmaps(decode_texture_load(load, bytes));
	}
	// D3DXCreateTextureFromFileInMemoryEx keeps the file's mip levels; only a
	// chain-less DDS gets generated ones. Bytes that are no DDS go through the
	// reader's other codecs (renderer::dds_reader_codec_order).
	if (!godot::bytes_look_like_dds(bytes)) {
		return texture_with_mipmaps(decode_texture_load(load, bytes));
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

renderer::TextureLoad normal_material_load(const godot::String &name, const std::string &selected) {
	renderer::TextureLoad load;
	load.file = selected;
	if (selected != to_std(name)) {
		load.reader = renderer::TextureReader::Dds;
	} else {
		const godot::String upper = name.to_upper();
		if (upper.contains(".MDT") || upper.contains(".TGA")) {
			load.reader = renderer::TextureReader::Tga;
		}
	}
	return load;
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
    if (!volume) return texture_with_mipmaps(slices[0]);
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
    // A row whose texture loads with a side cap (the normal maps and the occlusion
    // producer: renderer::material_texture_side_cap) is halved to it.
    const uint32_t cap = material_texture_side_cap(type);
    const bool over_cap = cap != 0 && source.is_valid() &&
            (static_cast<uint32_t>(source->get_width()) > cap ||
             static_cast<uint32_t>(source->get_height()) > cap);
    if (mode == MaterialTextureTransform::Unchanged && !over_cap) return source;
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
        if (image->has_mipmaps()) image->clear_mipmaps();
        image->convert(godot::Image::FORMAT_RGBA8);
        const godot::PackedByteArray rgba = image->get_data();
        const uint32_t width = image->get_width(), height = image->get_height();
        switch (mode) {
            case MaterialTextureTransform::Unchanged:
                pixels = {width, height, 1, std::vector<uint8_t>(rgba.ptr(), rgba.ptr() + rgba.size())};
                break;
            case MaterialTextureTransform::NormalFromAlpha:
                pixels = {width, height, 1, normal_map_from_height_rgba(rgba.ptr(), width, height, 1.0f / 64.0f, 3, 2)};
                break;
            case MaterialTextureTransform::HorizonVolume:
                pixels = horizon_volume_from_height(rgba.ptr(), width, height); break;
            case MaterialTextureTransform::AmbientOcclusion:
                pixels = ambient_occlusion_from_height(rgba.ptr(), width, height); break;
            default: break;
        }
        if (cap != 0 && pixels && pixels.depth == 1)
            halve_rgba_to_cap(pixels.rgba, pixels.width, pixels.height, cap);
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
    // A loose directory is the only source, so no archive entry competes
    // with a loose file and the DDS sibling rule decides alone.
    renderer::TextureLoad selected;
    if (type < 4 || type > 7) {
        const std::string native = to_std(name);
        if (type == 1) {
            selected = renderer::plain_texture_load(native);
        } else {
            const std::string query = renderer::material_texture_query(native);
            selected = renderer::stage_texture_load(query, false,
                    !resolve_file_in_dir(dir, to_gd(renderer::material_dds_sibling(query))).is_empty());
        }
    } else {
        const godot::String dds = name.get_basename() + ".dds";
        selected = normal_material_load(name, renderer::normal_material_filename(to_std(name),
                !resolve_file_in_dir(dir, name).is_empty(), !resolve_file_in_dir(dir, dds).is_empty()));
    }
    const godot::String path = resolve_file_in_dir(dir, to_gd(selected.file));
    godot::Ref<godot::Texture2D> source;
    if (!path.is_empty() && selected.reader != renderer::TextureReader::None) {
        const std::string key = "material-image:" + std::to_string(static_cast<int>(selected.transform)) +
                ":" + to_std(path);
        auto cached = g_texture_cache.find(key);
        if (cached == g_texture_cache.end()) {
            godot::PackedByteArray bytes;
            godot::Ref<godot::Texture2D> loaded;
            if (is_resource_dir(dir)) {
                loaded = godot::ResourceLoader::get_singleton()->load(path);
            } else if (godot::read_nova_payload_file(path, bytes)) {
                loaded = load_material_image_from_bytes(selected, bytes);
            }
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
