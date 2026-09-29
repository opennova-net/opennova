#include <editor/preview/texture_header.h>

#include <cstring>

#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

namespace opennova::editor {

namespace {

uint32_t be32(const std::vector<uint8_t> &b, size_t at) {
	return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | uint32_t(b[at + 3]);
}

} // namespace

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
	case menu::MenuTextureFormat::Png: {
		static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		if (bytes.size() < 24 || std::memcmp(bytes.data(), kSignature, 8) != 0 ||
		    std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
			return false;
		w = be32(bytes, 16);
		h = be32(bytes, 20);
		break;
	}
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
