#include "pcx_texture_bridge.h"

#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image_texture.hpp>

#include <pcx/pcx.h>
#include <pcx/pcx_io.h>

#include <cstring>
#include <string>
#include <vector>

namespace opennova {

namespace {

godot::Ref<godot::Image> make_rgb_image(const RgbImage &image) {
	if (image.empty()) {
		return godot::Ref<godot::Image>();
	}

	godot::PackedByteArray pixels;
	pixels.resize(static_cast<int>(image.pixels.size()));
	if (!image.pixels.empty()) {
		std::memcpy(pixels.ptrw(), image.pixels.data(), image.pixels.size());
	}

	return godot::Image::create_from_data(image.width, image.height, false, godot::Image::FORMAT_RGB8, pixels);
}

godot::Ref<godot::Image> make_rgb_image(const IndexedImage8 &image) {
	if (image.empty()) {
		return godot::Ref<godot::Image>();
	}

	godot::PackedByteArray pixels;
	pixels.resize(image.width * image.height * 3);
	uint8_t *dst = pixels.ptrw();
	for (int i = 0; i < image.width * image.height; ++i) {
		const uint8_t idx = image.indices[static_cast<size_t>(i)];
		dst[i * 3 + 0] = image.palette[idx][0];
		dst[i * 3 + 1] = image.palette[idx][1];
		dst[i * 3 + 2] = image.palette[idx][2];
	}

	return godot::Image::create_from_data(image.width, image.height, false, godot::Image::FORMAT_RGB8, pixels);
}

bool load_pcx_bytes(const godot::String &path, std::vector<uint8_t> &bytes) {
	godot::PackedByteArray packed;
	if (!godot::read_nova_payload_file(path, packed)) {
		return false;
	}

	bytes.resize(static_cast<size_t>(packed.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), packed.ptr(), static_cast<size_t>(packed.size()));
	}
	return true;
}

} // namespace

godot::Ref<godot::Image> decode_pcx_image(const uint8_t *data, size_t size) {
	RgbImage rgb;
	std::string error;
	if (decode_pcx_rgb(data, size, rgb, error)) {
		return make_rgb_image(rgb);
	}

	IndexedImage8 indexed;
	error.clear();
	if (decode_pcx_indexed(data, size, indexed, error)) {
		return make_rgb_image(indexed);
	}

	return godot::Ref<godot::Image>();
}

godot::Ref<godot::Texture2D> build_pcx_luminance_alpha_texture(const godot::PackedByteArray &bytes) {
	IndexedImage8 indexed;
	std::string error;
	if (!decode_pcx_indexed(bytes.ptr(), static_cast<size_t>(bytes.size()), indexed, error)) {
		return godot::Ref<godot::Texture2D>();
	}

	// Palette luminance table: A[i] = (85 * (r + g + b)) >> 8 per entry, then
	// each pixel's alpha byte is its palette entry's luminance — the sky/effect
	// texture loader's PCX alpha synthesis [orig: load_texture_from_archive
	// @ 0x58b980 — table build @ 0x58bc35..0x58bca9, per-pixel A @ 0x58bcee].
	uint8_t lum[256];
	for (int i = 0; i < 256; ++i) {
		const uint16_t sum = static_cast<uint16_t>(indexed.palette[i][0]) +
				static_cast<uint16_t>(indexed.palette[i][1]) +
				static_cast<uint16_t>(indexed.palette[i][2]);
		lum[i] = static_cast<uint8_t>(static_cast<uint16_t>(85u * sum) >> 8);
	}

	godot::PackedByteArray pixels;
	pixels.resize(indexed.width * indexed.height * 4);
	uint8_t *dst = pixels.ptrw();
	for (int i = 0; i < indexed.width * indexed.height; ++i) {
		const uint8_t idx = indexed.indices[static_cast<size_t>(i)];
		dst[i * 4 + 0] = indexed.palette[idx][0];
		dst[i * 4 + 1] = indexed.palette[idx][1];
		dst[i * 4 + 2] = indexed.palette[idx][2];
		dst[i * 4 + 3] = lum[idx];
	}

	godot::Ref<godot::Image> image = godot::Image::create_from_data(
			indexed.width, indexed.height, false, godot::Image::FORMAT_RGBA8, pixels);
	if (image.is_null()) {
		return godot::Ref<godot::Texture2D>();
	}
	image->generate_mipmaps();
	return godot::ImageTexture::create_from_image(image);
}

bool decode_pcx_with_palette(const uint8_t *data,
                             size_t size,
                             std::vector<uint8_t> &out_indices,
                             uint8_t out_palette[256][3],
                             int &out_w,
                             int &out_h) {
	IndexedImage8 indexed;
	std::string error;
	if (!decode_pcx_indexed(data, size, indexed, error)) {
		out_indices.clear();
		out_w = 0;
		out_h = 0;
		return false;
	}

	out_indices = indexed.indices;
	out_w = indexed.width;
	out_h = indexed.height;
	std::memcpy(out_palette, indexed.palette, sizeof(indexed.palette));
	return true;
}

godot::PackedByteArray encode_pcx_indices(const uint8_t *indices,
                                          int width,
                                          int height,
                                          const uint8_t palette[256][3]) {
	godot::PackedByteArray bytes;
	if (indices == nullptr || width <= 0 || height <= 0) {
		return bytes;
	}

	IndexedImage8 image;
	image.width = width;
	image.height = height;
	image.indices.assign(indices, indices + static_cast<size_t>(width * height));
	std::memcpy(image.palette, palette, sizeof(image.palette));

	std::vector<uint8_t> encoded;
	std::string error;
	if (!encode_pcx_indexed(image, encoded, error)) {
		return bytes;
	}

	bytes.resize(static_cast<int>(encoded.size()));
	if (!encoded.empty()) {
		std::memcpy(bytes.ptrw(), encoded.data(), encoded.size());
	}
	return bytes;
}

godot::Ref<godot::Texture2D> build_indexed_texture(const std::vector<uint8_t> &indices,
                                                   const uint8_t palette[256][3],
                                                   int width,
                                                   int height) {
	if (width <= 0 || height <= 0 || indices.size() < static_cast<size_t>(width * height)) {
		return godot::Ref<godot::Texture2D>();
	}

	IndexedImage8 image;
	image.width = width;
	image.height = height;
	image.indices = indices;
	std::memcpy(image.palette, palette, sizeof(image.palette));

	godot::Ref<godot::Image> rgb = make_rgb_image(image);
	if (rgb.is_null()) {
		return godot::Ref<godot::Texture2D>();
	}
	return godot::ImageTexture::create_from_image(rgb);
}

std::vector<uint8_t> load_pcx_indices(const godot::String &path, int &out_w, int &out_h) {
	out_w = 0;
	out_h = 0;

	std::vector<uint8_t> bytes;
	if (!load_pcx_bytes(path, bytes)) {
		return {};
	}

	IndexedImage8 indexed;
	std::string error;
	if (!decode_pcx_indexed(bytes.data(), bytes.size(), indexed, error)) {
		return {};
	}

	out_w = indexed.width;
	out_h = indexed.height;
	return indexed.indices;
}

} // namespace opennova
