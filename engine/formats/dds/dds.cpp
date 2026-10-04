// The DDS writer and header size (dds.h).

#include <formats/dds/dds.h>

#include <base/io/le.h>

#include <cstring>
#include <string>

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

namespace {

// D3DX's table of the DirectDraw pixel formats it reads, in its order, each matched to a Direct3D
// format [orig: the table at 0x8564A0, nine dwords a row: the format, the pixel format's size, its
// flags, FourCC, bits and four masks; read by D3DXTex::CImage::LoadDDS @ 0x6DDA66]. The name and what
// the port does with it are ours.
struct FormatRow {
	uint32_t d3d, flags, fourcc, bits, red, green, blue, alpha;
	const char *name;
};
constexpr uint32_t kRgb = DDPF_RGB, kRgba = DDPF_RGB | DDPF_ALPHAPIXELS;
constexpr FormatRow kFormats[] = {
	{0x14, kRgb, 0, 24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0, "R8G8B8"},
	{0x15, kRgba, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, "A8R8G8B8"},
	{0x16, kRgb, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0, "X8R8G8B8"},
	{0x17, kRgb, 0, 16, 0xF800, 0x07E0, 0x001F, 0, "R5G6B5"},
	{0x18, kRgb, 0, 16, 0x7C00, 0x03E0, 0x001F, 0, "X1R5G5B5"},
	{0x19, kRgba, 0, 16, 0x7C00, 0x03E0, 0x001F, 0x8000, "A1R5G5B5"},
	{0x1A, kRgba, 0, 16, 0x0F00, 0x00F0, 0x000F, 0xF000, "A4R4G4B4"},
	{0x1B, kRgb, 0, 8, 0xE0, 0x1C, 0x03, 0, "R3G3B2"},
	{0x1C, DDPF_ALPHA, 0, 8, 0, 0, 0, 0xFF, "A8"},
	{0x1D, kRgba, 0, 16, 0xE0, 0x1C, 0x03, 0xFF00, "A8R3G3B2"},
	{0x1E, kRgb, 0, 16, 0x0F00, 0x00F0, 0x000F, 0, "X4R4G4B4"},
	{0x1F, kRgba, 0, 32, 0x3FF00000, 0x000FFC00, 0x000003FF, 0xC0000000, "A2B10G10R10"},
	{0x20, kRgba, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000, "A8B8G8R8"},
	{0x21, kRgb, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0, "X8B8G8R8"},
	{0x22, kRgb, 0, 32, 0x0000FFFF, 0xFFFF0000, 0, 0, "G16R16"},
	{0x23, kRgba, 0, 32, 0x000003FF, 0x000FFC00, 0x3FF00000, 0xC0000000, "A2R10G10B10"},
	{0x28, DDPF_PALETTEINDEXED8 | DDPF_ALPHAPIXELS, 0, 16, 0, 0, 0, 0xFF00, "A8P8"},
	{0x29, DDPF_PALETTEINDEXED8, 0, 8, 0, 0, 0, 0, "P8"},
	{0x32, DDPF_LUMINANCE, 0, 8, 0xFF, 0, 0, 0, "L8"},
	{0x33, DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 0, 16, 0xFF, 0, 0, 0xFF00, "A8L8"},
	{0x34, DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 0, 8, 0x0F, 0, 0, 0xF0, "A4L4"},
	{0x51, DDPF_LUMINANCE, 0, 16, 0xFFFF, 0, 0, 0, "L16"},
	{0x3C, 0x80000, 0, 16, 0xFF, 0xFF00, 0, 0, "V8U8"},
	{0x3D, 0x40000, 0, 16, 0x1F, 0x3E0, 0xFC00, 0, "L6V5U5"},
	{0x3E, 0x40000, 0, 32, 0xFF, 0xFF00, 0xFF0000, 0, "X8L8V8U8"},
	{0x3F, 0x80000, 0, 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000, "Q8W8V8U8"},
	{0x40, 0x80000, 0, 32, 0xFFFF, 0xFFFF0000, 0, 0, "V16U16"},
	{0x43, 0x80001, 0, 32, 0x3FF00000, 0x000FFC00, 0x000003FF, 0xC0000000, "A2W10V10U10"},
	{dds_fourcc('U', 'Y', 'V', 'Y'), DDPF_FOURCC, dds_fourcc('U', 'Y', 'V', 'Y'), 16, 0, 0, 0, 0, "UYVY"},
	{dds_fourcc('R', 'G', 'B', 'G'), DDPF_FOURCC, dds_fourcc('R', 'G', 'B', 'G'), 16, 0, 0, 0, 0, "R8G8_B8G8"},
	{dds_fourcc('Y', 'U', 'Y', '2'), DDPF_FOURCC, dds_fourcc('Y', 'U', 'Y', '2'), 16, 0, 0, 0, 0, "YUY2"},
	{dds_fourcc('G', 'R', 'G', 'B'), DDPF_FOURCC, dds_fourcc('G', 'R', 'G', 'B'), 16, 0, 0, 0, 0, "G8R8_G8B8"},
	{dds_fourcc('D', 'X', 'T', '1'), DDPF_FOURCC, dds_fourcc('D', 'X', 'T', '1'), 0, 0, 0, 0, 0, "DXT1"},
	{dds_fourcc('D', 'X', 'T', '2'), DDPF_FOURCC, dds_fourcc('D', 'X', 'T', '2'), 0, 0, 0, 0, 0, "DXT2"},
	{dds_fourcc('D', 'X', 'T', '3'), DDPF_FOURCC, dds_fourcc('D', 'X', 'T', '3'), 0, 0, 0, 0, 0, "DXT3"},
	{dds_fourcc('D', 'X', 'T', '4'), DDPF_FOURCC, dds_fourcc('D', 'X', 'T', '4'), 0, 0, 0, 0, 0, "DXT4"},
	{dds_fourcc('D', 'X', 'T', '5'), DDPF_FOURCC, dds_fourcc('D', 'X', 'T', '5'), 0, 0, 0, 0, 0, "DXT5"},
	{0x46, 0x400, 0, 16, 0, 0xFFFF, 0, 0, "D16_LOCKABLE"},
	{0x52, DDPF_FOURCC, 0x52, 32, 0, 0, 0, 0, "D32F_LOCKABLE"},
	{0x24, DDPF_FOURCC, 0x24, 64, 0, 0, 0, 0, "A16B16G16R16"},
	{0x6E, DDPF_FOURCC, 0x6E, 64, 0, 0, 0, 0, "Q16W16V16U16"},
	{0x6F, DDPF_FOURCC, 0x6F, 16, 0, 0, 0, 0, "R16F"},
	{0x70, DDPF_FOURCC, 0x70, 32, 0, 0, 0, 0, "G16R16F"},
	{0x71, DDPF_FOURCC, 0x71, 64, 0, 0, 0, 0, "A16B16G16R16F"},
	{0x72, DDPF_FOURCC, 0x72, 32, 0, 0, 0, 0, "R32F"},
	{0x73, DDPF_FOURCC, 0x73, 64, 0, 0, 0, 0, "G32R32F"},
	{0x74, DDPF_FOURCC, 0x74, 128, 0, 0, 0, 0, "A32B32G32R32F"},
	{0x75, DDPF_FOURCC, 0x75, 16, 0, 0, 0, 0, "CxV8U8"},
};

bool is_dxt(uint32_t fourcc) { return dxt_block_bytes(fourcc) != 0; }

// The texels dds_read decodes: every masked RGB, luminance, alpha or palette form (not the bump,
// depth, packed-YUV or float forms, nor a DXT's blocks, which the D3DX codec's port decodes:
// runtime/renderer/texture_dxt).
bool port_decodes(const FormatRow &row) {
	if (row.flags == DDPF_FOURCC) return false;
	return (row.flags & (0x80000 | 0x40000 | 0x400)) == 0;
}

// A channel of a texel by its mask, scaled to 8 bits (round to nearest); `missing` where the mask is 0.
uint8_t channel(uint32_t texel, uint32_t mask, uint8_t missing) {
	if (mask == 0) return missing;
	uint32_t shift = 0;
	while (((mask >> shift) & 1u) == 0) ++shift;
	const uint64_t max = mask >> shift;
	const uint64_t value = (texel & mask) >> shift;
	return uint8_t((value * 255u + max / 2) / max);
}

void decode_masked_level(const uint8_t *data, const FormatRow &row, const std::vector<uint8_t> &palette,
                         DdsLevel &level) {
	const size_t bytes = row.bits / 8, count = size_t(level.width) * level.height;
	level.rgba.assign(count * 4, 0);
	const bool luminance = (row.flags & DDPF_LUMINANCE) != 0;
	const bool alpha_only = row.flags == DDPF_ALPHA;
	const bool indexed = (row.flags & DDPF_PALETTEINDEXED8) != 0;
	for (size_t i = 0; i < count; ++i) {
		uint32_t texel = 0;
		for (size_t b = 0; b < bytes; ++b) texel |= uint32_t(data[i * bytes + b]) << (8 * b);
		uint8_t *out = level.rgba.data() + i * 4;
		if (indexed) {
			const uint8_t *entry = palette.data() + size_t(texel & 0xFF) * 4;
			out[0] = entry[0];
			out[1] = entry[1];
			out[2] = entry[2];
			out[3] = row.alpha ? channel(texel, row.alpha, 0xFF) : 0xFF;
		} else if (alpha_only) {
			out[3] = channel(texel, row.alpha, 0xFF);
		} else if (luminance) {
			out[0] = out[1] = out[2] = channel(texel, row.red, 0xFF);
			out[3] = channel(texel, row.alpha, 0xFF);
		} else {
			out[0] = channel(texel, row.red, 0xFF);
			out[1] = channel(texel, row.green, 0xFF);
			out[2] = channel(texel, row.blue, 0xFF);
			out[3] = channel(texel, row.alpha, 0xFF);
		}
	}
}

// One level's bytes as D3DX sizes them: a DXT1's 8 bytes a block, DXT2 to DXT5's 16, the packed
// pairs' 4 bytes a two-texel pair, every other format its bits a texel, row after row.
// [orig: D3DXTex::CImage::LoadDDS @ 0x6DDA66, the per-format pitch and size]
size_t level_bytes(const FormatRow &row, uint32_t width, uint32_t height) {
	if (row.flags == DDPF_FOURCC && is_dxt(row.fourcc))
		return size_t(dxt_block_bytes(row.fourcc)) * ((width + 3) / 4) * ((height + 3) / 4);
	if (row.flags == DDPF_FOURCC && row.bits == 16 && row.d3d > 0xFFFF)
		return size_t(4) * ((width + 1) / 2) * height;
	return size_t(width) * (row.bits / 8) * height;
}

} // namespace

uint32_t dxt_block_bytes(uint32_t fourcc) {
	if (fourcc == dds_fourcc('D', 'X', 'T', '1')) return 8;
	if (fourcc == dds_fourcc('D', 'X', 'T', '2') || fourcc == dds_fourcc('D', 'X', 'T', '3') ||
	    fourcc == dds_fourcc('D', 'X', 'T', '4') || fourcc == dds_fourcc('D', 'X', 'T', '5'))
		return 16;
	return 0;
}

// The match D3DX makes [orig: D3DXTex::CImage::LoadDDS @ 0x6DDA66, the table walk]: the pixel
// format's size 32 and its flags the row's (a FourCC flag set reads as that flag alone); its FourCC
// where the flags say FourCC; its bits where they say RGB, YUV, luminance, alpha, a palette or a bump
// form (0xC4462); its red mask for RGB, YUV, luminance or bump (0xE4040); green and blue for RGB,
// YUV or bump (0xC4440, 0xC4040); alpha where they say alpha (0x80003).
DdsFormat dds_format_of(const DdsHeader &header) {
	DdsFormat out;
	if (header.format_size != 32) return out;
	const uint32_t flags = (header.format_flags & DDPF_FOURCC) ? DDPF_FOURCC : header.format_flags;
	for (const FormatRow &row : kFormats) {
		if (row.flags != flags) continue;
		if ((flags & DDPF_FOURCC) && header.fourcc != row.fourcc) continue;
		if ((flags & 0xC4462u) && header.bits != row.bits) continue;
		if ((flags & 0xE4040u) && header.red_mask != row.red) continue;
		if ((flags & 0xC4440u) && header.green_mask != row.green) continue;
		if ((flags & 0xC4040u) && header.blue_mask != row.blue) continue;
		if ((flags & 0x80003u) && header.alpha_mask != row.alpha) continue;
		out.d3d = row.d3d;
		out.name = row.name;
		out.compressed = row.flags == DDPF_FOURCC && is_dxt(row.fourcc);
		out.block_bytes = out.compressed ? dxt_block_bytes(row.fourcc) : 0;
		out.bits = row.bits;
		out.palette = (row.flags & DDPF_PALETTEINDEXED8) != 0;
		out.decoded = port_decodes(row);
		return out;
	}
	return out;
}

// What D3DX's DDS loader reads [orig: D3DXTex::CImage::LoadDDS @ 0x6DDA66, reached through
// D3DXTex::CImage::Load @ 0x6DF1DC]: the magic and a 124-byte header; the width at file offset 16,
// the height at 12; the depth (offset 24) only where the header's flags say DDSD_DEPTH, else 1, a 0
// read as 1; the faces: six where caps2 (offset 112) holds the cube map flag and all six faces, one
// where it holds none of them, the file refused for any other mix; the mip count (offset 28) read
// whatever the flags say, 0 as 1; the pixel format matched in its table (dds_format_of), the file
// refused for one it lacks; a palette format's 1024 bytes after the header (refused without them);
// then each face's levels in order, each level's bytes (level_bytes) times its slices, every side
// halved for the next (down to 1), the file refused where its bytes stop short of one. Nothing reads
// the bytes past the last level.
bool dds_read(const uint8_t *bytes, size_t size, DdsImage &out, std::string &error) {
	out = DdsImage();
	if (bytes == nullptr || size < DDS_HEADER_SIZE || std::memcmp(bytes, "DDS ", 4) != 0) {
		error = size >= 4 && bytes && std::memcmp(bytes, "DDS ", 4) == 0 ? "The DDS header is cut short."
		                                                                  : "This file is no DDS: it does not start with \"DDS \".";
		return false;
	}
	DdsHeader &h = out.header;
	h.flags = io::read_u32_le(bytes + 8);
	h.height = io::read_u32_le(bytes + 12);
	h.width = io::read_u32_le(bytes + 16);
	h.pitch = io::read_u32_le(bytes + 20);
	h.depth = io::read_u32_le(bytes + 24);
	h.mip_count = io::read_u32_le(bytes + 28);
	h.format_size = io::read_u32_le(bytes + 76);
	h.format_flags = io::read_u32_le(bytes + 80);
	h.fourcc = io::read_u32_le(bytes + 84);
	h.bits = io::read_u32_le(bytes + 88);
	h.red_mask = io::read_u32_le(bytes + 92);
	h.green_mask = io::read_u32_le(bytes + 96);
	h.blue_mask = io::read_u32_le(bytes + 100);
	h.alpha_mask = io::read_u32_le(bytes + 104);
	h.caps = io::read_u32_le(bytes + 108);
	h.caps2 = io::read_u32_le(bytes + 112);
	out.depth = (h.flags & DDSD_DEPTH) && h.depth ? h.depth : 1;
	const uint32_t cube = h.caps2 & DDSCAPS2_CUBEMAP_ALL;
	if (cube != 0 && cube != DDSCAPS2_CUBEMAP_ALL) {
		out.refusal = "It is a cube map without all six faces, which D3DX does not load.";
		return true;
	}
	out.faces = cube ? 6 : 1;
	const uint32_t mips = h.mip_count ? h.mip_count : 1;
	out.format = dds_format_of(h);
	const FormatRow *row = nullptr;
	for (const FormatRow &candidate : kFormats)
		if (candidate.d3d == out.format.d3d && out.format.d3d != 0) row = &candidate;
	if (!row) {
		out.refusal = "Its pixel format is none D3DX reads.";
		return true;
	}
	if (h.width == 0 || h.height == 0) {
		out.refusal = "It states a side of 0.";
		return true;
	}
	size_t at = DDS_HEADER_SIZE;
	if (out.format.palette) {
		if (size - at < 1024) {
			out.refusal = "Its palette (1024 bytes after the header) is cut short.";
			return true;
		}
		out.palette.resize(1024);
		for (size_t i = 0; i < 256; ++i) {
			out.palette[i * 4 + 0] = bytes[at + i * 4 + 0];
			out.palette[i * 4 + 1] = bytes[at + i * 4 + 1];
			out.palette[i * 4 + 2] = bytes[at + i * 4 + 2];
			out.palette[i * 4 + 3] = bytes[at + i * 4 + 3];
		}
		at += 1024;
	}
	const size_t data_start = at;
	for (uint32_t face = 0; face < out.faces; ++face) {
		uint32_t width = h.width, height = h.height, depth = out.depth;
		for (uint32_t mip = 0; mip < mips; ++mip) {
			const size_t one = level_bytes(*row, width, height);
			const size_t all = one * depth;
			if (size - at < all) {
				out.refusal = "Its data stops short of " +
				              (out.faces > 1 ? "face " + std::to_string(face + 1) + ", " : std::string()) + "level " +
				              std::to_string(mip) + " (" + std::to_string(width) + " x " + std::to_string(height) + ").";
				out.levels.clear();
				return true;
			}
			if (face == 0) {
				DdsLevel level;
				level.width = width;
				level.height = height;
				level.offset = at;
				level.bytes = all;
				out.levels.push_back(std::move(level));
			}
			at += all;
			width = width == 1 ? 1 : width >> 1;
			height = height == 1 ? 1 : height >> 1;
			depth = depth == 1 ? 1 : depth >> 1;
		}
	}
	out.data_bytes = at - data_start;
	out.extra_bytes = size - at;
	out.loads = true;
	if (!out.format.decoded) return true;
	for (DdsLevel &level : out.levels) decode_masked_level(bytes + level.offset, *row, out.palette, level);
	return true;
}

bool dds_write_dxt(uint32_t fourcc, uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>> &levels,
                   std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	const uint32_t block = dxt_block_bytes(fourcc);
	if (!block) {
		error = "A DXT texture's FourCC is DXT1 to DXT5.";
		return false;
	}
	if (width == 0 || height == 0 || levels.empty()) {
		error = "A DDS needs at least one texel and one level.";
		return false;
	}
	uint32_t w = width, h = height;
	for (size_t i = 0; i < levels.size(); ++i) {
		if (levels[i].size() != size_t(block) * ((w + 3) / 4) * ((h + 3) / 4)) {
			error = "Level " + std::to_string(i) + "'s bytes are not its blocks'.";
			return false;
		}
		w = w == 1 ? 1 : w >> 1;
		h = h == 1 ? 1 : h >> 1;
	}
	const bool chain = levels.size() > 1;
	out.insert(out.end(), {'D', 'D', 'S', ' '});
	io::append_u32_le(out, 124);
	io::append_u32_le(out, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE |
	                               (chain ? DDSD_MIPMAPCOUNT : 0));
	io::append_u32_le(out, height);
	io::append_u32_le(out, width);
	io::append_u32_le(out, uint32_t(levels.front().size())); // the first level's size
	io::append_u32_le(out, 0); // depth
	io::append_u32_le(out, chain ? uint32_t(levels.size()) : 0);
	for (int i = 0; i < 11; ++i) io::append_u32_le(out, 0); // reserved
	io::append_u32_le(out, 32);
	io::append_u32_le(out, DDPF_FOURCC);
	io::append_u32_le(out, fourcc);
	for (int i = 0; i < 5; ++i) io::append_u32_le(out, 0); // bits and the four masks
	io::append_u32_le(out, DDSCAPS_TEXTURE | (chain ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0));
	for (int i = 0; i < 4; ++i) io::append_u32_le(out, 0); // caps 2 to 4, reserved
	for (const std::vector<uint8_t> &level : levels) out.insert(out.end(), level.begin(), level.end());
	return true;
}

} // namespace opennova::dds
