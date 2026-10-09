#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <formats/dds/dds.h>

namespace opennova::renderer {

// Retail's DXT texture path: the texture creator picks a DXT format from the
// creation flags and the adapter caps, and the D3DX9 texture codec linked
// into Jointops.exe (D3DXTex::CCodecDXT::CCodecDXT picks the block coder per
// format @ 0x6ED977..0x6EDA31) encodes the pixels. Every value below is a
// structural translation of that codec. The device is created without
// D3DCREATE_FPU_PRESERVE [orig: CGfxDevice_CreateDevice @ 0x67EA17 (behavior
// flags 0x80, then 0x20)], so the x87 unit rounds every operation to single
// precision and the float arithmetic here reproduces it operation for
// operation; every float-to-integer conversion runs under a round-toward-zero
// control word, which static_cast matches.

// D3DXCOLOR.
struct DxtColor {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 0.0f;
};

using DxtBlockColors = std::array<DxtColor, 16>;

enum class TextureDxtFormat : uint8_t { None, Dxt1, Dxt5 };

// The adapter answers CheckDeviceFormat for DXT1 and DXT5
// [orig: CGfxDevice_DetectHardwareCapabilities @ 0x67D55D..0x67D604 (stores
// +0x11C, +0x124)], and a list of NVIDIA device ids (GeForce 256 through
// GeForce4 Ti) sets the flag that keeps DXT5 where a texture would otherwise
// fall back to DXT1 [orig: the same function @ 0x67D6DA (value 1), stores
// +0x138 @ 0x67D7A6, 0x67D812, 0x67D827, 0x67D853].
struct TextureDxtCaps {
	bool dxt1 = true;
	bool dxt5 = true;
	bool keep_dxt5 = false;
};

// The reference machine: a current adapter with both formats and no quirk.
inline constexpr TextureDxtCaps kReferenceTextureDxtCaps{};

// The format a pixel-data texture is created in.
// [orig: GTexture_CreateFromPixelData_0 @ 0x687717..0x687766;
// GTexture_CreateFromPixelDataWithAlphaBlend @ 0x6872E0..0x68732F]
TextureDxtFormat select_texture_dxt_format(
		uint32_t creation_flags, const TextureDxtCaps &caps);

// The level count GTexture_CreateFromPixelData_0 asks D3DXCreateTexture for:
// one level per halving while the smaller side exceeds 2, so the chain ends at
// 4 texels; flag 0x40000 keeps one level and 0x80000 at most three. A count of
// 0 is D3DX's full chain to 1x1.
// [orig: GTexture_CreateFromPixelData_0 @ 0x6877BA..0x687801]
uint32_t texture_level_count(uint32_t width, uint32_t height,
		uint32_t creation_flags);

size_t dxt_block_bytes(TextureDxtFormat format);

// One 4x4 block, texels row-major. [orig: D3DXTex_D3DXEncodeDXT1 @ 0x72170F
// (colorkeyed: alpha below 0.5 selects the transparent index);
// D3DXTex_EncodeDXT5Block @ 0x721962]
void encode_dxt1_block(const DxtBlockColors &texels, bool dither,
		uint8_t *block);
void encode_dxt5_block(const DxtBlockColors &texels, bool dither,
		uint8_t *block);

// [orig: D3DXTex_D3DXDecodeDXT1 @ 0x72140A; D3DXTex_D3DXDecodeDXT5
// @ 0x7215D1]
void decode_dxt1_block(const uint8_t *block, DxtBlockColors &texels);
void decode_dxt5_block(const uint8_t *block, DxtBlockColors &texels);

struct DxtSurface {
	TextureDxtFormat format = TextureDxtFormat::None;
	uint32_t width = 0;
	uint32_t height = 0;
	// Blocks row-major, 4x4 texels each.
	std::vector<uint8_t> blocks;

	bool is_valid() const noexcept;
};

// The A8R8G8B8 source codec: each byte times 1/255. Pixels are R, G, B, A.
// [orig: D3DXTex::CCodec_A8R8G8B8::Decode @ 0x6EBE51..0x6EBEA6]
std::vector<DxtColor> decode_rgba8(const uint8_t *rgba, uint32_t width,
		uint32_t height);

// D3DX's A8R8G8B8 destination codec without dithering: round half up by
// adding 0.5 before truncation, clamped to a byte.
// [orig: D3DXTex::CCodec_A8R8G8B8::Encode @ 0x6E58EE..0x6E597F (unk_856B90
// row of 0.5), clamp @ 0x6E5AC0..0x6E5B3E]
std::vector<uint8_t> encode_rgba8(const std::vector<DxtColor> &colors);

// The DXT codec's surface encode: one block per 4x4 texels; a block that runs
// past the right or bottom edge repeats the edge texels.
// [orig: D3DXTex::CCodecDXT::Encode @ 0x6EDB3A (block assembly
// @ 0x6EDD81..0x6EDDC8, edge fill @ 0x6EDE37..0x6EDEC6, block call
// @ 0x6EDEE5)]
DxtSurface encode_dxt_surface(const std::vector<DxtColor> &colors,
		uint32_t width, uint32_t height, TextureDxtFormat format);

// [orig: D3DXTex::CCodecDXT::Decode @ 0x6EDF5B]
std::vector<DxtColor> decode_dxt_surface(const DxtSurface &surface);

// D3DX_FILTER_BOX halving: each output texel is ((p01 + p00) + p10) + p11,
// times 0.25; a side of 1 reuses its single texel.
// [orig: D3DXTex::CBlt::BltBox2D @ 0x6E1100, generic path @ 0x6E13C4..0x6E1448]
std::vector<DxtColor> box_filter_half(const std::vector<DxtColor> &colors,
		uint32_t width, uint32_t height);

// One D3DX_FILTER_BOX level of RGBA8 texels: the level below `width` x `height` (each side
// halved, at least 1), box_filter_half of the A8R8G8B8 codec's read of the texels, stored back
// through it (decode_rgba8, encode_rgba8).
std::vector<uint8_t> box_filter_half_rgba8(const uint8_t *rgba, uint32_t width, uint32_t height);

// D3DXFilterTexture's chain of an A8R8G8B8 texture, D3DX_FILTER_BOX, each level the box filter of
// the bytes the level before was stored as: `levels` (its first the texture's top level, a level
// its sides in `width` and `height`, int or unsigned, and its RGBA8 texels in `rgba`) grown by
// box_filter_half_rgba8 of its last level until it holds `count` levels or its last is 1 x 1
// (`count` 0: on to 1 x 1).
// [orig: GTexture_CreateFromPixelData_0 @ 0x6878B5..0x6878BE (D3DXFilterTexture, filter 5);
// D3DXFilterTexture @ 0x6910C9 (each level from the previous one @ 0x6912AD..0x6912F9)]
template <typename Level>
void extend_box_chain(std::vector<Level> &levels, size_t count) {
	while (!levels.empty() && (count == 0 || levels.size() < count)) {
		const uint32_t width = static_cast<uint32_t>(levels.back().width);
		const uint32_t height = static_cast<uint32_t>(levels.back().height);
		if (width <= 1 && height <= 1)
			break;
		Level below;
		below.width = static_cast<decltype(below.width)>(std::max(1u, width / 2));
		below.height = static_cast<decltype(below.height)>(std::max(1u, height / 2));
		below.rgba = box_filter_half_rgba8(levels.back().rgba.data(), width, height);
		levels.push_back(std::move(below));
	}
}

// A DDS's levels as D3DX reads them, RGBA8: `image` is dds_read's of the file's `bytes`, which
// decodes the masked, luminance, alpha and palette forms itself and leaves a DXT's blocks to the
// D3DX codec's port; each DXT1, DXT4 or DXT5 level's blocks are decoded here (decode_dxt_surface;
// DXT4 is DXT5's blocks over colour premultiplied by alpha). `max_levels`, when not 0, decodes the
// chain's first levels alone, the rest left coded. True when the format's texels are known:
// dds_read decoded them, or this did.
bool decode_dds_levels(const uint8_t *bytes, size_t size, dds::DdsImage &image, size_t max_levels = 0);

// The full level set of a pixel-data texture created in `format`: level 0 is
// the source loaded through D3DXLoadSurfaceFromMemory with D3DX_FILTER_NONE,
// every later level is D3DXFilterTexture's box filter of the level before it,
// decoded from that level's own blocks.
// [orig: GTexture_CreateFromPixelData_0 @ 0x6878A0 (load, filter 1) and
// @ 0x6878BE (D3DXFilterTexture, filter 5); D3DXFilterTexture @ 0x6910C9
// (each level from the previous one @ 0x6912AD..0x6912F9)]
std::vector<DxtSurface> build_dxt_texture_levels(const uint8_t *rgba,
		uint32_t width, uint32_t height, TextureDxtFormat format,
		uint32_t level_count);

} // namespace opennova::renderer
