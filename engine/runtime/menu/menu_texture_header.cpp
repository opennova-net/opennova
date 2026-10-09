#include <runtime/menu/menu_texture_header.h>

#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_decode.h>
#include <formats/tga/tga.h>

namespace opennova::menu {

bool menu_texture_header_size(MenuTextureFormat format, const std::vector<uint8_t> &bytes, int *width,
                              int *height) {
	uint32_t w = 0, h = 0;
	switch (format) {
	case MenuTextureFormat::Tga:
		if (!tga::tga_header_size(bytes.data(), bytes.size(), w, h)) return false;
		break;
	case MenuTextureFormat::Dds:
		if (!dds::dds_header_size(bytes.data(), bytes.size(), w, h)) return false;
		break;
	case MenuTextureFormat::Png:
		if (!png::png_header_size(bytes, w, h)) return false;
		break;
	case MenuTextureFormat::Pcx: {
		// The game's own decode, its size what the menu's load comes to [orig: load_pcx_to_argb
		// @ 0x664cc0 via CTextureManager_LoadOrFindTexture @ 0x654980].
		RgbaImage image;
		std::string error;
		if (!decode_pcx_menu_rgba(bytes.data(), bytes.size(), image, error) || image.empty()) return false;
		w = uint32_t(image.width);
		h = uint32_t(image.height);
		break;
	}
	case MenuTextureFormat::None:
		return false;
	}
	if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return false;
	*width = int(w);
	*height = int(h);
	return true;
}

bool MenuTextureHeaderProbe::decode(const std::string &, MenuTextureFormat format, const std::vector<uint8_t> &bytes,
                                    int &width, int &height) {
	return menu_texture_header_size(format, bytes, &width, &height);
}

void MenuTextureHeaderProbe::release(const std::string &) {}

} // namespace opennova::menu
