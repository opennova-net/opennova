#pragma once

// Shared texture utilities: PCX decoding, case-insensitive path resolution,
// and ResourceLoader-based texture loading.

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstdint>
#include <vector>

namespace opennova {

// Decode a PCX from raw bytes into a Godot Image (RGB8).
// Supports 8-bit paletted (1 plane) and 24-bit RGB (3 planes).
static godot::Ref<godot::Image> decode_pcx_image(const uint8_t* data, size_t size) {
	if (size < 128) return godot::Ref<godot::Image>();
	if (data[0] != 0x0A) return godot::Ref<godot::Image>();

	int xmin = data[4] | (data[5] << 8);
	int ymin = data[6] | (data[7] << 8);
	int xmax = data[8] | (data[9] << 8);
	int ymax = data[10] | (data[11] << 8);
	int w = xmax - xmin + 1, h = ymax - ymin + 1;
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return godot::Ref<godot::Image>();
	int nplanes = data[65];
	int bpl = data[66] | (data[67] << 8);
	if (bpl < w) bpl = w;

	godot::PackedByteArray rgb_data;
	rgb_data.resize(w * h * 3);
	uint8_t* dst = rgb_data.ptrw();

	if (nplanes == 3) {
		// 24-bit RGB: 3 planes per scanline, each bpl bytes
		std::vector<uint8_t> scanline(bpl * 3, 0);
		size_t pos = 128;
		for (int row = 0; row < h; row++) {
			int col = 0;
			int total = bpl * 3;
			while (col < total && pos < size) {
				uint8_t byte = data[pos++];
				if ((byte & 0xC0) == 0xC0) {
					int count = byte & 0x3F;
					if (pos >= size) break;
					uint8_t val = data[pos++];
					for (int j = 0; j < count && col < total; j++)
						scanline[col++] = val;
				} else {
					scanline[col++] = byte;
				}
			}
			for (int x = 0; x < w; x++) {
				dst[(row * w + x) * 3 + 0] = scanline[x];
				dst[(row * w + x) * 3 + 1] = scanline[bpl + x];
				dst[(row * w + x) * 3 + 2] = scanline[bpl * 2 + x];
			}
		}
	} else if (nplanes == 1) {
		// 8-bit paletted: 1 plane + 256-color palette appended
		if (size < 128 + 769) return godot::Ref<godot::Image>();

		std::vector<uint8_t> indices(w * h, 0);
		size_t pos = 128;
		for (int row = 0; row < h; row++) {
			int col = 0;
			while (col < bpl && pos < size) {
				uint8_t byte = data[pos++];
				if ((byte & 0xC0) == 0xC0) {
					int count = byte & 0x3F;
					if (pos >= size) break;
					uint8_t val = data[pos++];
					for (int j = 0; j < count && col < bpl; j++) {
						if (col < w) indices[row * w + col] = val;
						col++;
					}
				} else {
					if (col < w) indices[row * w + col] = byte;
					col++;
				}
			}
		}

		const uint8_t* pal_start = data + size - 769;
		if (pal_start[0] != 0x0C) return godot::Ref<godot::Image>();
		const uint8_t* palette = pal_start + 1;

		for (int i = 0; i < w * h; i++) {
			dst[i * 3 + 0] = palette[indices[i] * 3 + 0];
			dst[i * 3 + 1] = palette[indices[i] * 3 + 1];
			dst[i * 3 + 2] = palette[indices[i] * 3 + 2];
		}
	} else {
		return godot::Ref<godot::Image>();
	}

	return godot::Image::create_from_data(w, h, false, godot::Image::FORMAT_RGB8, rgb_data);
}

// Decode PCX into indices AND capture the 256-color palette.
// Returns true on success; fills out_indices, out_palette (768 bytes = 256 RGB), out_w, out_h.
static bool decode_pcx_with_palette(const uint8_t* data, size_t size,
                                    std::vector<uint8_t>& out_indices,
                                    uint8_t out_palette[256][3],
                                    int& out_w, int& out_h) {
	out_w = 0; out_h = 0;
	if (size < 128) return false;
	if (data[0] != 0x0A) return false;

	int xmin = data[4] | (data[5] << 8);
	int ymin = data[6] | (data[7] << 8);
	int xmax = data[8] | (data[9] << 8);
	int ymax = data[10] | (data[11] << 8);
	int w = xmax - xmin + 1, h = ymax - ymin + 1;
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
	int nplanes = data[65];
	if (nplanes != 1) return false;  // palette path requires 8-bit single plane
	int bpl = data[66] | (data[67] << 8);
	if (bpl < w) bpl = w;

	if (size < 128 + 769) return false;

	out_indices.assign(w * h, 0);
	size_t pos = 128;
	for (int row = 0; row < h; row++) {
		int col = 0;
		while (col < bpl && pos < size) {
			uint8_t byte = data[pos++];
			if ((byte & 0xC0) == 0xC0) {
				int count = byte & 0x3F;
				if (pos >= size) break;
				uint8_t val = data[pos++];
				for (int j = 0; j < count && col < bpl; j++) {
					if (col < w) out_indices[row * w + col] = val;
					col++;
				}
			} else {
				if (col < w) out_indices[row * w + col] = byte;
				col++;
			}
		}
	}

	const uint8_t* pal_start = data + size - 769;
	if (pal_start[0] != 0x0C) return false;
	const uint8_t* palette = pal_start + 1;
	for (int i = 0; i < 256; i++) {
		out_palette[i][0] = palette[i * 3 + 0];
		out_palette[i][1] = palette[i * 3 + 1];
		out_palette[i][2] = palette[i * 3 + 2];
	}

	out_w = w;
	out_h = h;
	return true;
}


// Encode 8-bit palette-indexed PCX (v5, 1 plane, RLE).
// indices is width*height bytes; palette is 256 RGB triplets (768 bytes).
static godot::PackedByteArray encode_pcx_indices(const uint8_t* indices, int width, int height,
                                                  const uint8_t palette[256][3]) {
	godot::PackedByteArray out;
	if (indices == nullptr || width <= 0 || height <= 0) return out;

	int bpl = width + (width & 1);  // PCX bytes-per-line must be even

	// Header — 128 bytes
	uint8_t header[128] = {};
	header[0] = 0x0A;               // manufacturer
	header[1] = 5;                  // version (5 = Paintbrush 3.0, supports 256-color palette)
	header[2] = 1;                  // encoding (RLE)
	header[3] = 8;                  // bits per pixel per plane
	header[4] = 0; header[5] = 0;   // xmin
	header[6] = 0; header[7] = 0;   // ymin
	header[8] = (width - 1) & 0xFF;  header[9] = ((width - 1) >> 8) & 0xFF;   // xmax
	header[10] = (height - 1) & 0xFF; header[11] = ((height - 1) >> 8) & 0xFF; // ymax
	header[12] = 72; header[13] = 0;  // hdpi
	header[14] = 72; header[15] = 0;  // vdpi
	// bytes 16..63: 16-color palette, unused for 256-color — leave zero
	header[64] = 0;                 // reserved
	header[65] = 1;                 // color planes
	header[66] = bpl & 0xFF; header[67] = (bpl >> 8) & 0xFF;  // bytes per line
	header[68] = 1; header[69] = 0; // palette info (1 = color)
	// bytes 70..127: zero
	for (int i = 0; i < 128; i++) out.push_back(header[i]);

	// RLE-compressed scanlines
	std::vector<uint8_t> row(bpl, 0);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) row[x] = indices[y * width + x];
		// Trailing byte padding (if bpl > width) is zero
		int pos = 0;
		while (pos < bpl) {
			uint8_t value = row[pos];
			int run = 1;
			while (pos + run < bpl && run < 63 && row[pos + run] == value) run++;
			if (run > 1 || (value & 0xC0) == 0xC0) {
				out.push_back(0xC0 | run);
				out.push_back(value);
			} else {
				out.push_back(value);
			}
			pos += run;
		}
	}

	// 256-color palette marker + 768-byte palette
	out.push_back(0x0C);
	for (int i = 0; i < 256; i++) {
		out.push_back(palette[i][0]);
		out.push_back(palette[i][1]);
		out.push_back(palette[i][2]);
	}

	return out;
}


// Build an RGB Texture2D from 8-bit palette indices (used as a preview cache).
static godot::Ref<godot::Texture2D> build_indexed_texture(const std::vector<uint8_t>& indices,
                                                          const uint8_t palette[256][3],
                                                          int width, int height) {
	if ((int)indices.size() < width * height || width <= 0 || height <= 0)
		return godot::Ref<godot::Texture2D>();
	godot::PackedByteArray rgb;
	rgb.resize(width * height * 3);
	uint8_t* dst = rgb.ptrw();
	for (int i = 0; i < width * height; i++) {
		uint8_t idx = indices[i];
		dst[i * 3 + 0] = palette[idx][0];
		dst[i * 3 + 1] = palette[idx][1];
		dst[i * 3 + 2] = palette[idx][2];
	}
	godot::Ref<godot::Image> img = godot::Image::create_from_data(width, height, false, godot::Image::FORMAT_RGB8, rgb);
	if (img.is_null()) return godot::Ref<godot::Texture2D>();
	godot::Ref<godot::ImageTexture> tex = godot::ImageTexture::create_from_image(img);
	return tex;
}


// Decode PCX palette indices only (for foliage map placement).
static std::vector<uint8_t> decode_pcx_indices(const uint8_t* data, size_t size, int& out_w, int& out_h) {
	out_w = 0; out_h = 0;
	if (size < 128) return {};
	if (data[0] != 0x0A) return {};

	int xmin = data[4] | (data[5] << 8);
	int ymin = data[6] | (data[7] << 8);
	int xmax = data[8] | (data[9] << 8);
	int ymax = data[10] | (data[11] << 8);
	int w = xmax - xmin + 1, h = ymax - ymin + 1;
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return {};
	int bpl = data[66] | (data[67] << 8);
	if (bpl < w) bpl = w;

	std::vector<uint8_t> indices(w * h, 0);
	size_t pos = 128;
	for (int row = 0; row < h; row++) {
		int col = 0;
		while (col < bpl && pos < size) {
			uint8_t byte = data[pos++];
			if ((byte & 0xC0) == 0xC0) {
				int count = byte & 0x3F;
				if (pos >= size) break;
				uint8_t val = data[pos++];
				for (int j = 0; j < count && col < bpl; j++) {
					if (col < w && row < h) indices[row * w + col] = val;
					col++;
				}
			} else {
				if (col < w && row < h) indices[row * w + col] = byte;
				col++;
			}
		}
	}

	out_w = w;
	out_h = h;
	return indices;
}

// Extensions to try when resolving texture names. Both casings because
// Godot's resource paths are case-sensitive internally.
static constexpr const char* tex_ext_priority[] = {
	"tga", "TGA", "dds", "DDS", "mdt", "MDT", "pcx", "PCX",
	"png", "PNG", "jpg", "JPG", "jpeg", "JPEG", "bmp", "BMP"
};

// Resolve a texture filename to its actual path in a directory.
// Strips the extension from the input and tries all known texture extensions
// with multiple stem casings. For res:// paths uses ResourceLoader::exists()
// (handles .ctex remap in exports); for absolute/user:// paths falls back to
// FileAccess::file_exists() so .trn files opened from outside res:// resolve.
static godot::String resolve_texture_path(const godot::String& dir, const godot::String& filename) {
	if (filename.is_empty()) return godot::String();

	godot::String stem = filename.get_file().get_basename();
	if (stem.is_empty()) return godot::String();

	// Build stem variants to try: original, lowercase, uppercase
	godot::String stem_lower = stem.to_lower();
	godot::String stem_upper = stem.to_upper();
	const godot::String stems[] = { stem, stem_lower, stem_upper };

	bool is_res_path = dir.begins_with("res://");
	for (const char* ext : tex_ext_priority) {
		for (const auto& s : stems) {
			godot::String try_path = dir.path_join(s + godot::String(".") + ext);
			if (is_res_path) {
				if (godot::ResourceLoader::get_singleton()->exists(try_path))
					return try_path;
			} else {
				if (godot::FileAccess::file_exists(try_path))
					return try_path;
			}
		}
	}

	return godot::String();
}

// Load a texture from a resource directory via ResourceLoader.
// All texture formats have registered loaders (built-in for .tga/.dds/.png,
// ResourceFormatLoaderNovaTexture for .pcx/.mdt), so ResourceLoader handles
// everything — including .ctex remap in exported builds.
static godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String& dir, const godot::String& filename) {
	if (filename.is_empty()) return godot::Ref<godot::Texture2D>();

	godot::String resolved = resolve_texture_path(dir, filename);
	if (resolved.is_empty()) return godot::Ref<godot::Texture2D>();

	return godot::ResourceLoader::get_singleton()->load(resolved);
}

// Load PCX palette indices from a Godot resource path.
static std::vector<uint8_t> load_pcx_indices(const godot::String& path, int& out_w, int& out_h) {
	out_w = 0; out_h = 0;
	godot::Ref<godot::FileAccess> f = godot::FileAccess::open(path, godot::FileAccess::READ);
	if (f.is_null()) return {};
	godot::PackedByteArray bytes = f->get_buffer(f->get_length());
	f.unref();
	return decode_pcx_indices(bytes.ptr(), bytes.size(), out_w, out_h);
}

// Resolve an asset by name with case-insensitive matching.
// Tries original, lowercase, and uppercase stems with the given extension.
static godot::String resolve_asset_path(const godot::String& dir, const godot::String& name, const char* ext) {
	if (name.is_empty()) return godot::String();

	godot::String stem = name.get_file().get_basename();
	if (stem.is_empty()) stem = name;

	godot::String stem_lower = stem.to_lower();
	godot::String stem_upper = stem.to_upper();
	// Also try capitalised (first char upper, rest lower)
	godot::String stem_cap = stem_lower;
	if (!stem_cap.is_empty()) {
		godot::String first = stem_cap.substr(0, 1).to_upper();
		stem_cap = first + stem_cap.substr(1);
	}
	const godot::String stems[] = { stem, stem_lower, stem_upper, stem_cap };
	const char* exts[] = { ext };

	for (const char* e : exts) {
		for (const auto& s : stems) {
			godot::String try_path = dir.path_join(s + godot::String(".") + e);
			if (godot::ResourceLoader::get_singleton()->exists(try_path))
				return try_path;
			// Also try uppercase extension
			godot::String try_path_upper = dir.path_join(s + godot::String(".") + godot::String(e).to_upper());
			if (godot::ResourceLoader::get_singleton()->exists(try_path_upper))
				return try_path_upper;
		}
	}

	return godot::String();
}

} // namespace opennova
