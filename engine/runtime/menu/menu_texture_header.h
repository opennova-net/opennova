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

// The sides a menu image is read back at from the texture its loader made, where that texture's
// sides can be larger than the image's: a DDS's, the sides its header states. The DDS loader asks
// D3DX for D3DX_DEFAULT sides, which rounds a side no power of two up with the file's top level in
// the texture's top-left corner (renderer::d3dx_default_texture_side, D-RMAT-18); the menu loader
// keeps the sides D3DX reports of the FILE and reads back that many texels of each row from the
// texture's top-left, so the menu image is the file's top level at its own sides. False for any
// other format: its reader decodes the pixels at their own sides already.
// [orig: TextureSlot_LoadFromDXTFile @ 0x6642E0 - GTexture_FindOrCreateByName("DXTTEMP", ..,
//  0xC0000000) @ 0x66438E, GTexture_GetDimensionsChecked @ 0x6643A0, the w x h buffer @ 0x6643D4,
//  the read-back @ 0x6643F4; GTexture_InitFromMemory @ 0x687DF0 - the D3DX call with D3DX_DEFAULT
//  sides @ 0x687EF5, its D3DXIMAGE_INFO Width and Height stored at +0x0C / +0x10 @ 0x687F09 /
//  @ 0x687F0C; GTexture_ReadMipPixels @ 0x6879C0 - the sides @ 0x6879D5 / @ 0x6879D7, the row copy
//  @ 0x687A72..0x687A8E. The same in TextureSlot_LoadFromDXTFile @ 0x6f1550, GTexture_InitFromMemory
//  @ 0x6c49b0 (@ 0x6c4ab5, @ 0x6c4ac9 / @ 0x6c4acc) and GTexture_ReadMipPixels @ 0x6c4580 (dfx2.exe)]
bool menu_image_readback_size(MenuTextureFormat format, const std::vector<uint8_t> &bytes, int *width,
                              int *height);

// A texture decoder that keeps no pixels: the size menu_texture_header_size reads.
class MenuTextureHeaderProbe : public MenuTextureDecoder {
public:
	bool decode(const std::string &key, MenuTextureFormat format, const std::vector<uint8_t> &bytes,
	            int &width, int &height) override;
	void release(const std::string &key) override;
};

} // namespace opennova::menu
