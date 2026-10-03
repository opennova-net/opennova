// TGA: the true-colour image the game's model, menu and particle loaders read (tga.cpp
// holds the witnesses). The writer makes the one shape all of them take: an uncompressed
// 32-bit true-colour image (image type 2, 32 bits a pixel, 8 of them alpha, no image ID, no
// colour map), its pixels B, G, R, A from the bottom row up, the header saying so. The writer,
// a header's size, and the game's own decode of the pixels (tga_decode_game: the editor's texture
// document shows a file as the game reads it, ADR 0046 S18); the runtime decodes a TGA through its
// embedder (the shell's Godot decoder), and the editor's menu render reads a header's size alone
// (editor/preview/texture_header).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::tga {

inline constexpr size_t TGA_HEADER_SIZE = 18;
inline constexpr uint8_t TGA_TYPE_TRUE_COLOR = 2;
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
	// (bit 5), which no game reader honours (tga_decode_game).
	uint8_t alpha_bits() const { return descriptor & 0x0F; }
	bool top_first() const { return (descriptor & 0x20) != 0; }
	bool run_length() const { return image_type >= 9 && image_type <= 11; }
};
// False when `size` is shorter than the header.
bool tga_read_header(const uint8_t *bytes, size_t size, TgaHeader &out);

// What the game's TGA reader makes of a file's pixels (tga_decode_game).
enum class TgaPixels : uint8_t {
	Decoded, // a form it decodes: the pixels the file holds
	Blank, // a form it zeroes: every texel transparent black (image types 9 and 11, a true-colour or
		   // run-length image of another depth than 24 or 32, a colour map of another entry size than 24)
	Unset, // a form it leaves as its buffer was allocated, never written (a grey image of another
		   // depth than 8, any image type but 1, 2, 3, 9, 10 and 11): no pixels the file says; the port
		   // reads it as blank
};

// A TGA as the game's readers decode it (tga.cpp holds the witnesses): the header, what the reader
// made of its pixels, and the image, R, G, B, A a texel from the top row down as the game's buffer
// holds it after its flip (the file's rows taken bottom-up whatever its descriptor says). A
// colour-mapped image keeps its map (R, G, B an entry, the file's 24-bit entries) and each texel's
// index too. The sides are the header's as the reader takes them, signed 16-bit (a side of 0 or past
// 32767 makes no texel).
struct TgaImage {
	TgaHeader header;
	TgaPixels pixels = TgaPixels::Unset;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;
	std::vector<uint8_t> palette;
	std::vector<uint8_t> indices;
};
// The game's TGA reader over a file's bytes (decoded already: the archive reader unpacks a BFC1
// file first, tga.cpp): false, with `error`, for bytes shorter than the header. Bytes the file does
// not hold read as 0 (the reader reads past them).
bool tga_decode_game(const uint8_t *bytes, size_t size, TgaImage &out, std::string &error);

} // namespace opennova::tga
