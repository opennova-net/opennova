// DDS: the DirectDraw surface file the game hands whole to D3DX, which reads the file's own
// pixel format (dds.cpp holds the witnesses). The writer makes the plainest one: A8R8G8B8
// (32 bits a pixel, alpha in the top byte), one level and no mip chain, its pixels B, G, R, A
// from the top row down; and a DXT texture of its blocks and mip chain (dds_write_dxt). The
// writers, a header's size and D3DX's read of the file (dds_read: the editor's texture document
// shows a file as the game loads it, ADR 0046 S18); the runtime decodes a DDS through its embedder
// (the shell's Godot decoder), and the editor's menu render reads a header's size alone
// (editor/preview/texture_header).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::dds {

inline constexpr size_t DDS_HEADER_SIZE = 128; // "DDS " and the 124-byte DDS_HEADER

// The header's flags and fields the writer sets (the DirectDraw names).
inline constexpr uint32_t DDSD_CAPS = 0x1;
inline constexpr uint32_t DDSD_HEIGHT = 0x2;
inline constexpr uint32_t DDSD_WIDTH = 0x4;
inline constexpr uint32_t DDSD_PITCH = 0x8;
inline constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
inline constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
inline constexpr uint32_t DDPF_RGB = 0x40;
inline constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;

// `rgba` holds width x height pixels, R, G, B, A each, the top row first. False, with
// `error`, for an empty image or one whose row pitch a 32-bit field cannot hold.
bool dds_write_a8r8g8b8(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                        std::string &error);

// A cube map of six square faces, each `side` x `side` pixels of A8R8G8B8, one level, in DirectDraw's face order
// (+X, -X, +Y, -Y, +Z, -Z), each `faces[i]` holding R, G, B, A, the top row first: the header's caps COMPLEX and
// TEXTURE, its caps 2 the cube map flag (0x200) and all six faces (DDSCAPS2_CUBEMAP_ALL), as the game's own
// HwmCube.dds states them. What D3DX's cube load takes [orig: sub_58A690 -> D3DXCreateCubeTextureFromFileInMemory
// @ 0x68464e]. False, with `error`, for a face missing or a side of 0 or past what a pitch holds.
bool dds_write_cube_a8r8g8b8(const uint8_t *const faces[6], uint32_t side, std::vector<uint8_t> &out, std::string &error);

// The size a DDS header states: DirectDraw's layout, the "DDS " magic and then the
// DDS_HEADER, whose height and width are the dwords at file offsets 12 and 16. False when
// `size` is shorter than those fields or the magic is not there. A side may be 0.
bool dds_header_size(const uint8_t *bytes, size_t size, uint32_t &width, uint32_t &height);

// --- the read: what D3DX makes of the file (dds.cpp holds the witnesses) -------------------------

inline constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
inline constexpr uint32_t DDSD_LINEARSIZE = 0x80000;
inline constexpr uint32_t DDSD_DEPTH = 0x800000;
inline constexpr uint32_t DDPF_ALPHA = 0x2;
inline constexpr uint32_t DDPF_FOURCC = 0x4;
inline constexpr uint32_t DDPF_PALETTEINDEXED8 = 0x20;
inline constexpr uint32_t DDPF_LUMINANCE = 0x20000;
inline constexpr uint32_t DDSCAPS_COMPLEX = 0x8;
inline constexpr uint32_t DDSCAPS_MIPMAP = 0x400000;
inline constexpr uint32_t DDSCAPS2_CUBEMAP_ALL = 0xFC00; // the cube map flag and its six faces
// A FourCC as its four characters spell it, the first in the low byte.
constexpr uint32_t dds_fourcc(char a, char b, char c, char d) {
	return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// The header's fields D3DX reads, as the file states them.
struct DdsHeader {
	uint32_t flags = 0;
	uint32_t height = 0;
	uint32_t width = 0;
	uint32_t pitch = 0;
	uint32_t depth = 0;
	uint32_t mip_count = 0;
	uint32_t format_size = 0; // the pixel format's own size field (32)
	uint32_t format_flags = 0;
	uint32_t fourcc = 0;
	uint32_t bits = 0;
	uint32_t red_mask = 0, green_mask = 0, blue_mask = 0, alpha_mask = 0;
	uint32_t caps = 0, caps2 = 0;
};

// The Direct3D format D3DX takes a file's pixel format as (its table of DirectDraw pixel formats,
// dds.cpp), by its D3DFORMAT value (a FourCC's own code for a FourCC format); 0 for none.
struct DdsFormat {
	uint32_t d3d = 0;
	const char *name = ""; // "DXT1", "A8R8G8B8", ...
	bool compressed = false; // a 4 x 4 block format (the DXTs)
	uint32_t block_bytes = 0; // a DXT's block
	uint32_t bits = 0; // a texel's bits otherwise (a FourCC's packed pair: 16)
	bool palette = false; // a 256-entry palette after the header (P8, A8P8)
	// dds_read decodes its texels: the masked RGB, luminance, alpha and palette forms. A DXT's blocks it
	// leaves to the D3DX codec's port (runtime/renderer/texture_dxt), each level's bytes at its offset.
	bool decoded = false;
};

// One level of the surface D3DX reads (the first face of a cube map, the first slice of a volume):
// its sides and, where the format's texels are decoded, R, G, B, A a texel from the top row down.
struct DdsLevel {
	uint32_t width = 0;
	uint32_t height = 0;
	size_t offset = 0; // where its texels start in the file
	size_t bytes = 0;  // its texels' bytes (one face, every slice)
	std::vector<uint8_t> rgba;
};

// What D3DX makes of a DDS file (dds_read). `loads`: D3DX's DDS loader takes it (the magic and a
// whole header, a pixel format its table holds, a cube map of its six faces or none, every level of
// every face and slice the header states held by the file); else `refusal` says why, which leaves the
// game's texture unloaded. `levels` the first face's (and slice's) chain, as the header's mip count
// says it (0 read as 1), each level half the one before; `faces` 6 for a cube map; `depth` a volume's
// slices; `palette` a P8's 256 entries (R, G, B, A each, the file's PALETTEENTRY flags its alpha).
struct DdsImage {
	DdsHeader header;
	DdsFormat format;
	bool loads = false;
	std::string refusal;
	uint32_t faces = 1;
	uint32_t depth = 1;
	std::vector<DdsLevel> levels;
	std::vector<uint8_t> palette;
	size_t data_bytes = 0;  // the bytes every level of every face takes
	size_t extra_bytes = 0; // the bytes after them, which nothing reads
};
// D3DX's DDS loader over a file's bytes: false, with `error`, for bytes that are no DDS at all (no
// "DDS " magic, or shorter than the magic and its 124-byte header); else true, `out` saying whether
// D3DX loads it and, for a format it decodes (DdsFormat::decoded), each level's texels.
bool dds_read(const uint8_t *bytes, size_t size, DdsImage &out, std::string &error);
// The format D3DX's table matches a header's pixel format to (dds.cpp); `d3d` 0 for none.
DdsFormat dds_format_of(const DdsHeader &header);

// A DXT texture written: the magic, a header stating the FourCC, the sides, the first level's size
// (DDSD_LINEARSIZE) and the mip count, then each level's blocks in order (`levels`, the first level
// first, each the blocks of a side half the one before, 4 x 4 texels a block: dxt_block_bytes of the
// FourCC each). False, with `error`, for a FourCC that is no DXT, an empty image or a level whose
// bytes are not its blocks'.
bool dds_write_dxt(uint32_t fourcc, uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>> &levels,
                   std::vector<uint8_t> &out, std::string &error);
// A DXT FourCC's block (8 for DXT1, 16 for DXT2 to DXT5), 0 for another.
uint32_t dxt_block_bytes(uint32_t fourcc);

} // namespace opennova::dds
