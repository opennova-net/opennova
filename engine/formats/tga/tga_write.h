// Truecolor TGA encoder (write-only). Decoding stays with the platform image
// loader (Godot's Image, godot/src/util/texture_path_resolver.cpp); the
// engine only ever has to PRODUCE the art a model or a menu names, and retail
// consumes exactly one shape of it: an uncompressed truecolor TGA (image type
// 2) at 24 or 32 bpp with the rows stored bottom-up, the form every retail
// loose texture and the menu cursor ship in (tests/fixtures/
// minimal_art_validate.cpp pins the cursor's). An industry format, not a
// NovaLogic one, so nothing here is a port; it earns its formats/ home
// because the authored set's textures must be minted by our own writer from
// scratch (ADR 0003) like every other artifact in assets/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::tga {

// One image in memory. `pixels` holds width * height * (bpp / 8) bytes in
// BGR (24 bpp) or BGRA (32 bpp) byte order, rows TOP-DOWN as a raster editor
// hands them over; the encoder stores them bottom-up on disk.
struct TgaImage {
	int width = 0;
	int height = 0;
	int bpp = 24;
	std::vector<uint8_t> pixels;
};

inline constexpr size_t TGA_HEADER_SIZE = 18;
inline constexpr uint8_t TGA_IMAGE_TYPE_TRUECOLOR = 2;

// Encode `img` as an image type 2 TGA: 18-byte header (no id field, no color
// map), the descriptor carrying 8 alpha bits for 32 bpp and 0 for 24 bpp with
// the top-down bit clear, then the pixel rows bottom-up. Returns false when
// the image is empty, larger than 65535 on a side, not 24 or 32 bpp, or its
// pixel buffer does not match its dimensions.
bool tga_encode(const TgaImage &img, std::vector<uint8_t> &out);

// tga_encode to a file. Returns 0 on success, -1 when the image is rejected
// or the file cannot be written.
int tga_write(const char *path, const TgaImage &img);

} // namespace opennova::tga
