#include "util/pcx_texture_bridge.h"


#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image_texture.hpp>

#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

#include <cstring>
#include <string>
#include <vector>

namespace opennova {

namespace {

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

} // namespace

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

} // namespace opennova
