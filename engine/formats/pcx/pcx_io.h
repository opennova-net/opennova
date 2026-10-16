#pragma once

#include "pcx.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// The PCX header's fields as the file states them (the 128 bytes before the texels): the bits a
// plane (byte 3), the sides from the window (bytes 4..11, xmax - xmin + 1 and ymax - ymin + 1),
// the planes (byte 65) and the bytes a line (66).
struct PcxHeader {
	uint8_t bits = 0;
	int width = 0;
	int height = 0;
	uint8_t planes = 0;
	uint16_t bytes_per_line = 0;
};
// False when `size` is shorter than the 128-byte header or its first byte is not the 0x0A
// manufacturer mark. No field is range-checked: each decoder applies its own limits.
bool pcx_read_header(const uint8_t *data, size_t size, PcxHeader &out);

// How an indexed image's rows fit the buffer the game's PCX reader writes them into: it decodes
// each row to its bytes a line and writes them at a stride of the image's width, so a row longer
// than the width runs into the next row and the last row's past the buffer (Overrun), and a
// shorter one leaves the rest of each row as the buffer held it (Short) [orig:
// Texture_LoadPCXFromPFF32 @ 0x56EA30, the rows @ 0x56ED70..0x56EDFC]. A three-plane image's rows
// are read a plane at a time [orig: @ 0x56EB31] and fit (Exact). The reader takes 8 bits a plane
// alone [orig: @ 0x56EABC]; this reads the rows of any depth.
enum class PcxRowFit : uint8_t { Exact, Overrun, Short };
PcxRowFit pcx_row_fit(const PcxHeader &header);

// How an 8-bit indexed PCX's rows are run-length coded, as a file shows it. Each row is coded alone, a run
// of 2 to 63 equal bytes as a count byte (0xC0 | n) and the byte, a byte with its top two bits set as a
// count of 1 and the byte, and:
//   Shipped  any other byte alone: the import's writer. Every char and foliage map JO ships ties between this
//            coder and MED's (every row ends in a run, none in a lone byte), so the shipped tool's own tail
//            rule is unwitnessed; a shipped map's header (72 dpi) reads it as this one.
//   Med      the same, but each row's LAST run always a count pair, a single byte below 0xC0 included:
//            JO's mission editor, which writes the char map it paints [orig: JOTACmed.exe sub_45E8A0 @
//            0x45E8A0, the tail @ 0x45E9BF..0x45E9E7; the rows a row at a time, sub_45EA00 @ 0x45EA2B].
//   Other    rows neither coder writes as they are (another program's, a row padded past its width, a
//            run across rows): read whole, written again in the shipped coding.
enum class PcxCoding : uint8_t { Shipped, Med, Other };

// A PCX header's 16-colour (EGA) palette, bytes 16..63: 16 entries of R, G, B.
using PcxEgaPalette = std::array<std::array<uint8_t, 3>, 16>;
// Every entry's every byte `value` (the shipped char maps' 0xFF, MED's 0).
PcxEgaPalette pcx_ega_filled(uint8_t value);

// An 8-bit indexed PCX's form: every header field its image does not decide, as the file holds it, and the
// coding of its rows. The image decides the manufacturer mark (0x0A), the RLE flag (1), the bits a plane
// (8), the window's far corner (the origin plus the size less one), the one plane, the bytes a line (the
// width) and the palette's 0x0C mark; a writer computes those and takes the rest from here, so a file
// read and written again is the same bytes (the shipped _f maps' leftover EGA entries included). The
// filler, bytes 74..127, is zero in every shipped map and in MED's writer, and is written zero. The
// defaults are the shipped maps' form.
struct PcxIndexedForm {
	uint8_t version = 5;                         // byte 1
	uint16_t window_x = 0;                       // bytes 4..5, the window's xmin
	uint16_t window_y = 0;                       // bytes 6..7, ymin
	uint16_t hdpi = 72;                          // bytes 12..13
	uint16_t vdpi = 72;                          // bytes 14..15
	PcxEgaPalette ega = pcx_ega_filled(0xFF);    // bytes 16..63, the 16-colour palette
	uint8_t reserved = 0;                        // byte 64
	uint16_t palette_info = 1;                   // bytes 68..69
	uint16_t hscreen = 0;                        // bytes 70..71
	uint16_t vscreen = 0;                        // bytes 72..73
	PcxCoding coding = PcxCoding::Shipped;

	// The same header fields (the coding aside).
	bool same_header(const PcxIndexedForm &other) const {
		return version == other.version && window_x == other.window_x && window_y == other.window_y &&
		       hdpi == other.hdpi && vdpi == other.vdpi && ega == other.ega && reserved == other.reserved &&
		       palette_info == other.palette_info && hscreen == other.hscreen && vscreen == other.vscreen;
	}
};

// The form JO's mission editor writes a char map in: the header zero but for 0A 05 01 08, the window, one
// plane, the bytes a line and the palette info 1 (no resolution, no EGA palette), its rows MED-coded
// [orig: JOTACmed.exe sub_45EB60 @ 0x45EBA0..0x45EBFF].
PcxIndexedForm pcx_med_form();

bool decode_pcx_rgb(const uint8_t *data, size_t size, RgbImage &out, std::string &error);
// `form`, where the caller asks it: the file's form, its coding the one its rows show (where both coders
// write them alike, MED's when the header is MED's header, pcx_med_form's, else the shipped).
bool decode_pcx_indexed(const uint8_t *data, size_t size, IndexedImage8 &out, std::string &error,
                        PcxIndexedForm *form = nullptr);
// An 8-bit indexed PCX in `form` (the shipped form when none is given): the header from the form and the
// image, the rows coded as the form says (Other as Shipped), 0x0C and the palette.
bool encode_pcx_indexed(const IndexedImage8 &image, std::vector<uint8_t> &out, std::string &error,
                        const PcxIndexedForm *form = nullptr);
// A 24-bit PCX (8 bits a plane, three planes): each row the red, green and blue planes of `width`
// bytes each, run-length coded a plane at a time, no palette. The shape the game's 24-bit path reads
// [orig: Texture_LoadPCXFromPFF32 @ 0x56EB31; load_pcx_to_argb @ 0x664cc0's NPlanes == 3 path]: 3 x
// BytesPerLine bytes a row, the planes `width` apart, so its bytes a line are its width exactly.
bool encode_pcx_rgb(const RgbImage &image, std::vector<uint8_t> &out, std::string &error);

// The archive loader's alpha read: the game's 8-bit PCX reader
// (Texture_LoadPCXFromPFF8Bit) then each pixel's palette-entry luminance
// (85 * (r + g + b)) >> 8 as its alpha, RGB the palette entry's colour [orig:
// Texture_LoadFromArchive @ 0x58b980 — table build @ 0x58bc35..0x58bca9, per-pixel A
// @ 0x58bcee]. The reader is as lax as retail's: the bits per pixel (byte 3) must be
// 8, NPlanes is not read, the 0x0C marker is not tested (the palette is the last 768
// bytes), the RLE runs from offset 128 through the file's end, and each row decodes
// BytesPerLine indices at a stride of `width`, so pad indices spill onto the next
// row's start and the last row's onto the palette, which the luminance then reads.
// Data the file does not hold reads as 0. False on a short header, a bits-per-pixel
// other than 8, a side that is not positive, or more pixels than the file's data can
// describe (decode_pcx_menu_rgba's bound).
bool decode_pcx_luminance_alpha(const uint8_t *data, size_t size, RgbaImage &out, std::string &error);

// The menu texture loader's PCX decode, a structural port [orig: load_pcx_to_argb
// @ 0x664cc0 via CTextureManager_LoadOrFindTexture @ 0x654980]: the header's bits
// per pixel (byte 3) must be 8; NPlanes (byte 0x41) 3 takes the 24-bit path, which
// reads 3 * BytesPerLine RLE bytes a row and takes the red, green and blue planes
// `width` bytes apart; any other NPlanes takes the 8-bit path: the palette is the
// last 768 bytes (every entry fully opaque, no colour key), file size - 896 bytes of
// RLE data from offset 128, each row decoded to BytesPerLine (byte 0x42) pixels
// starting `width` pixels after the last, so a row's padding lands on the start of
// the next row (the next row overwrites it; retail's last row writes past its
// buffer, which this port drops). Data the file does not hold reads as 0. RGBA8 out.
// The header-sized buffer is allocated only when the file's data can describe it (32
// pixels a byte after the 128-byte header, at most 2^28 pixels); a larger claim fails
// the decode, as retail's failed allocation fails the load (code 2).
//
// `indexed`, where the caller asks it: what the 8-bit path (NPlanes other than 3) made the
// image of, each texel's palette index as the rows wrote it (a row's padding landing on
// the next row's start, as the colours do) and the palette, the last 768 bytes; the
// models' reader decodes alike [orig: Texture_LoadPCXFromPFF32 @ 0x56EA30: the BPP check
// returning 3, the palette from Seek(-768, 2), file size - 896 bytes of RLE data from
// offset 128, each row to BytesPerLine], so a texel's colour is its index's entry,
// opaque. An image the 24-bit path reads leaves it not indexed.
struct PcxIndexed {
	bool indexed = false;
	std::vector<uint8_t> indices; // a texel's index, the top row first
	uint8_t palette[256][3] = {};
};
bool decode_pcx_menu_rgba(const uint8_t *data, size_t size, RgbaImage &out, std::string &error,
                          PcxIndexed *indexed = nullptr);

} // namespace opennova
