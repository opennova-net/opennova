#pragma once

// A menu texture's size read from its header, no pixels decoded: what a headless compile of a
// menu screen measures its textures with (MenuFrameAssets over a MenuTextureDecoder that keeps no
// pixels). The game's frame decodes the pixels; the sizes agree.

#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_frame_assets.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

// The size a menu texture file states, by the format the game's loader picks for its name
// (menu_texture_source): a TGA's header (tga::tga_header_size: image types 1, 2, 3, 9, 10 and
// 11, the colour-mapped, true-colour and grey forms, raw and run-length), a DDS's
// (dds::dds_header_size), a PNG's IHDR (png::png_header_size), and a PCX through the engine's
// own port of the game's decoder (the one the shell's frame decodes it with). False when the
// file does not hold one, or states a side of 0 (or one past 0x7FFFFFFF).
bool menu_texture_header_size(MenuTextureFormat format, const std::vector<uint8_t> &bytes, int *width,
                              int *height);

// A texture decoder that keeps no pixels: the size menu_texture_header_size reads.
class MenuTextureHeaderProbe : public MenuTextureDecoder {
public:
	bool decode(const std::string &key, MenuTextureFormat format, const std::vector<uint8_t> &bytes,
	            int &width, int &height) override;
	void release(const std::string &key) override;
};

} // namespace opennova::menu
