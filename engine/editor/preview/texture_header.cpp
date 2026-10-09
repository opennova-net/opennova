#include <editor/preview/texture_header.h>

#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_decode.h>
#include <formats/tga/tga.h>

namespace opennova::editor {

bool texture_header_size(menu::MenuTextureFormat format, const std::vector<uint8_t> &bytes, int *width,
                         int *height) {
	uint32_t w = 0, h = 0;
	switch (format) {
	case menu::MenuTextureFormat::Tga:
		if (!tga::tga_header_size(bytes.data(), bytes.size(), w, h)) return false;
		break;
	case menu::MenuTextureFormat::Dds:
		if (!dds::dds_header_size(bytes.data(), bytes.size(), w, h)) return false;
		break;
	case menu::MenuTextureFormat::Png:
		if (!png::png_header_size(bytes, w, h)) return false;
		break;
	case menu::MenuTextureFormat::Pcx: {
		RgbaImage image;
		std::string error;
		if (!decode_pcx_menu_rgba(bytes.data(), bytes.size(), image, error) || image.empty()) return false;
		w = uint32_t(image.width);
		h = uint32_t(image.height);
		break;
	}
	case menu::MenuTextureFormat::None:
		return false;
	}
	if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return false;
	*width = int(w);
	*height = int(h);
	return true;
}

bool TextureHeaderProbe::decode(const std::string &, menu::MenuTextureFormat format, const std::vector<uint8_t> &bytes,
                                int &width, int &height) {
	return texture_header_size(format, bytes, &width, &height);
}

void TextureHeaderProbe::release(const std::string &) {}

} // namespace opennova::editor
