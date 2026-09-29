#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_frame_assets.h>

namespace opennova::editor {

// The size a menu texture file states, by the format the game's loader picks for its name
// (menu::menu_texture_source): a TGA's header (tga::tga_header_size: image types 1, 2, 3,
// 9, 10 and 11, the colour-mapped, true-colour and grey forms, raw and run-length), a DDS's
// (dds::dds_header_size), a PNG's IHDR, and a PCX through the engine's own port of the
// game's decoder (the one the shell's frame decodes it with). False when the file does not
// hold one, or states a side of 0. The editor's headless render
// measures textures with it; the shell's frame decodes the pixels (ADR 0046 S9j2).
bool texture_header_size(menu::MenuTextureFormat format, const std::vector<uint8_t> &bytes, int *width,
                         int *height);

// A texture decoder that keeps no pixels: the size texture_header_size reads.
class TextureHeaderProbe : public menu::MenuTextureDecoder {
public:
	bool decode(const std::string &key, menu::MenuTextureFormat format, const std::vector<uint8_t> &bytes,
	            int &width, int &height) override;
	void release(const std::string &key) override;
};

} // namespace opennova::editor
