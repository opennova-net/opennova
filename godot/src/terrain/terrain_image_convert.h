#pragma once

// Godot image / texture -> the engine's RGBA8 image, shared by the terrain
// surface inputs and the tile-cache device. A failed conversion leaves the
// output empty.

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <runtime/terrain/texture_preprocess.h>

#include <cstddef>
#include <cstdint>

namespace godot {

inline bool image_to_rgba8(const Ref<Image> &p_source,
		opennova::terrain::Rgba8Image &r_output) {
	r_output = {};
	if (p_source.is_null() || p_source->is_empty()) {
		return false;
	}
	Ref<Image> image = p_source->duplicate();
	if (image.is_null() ||
			(image->is_compressed() && image->decompress() != OK)) {
		return false;
	}
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	const int width = image->get_width();
	const int height = image->get_height();
	if (width <= 0 || height <= 0) {
		return false;
	}
	const size_t byte_count = static_cast<size_t>(width) * height * 4u;
	const PackedByteArray bytes = image->get_data();
	if (bytes.size() < static_cast<int64_t>(byte_count)) {
		return false;
	}
	r_output.width = static_cast<uint32_t>(width);
	r_output.height = static_cast<uint32_t>(height);
	r_output.pixels.assign(bytes.ptr(), bytes.ptr() + byte_count);
	return true;
}

inline bool texture_to_rgba8(const Ref<Texture2D> &p_texture,
		opennova::terrain::Rgba8Image &r_output) {
	return p_texture.is_valid() && image_to_rgba8(p_texture->get_image(), r_output);
}

} // namespace godot
