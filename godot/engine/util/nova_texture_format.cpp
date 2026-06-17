#include "util/nova_texture_format.h"
#include "util/nova_data_format.h"
#include "util/pcx_texture_bridge.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>

using namespace godot;

PackedStringArray ResourceFormatLoaderNovaTexture::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("tga");
	exts.push_back("TGA");
	exts.push_back("dds");
	exts.push_back("DDS");
	exts.push_back("mdt");
	exts.push_back("MDT");
	exts.push_back("pcx");
	exts.push_back("PCX");
	return exts;
}

bool ResourceFormatLoaderNovaTexture::_handles_type(const StringName &p_type) const {
	return p_type == StringName("Texture2D") || p_type == StringName("ImageTexture") || p_type == StringName("Resource");
}

String ResourceFormatLoaderNovaTexture::_get_resource_type(const String &p_path) const {
	String ext = p_path.get_extension().to_lower();
	if (ext == "tga" || ext == "dds" || ext == "mdt" || ext == "pcx")
		return "ImageTexture";
	return "";
}

Variant ResourceFormatLoaderNovaTexture::_load(const String &p_path, const String &p_original_path,
                                                bool p_use_sub_threads, int32_t p_cache_mode) const {
	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) return Variant();

	String ext = p_path.get_extension().to_lower();
	Ref<Image> img;

	if (ext == "pcx") {
		img = opennova::decode_pcx_image(bytes.ptr(), bytes.size());
	} else if (bytes_look_like_dds(bytes)) {
		// DDS payload by magic, regardless of extension (NovaLogic ships DDS under .tga names).
		img.instantiate();
		if (img->load_dds_from_buffer(bytes) != OK)
			return Variant();
	} else {
		// .tga / .mdt — TGA format in NovaLogic assets
		if (bytes.size() < 18)
			return Variant();
		img.instantiate();
		if (img->load_tga_from_buffer(bytes) != OK)
			return Variant();
	}

	if (img.is_null() || img->is_empty()) return Variant();
	if (img->is_compressed()) img->decompress();
	img->generate_mipmaps();
	return ImageTexture::create_from_image(img);
}
