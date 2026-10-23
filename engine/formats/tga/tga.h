// TGA: the true-colour image the game's model, menu and particle loaders read (tga.cpp
// holds the witnesses). The writer makes the one shape all of them take: an uncompressed
// 32-bit true-colour image (image type 2, 32 bits a pixel, 8 of them alpha, no image ID, no
// colour map), its pixels B, G, R, A from the bottom row up, the header saying so. The writers, a
// header's size and its fields (the editor's texture document says what a file is, ADR 0046 S18); the
// game's own decode of the pixels is the TGA reader, formats/tga/tga_read.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::tga {

inline constexpr size_t TGA_HEADER_SIZE = 18;
inline constexpr uint8_t TGA_TYPE_TRUE_COLOR = 2;
inline constexpr uint8_t TGA_TYPE_RUN_LENGTH_TRUE_COLOR = 10;
inline constexpr uint8_t TGA_DESCRIPTOR_ALPHA_8 = 8; // 8 alpha bits, the origin at the bottom left

// `rgba` holds width x height pixels, R, G, B, A each, the top row first. False, with
// `error`, for an empty image or a side past the header's 16 bits.
bool tga_write_rgba32(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                      std::string &error);
// The same image as 24-bit true colour, its alpha left out (image type 2, 24 bits a pixel, no alpha
// bits, the origin at the bottom left): B, G, R a pixel from the bottom row up, which the game's readers
// take opaque (ADR 0046 S18: a terrain colour map's form). `rgba` as above; refused alike.
bool tga_write_rgb24(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                     std::string &error);

// The TGA 2.0 footer's 26 bytes: the extension and developer areas' offsets, then the signature
// "TRUEVISION-XFILE", '.' and a NUL (the Truevision TGA 2.0 specification). No game reader looks past
// the pixels; every shipped terrain colour map, detail blend map and tile atlas ends with it, both
// offsets 0.
inline constexpr size_t TGA_FOOTER_SIZE = 26;

// The form of an uncompressed true-colour TGA, as the writer makes one: its depth (24 or 32 bits), its
// descriptor byte (written as it was read: no game reader honours its origin bit, so the rows stay
// bottom-up), and whether it ends with the TGA 2.0 footer (offsets 0, no extension or developer area).
// The defaults are tga_write_rgba32's form.
struct TgaForm {
	uint8_t bits = 32;
	uint8_t descriptor = TGA_DESCRIPTOR_ALPHA_8;
	bool footer = false;
};

// The form a file holds, when the writer makes it again byte for byte from its pixels: image type 2, no
// image ID, no colour map, the origin (0, 0), 24 or 32 bits, the pixels exactly, then nothing or the
// TGA 2.0 footer with both offsets 0. False for any other file, which a writer saves in its own form.
bool tga_read_form(const uint8_t *bytes, size_t size, TgaForm &out);

// `rgba` as tga_write_rgba32, written in `form`: the header (type 2, the depth, the descriptor as the form
// carries it), the rows from the bottom up (B, G, R and, at 32 bits, A), then the footer when the form has
// one. Refused as tga_write_rgba32 is, for a depth other than 24 or 32, and for a 24-bit form over pixels
// that are not all opaque (the depth holds no alpha: a painted weight would be lost; tga_write_rgb24 is the
// writer that leaves alpha out by design).
bool tga_write(const uint8_t *rgba, uint32_t width, uint32_t height, const TgaForm &form, std::vector<uint8_t> &out,
               std::string &error);

// The Screen Shot action's TGA (the launch's /sstga): the frame run-length encoded the way the game's
// capture writes it. `rgba` as tga_write_rgba32 (its alpha unread: the capture's pixels are 24-bit).
// The header: no image ID, no colour map, image type 10 (run-length true colour), the origin (0, 0),
// the sides, 24 bits a pixel, the descriptor 0 (no alpha bits, the first row the bottom one). The
// packets walk the rows from the bottom up, each packet carrying on into the next row written:
// - a pixel equal to the pixel after it opens a run: one more pixel each step while the pixel after
//   the newest one equals the first, at most 128, then the byte 0x80 | (count - 1) and the colour's
//   B, G, R;
// - any other opens a literal: one more pixel each step unless the next two are equal, at most 128,
//   stopping too after a pixel equal to the one after it; then the byte count - 1 and each pixel's
//   B, G, R.
// "The pixel after" is the next one in the frame's memory, top row first: past a row's last pixel that
// is the first pixel of the row below it on screen (the row written before), while the packet goes on
// with the first pixel of the row above (the next one written). So a run that reaches a row's end takes
// the next row's first pixel in its colour whenever the row below starts in that colour, whatever that
// pixel holds (a reader sees it so), and a run never goes on into the next row on its own colour alone.
// The capture's own reads outside the frame are none here: the first row written compares its last
// pixel with the word past the frame's end (a pixel outside matches nothing), and a packet ends at the
// image's last pixel, where the game's runs on into the memory before its buffer (a reader stops at
// the image's last pixel either way). False, with `error`, as tga_write_rgba32 refuses.
// [orig: Screenshot_SaveTGARLE @0x51FF20 - the header @0x51FFA8..0x51FFE7 (type 10 @0x51FFD3, the sides
//  @0x51FFD8 / @0x51FFDD, 24 bits @0x51FFE2), the rows from height - 1 down @0x51FFEC / @0x5201AD, the run
//  test @0x520016..0x520032, the run @0x520050..0x5200AF (the cap @0x520050, the next row's start
//  @0x520069..0x520072, the pixel after the newest @0x520074..0x52007F, the 0x80 | count word
//  @0x520090..0x5200AF), the literal @0x5200BC..0x520193 (the pair ahead @0x520107..0x520112, the cap
//  @0x520114, the next row's start @0x520126..0x520135, the pair at the newest @0x52015A..0x520169, the
//  count byte and the pixels @0x52017D / @0x520193); its pixels Screenshot_CaptureTGA @0x5201E0 (the
//  viewport's sides @0x5201F6..0x52020F, the "Snap Buffer for TGA ss" @0x520217, the back buffer
//  @0x520241, top row first: CGfxDevice_CaptureBackbufferToARGB @0x6772E0)]
bool tga_write_screenshot(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                          std::string &error);

// The size a TGA's 18-byte header states: the width and height at bytes 12 and 14 (tga.cpp
// holds the witnesses). False when `size` is shorter than the header or the image type at
// byte 2 is none of the TGA format's image forms: colour-mapped (1), true-colour (2) and grey
// (3), raw, and their run-length forms (9, 10, 11). A side may be 0.
bool tga_header_size(const uint8_t *bytes, size_t size, uint32_t &width, uint32_t &height);

// The 18-byte header as the file states it (the TGA format's fields; the game's readers read the
// image type, the colour map's length and entry size, the sides, the depth and the ID's length).
struct TgaHeader {
	uint8_t id_length = 0;
	uint8_t colour_map_type = 0;
	uint8_t image_type = 0;
	uint16_t map_first = 0;
	uint16_t map_length = 0;
	uint8_t map_entry_bits = 0;
	uint16_t x_origin = 0;
	uint16_t y_origin = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	uint8_t bits = 0;
	uint8_t descriptor = 0;
	// The descriptor's alpha bits (its low four) and whether it says the first row is the top one
	// (bit 5), which no game reader honours (tga_read.h).
	uint8_t alpha_bits() const { return descriptor & 0x0F; }
	bool top_first() const { return (descriptor & 0x20) != 0; }
	bool run_length() const { return image_type >= 9 && image_type <= 11; }
};
// False when `size` is shorter than the header.
bool tga_read_header(const uint8_t *bytes, size_t size, TgaHeader &out);

} // namespace opennova::tga
