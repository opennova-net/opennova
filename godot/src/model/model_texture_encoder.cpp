#include "model/model_texture_encoder.h"

#include "util/string_convert.h"

#include <godot_cpp/classes/file_access.hpp>

#include <cstring>
#include <vector>

#include <formats/tga/tga_write.h>

using namespace godot;

void ModelTextureEncoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("encode_image", "image", "with_alpha"), &ModelTextureEncoder::encode_image);
	ClassDB::bind_method(D_METHOD("encode_file", "path", "with_alpha"), &ModelTextureEncoder::encode_file);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ModelTextureEncoder::get_last_error);
}

PackedByteArray ModelTextureEncoder::encode_image(const Ref<Image> &p_image, bool p_with_alpha) {
	PackedByteArray out;
	last_error_ = String();
	if (p_image.is_null() || p_image->is_empty()) {
		last_error_ = "no image";
		return out;
	}
	Ref<Image> image = p_image->duplicate();
	if (image->is_compressed()) {
		if (image->decompress() != OK) {
			last_error_ = "the image is compressed and cannot be decompressed";
			return out;
		}
	}
	image->convert(Image::FORMAT_RGBA8);
	const int width = image->get_width();
	const int height = image->get_height();
	const PackedByteArray rgba = image->get_data();
	if (rgba.size() != static_cast<int64_t>(width) * height * 4) {
		last_error_ = "unexpected RGBA8 payload size";
		return out;
	}
	opennova::tga::TgaImage tga;
	tga.width = width;
	tga.height = height;
	tga.bpp = p_with_alpha ? 32 : 24;
	tga.pixels.reserve(static_cast<size_t>(width) * height * (p_with_alpha ? 4 : 3));
	const uint8_t *src = rgba.ptr();
	for (int64_t i = 0; i < static_cast<int64_t>(width) * height; ++i) {
		tga.pixels.push_back(src[i * 4 + 2]); // B
		tga.pixels.push_back(src[i * 4 + 1]); // G
		tga.pixels.push_back(src[i * 4 + 0]); // R
		if (p_with_alpha) {
			tga.pixels.push_back(src[i * 4 + 3]);
		}
	}
	std::vector<uint8_t> bytes;
	if (!opennova::tga::tga_encode(tga, bytes)) {
		last_error_ = "the TGA encoder refused the image (size or depth)";
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	memcpy(out.ptrw(), bytes.data(), bytes.size());
	return out;
}

PackedByteArray ModelTextureEncoder::encode_file(const String &p_path, bool p_with_alpha) {
	last_error_ = String();
	const PackedByteArray file_bytes = FileAccess::get_file_as_bytes(p_path);
	if (file_bytes.is_empty()) {
		last_error_ = "cannot read " + p_path;
		return PackedByteArray();
	}
	Ref<Image> image;
	image.instantiate();
	Error err = ERR_FILE_UNRECOGNIZED;
	const String extension = p_path.get_extension().to_lower();
	if (extension == "png") {
		err = image->load_png_from_buffer(file_bytes);
	} else if (extension == "tga") {
		err = image->load_tga_from_buffer(file_bytes);
	} else if (extension == "jpg" || extension == "jpeg") {
		err = image->load_jpg_from_buffer(file_bytes);
	} else if (extension == "bmp") {
		err = image->load_bmp_from_buffer(file_bytes);
	} else if (extension == "webp") {
		err = image->load_webp_from_buffer(file_bytes);
	}
	if (err != OK) {
		last_error_ = "cannot decode " + p_path + " (png, tga, jpg, bmp or webp)";
		return PackedByteArray();
	}
	return encode_image(image, p_with_alpha);
}
