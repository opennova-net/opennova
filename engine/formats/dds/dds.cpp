// The DDS writer and header size (dds.h).

#include <formats/dds/dds.h>

#include <base/io/le.h>

#include <cstring>

namespace opennova::dds {

// Every DDS the game reads goes whole to D3DXCreateTextureFromFileInMemoryEx, which takes
// the file's own format: the loader reads the pixel format's FourCC only to pick out
// DXT1/3/5 [orig: GTexture_InitFromMemory @ 0x687DF0, the FourCC @ 0x687E61, the load
// @ 0x687EF5 / @ 0x688315]; a menu's .dds reaches it through TextureSlot_LoadFromDXTFile
// @ 0x6642E0 (GTexture_FindOrCreateByName @ 0x6784F0), a model row's DDS sibling through
// Texture_LoadByNameWithChannel @ 0x58B616. So the file is DirectDraw's own layout: the
// magic, a 124-byte header whose pixel format is 32-bit RGB with alpha under the A8R8G8B8
// masks, and one level of rows, each pixel's dword 0xAARRGGBB stored little-endian.
bool dds_write_a8r8g8b8(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                        std::string &error) {
	out.clear();
	if (rgba == nullptr || width == 0 || height == 0) {
		error = "A DDS needs at least one pixel.";
		return false;
	}
	if (width > 0xFFFFFFFFu / 4) {
		error = "A DDS row this wide does not fit its pitch field.";
		return false;
	}
	out.reserve(DDS_HEADER_SIZE + size_t(width) * height * 4);
	out.insert(out.end(), {'D', 'D', 'S', ' '});
	io::append_u32_le(out, 124); // the header's size
	io::append_u32_le(out, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
	io::append_u32_le(out, height);
	io::append_u32_le(out, width);
	io::append_u32_le(out, width * 4); // the row pitch
	io::append_u32_le(out, 0);         // depth
	io::append_u32_le(out, 0);         // mip levels: none past this one
	for (int i = 0; i < 11; ++i) io::append_u32_le(out, 0); // reserved
	io::append_u32_le(out, 32);        // the pixel format's size
	io::append_u32_le(out, DDPF_RGB | DDPF_ALPHAPIXELS);
	io::append_u32_le(out, 0);         // no FourCC
	io::append_u32_le(out, 32);        // bits a pixel
	io::append_u32_le(out, 0x00FF0000u); // red
	io::append_u32_le(out, 0x0000FF00u); // green
	io::append_u32_le(out, 0x000000FFu); // blue
	io::append_u32_le(out, 0xFF000000u); // alpha
	io::append_u32_le(out, DDSCAPS_TEXTURE);
	for (int i = 0; i < 4; ++i) io::append_u32_le(out, 0); // caps 2 to 4, reserved
	const size_t pixels = size_t(width) * height;
	for (size_t i = 0; i < pixels; ++i) {
		const uint8_t *p = rgba + i * 4;
		out.push_back(p[2]);
		out.push_back(p[1]);
		out.push_back(p[0]);
		out.push_back(p[3]);
	}
	return true;
}

bool dds_header_size(const uint8_t *bytes, size_t size, uint32_t &width, uint32_t &height) {
	if (bytes == nullptr || size < 20 || std::memcmp(bytes, "DDS ", 4) != 0) return false;
	height = io::read_u32_le(bytes + 12);
	width = io::read_u32_le(bytes + 16);
	return true;
}

} // namespace opennova::dds
