// The game's TGA reader, a structural port of CTerrainTileData_LoadTGAFromArchive
// (its menu and particle twins CUIImage_LoadTGA @ 0x6647D0 and CTextureData_LoadTGA
// @ 0x5F7B20 run the same decode and the same unconditional flip, @ 0x66499D..0x6649DD
// and @ 0x5F7FE8..0x5F7FF2).
#include <formats/tga/tga_read.h>

namespace opennova::tga {

namespace {

constexpr size_t kHeaderSize = 18;

} // namespace

// [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570]
bool tga_decode_retail(const uint8_t *data, size_t size, TgaImage &out, std::string &error) {
	out = TgaImage{};
	if (data == nullptr || size < kHeaderSize) {
		error = "TGA header truncated";
		return false;
	}
	const auto byte_at = [&](size_t offset) -> uint8_t { return offset < size ? data[offset] : 0; };
	// The sides are the header's 16-bit words, multiplied signed [orig: @ 0x56E661..0x56E677].
	const int16_t width = static_cast<int16_t>(data[12] | (data[13] << 8));
	const int16_t height = static_cast<int16_t>(data[14] | (data[15] << 8));
	if (width <= 0 || height <= 0) {
		error = "TGA has no pixels";
		return false;
	}
	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	// B, G, R, A per pixel, as the reader's A8R8G8B8 buffer holds them.
	std::vector<uint8_t> bgra(pixels * 4u, 0);
	// The pixels start after the header and the image ID, colour map or not
	// [orig: @ 0x56E6B4..0x56E6BA].
	size_t src = kHeaderSize + data[0];
	const uint8_t depth = data[16];
	const auto put = [&](size_t pixel, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
		uint8_t *dst = &bgra[pixel * 4u];
		dst[0] = b;
		dst[1] = g;
		dst[2] = r;
		dst[3] = a;
	};
	// The run-length forms: a packet byte whose top bit is clear copies (count + 1)
	// pixels and stops the decode at the image's end; one whose top bit is set
	// repeats one pixel (count + 1) times, clipped at the end
	// [orig: case 10 @ 0x56E7FB (24-bit) / @ 0x56E8B6 (32-bit)].
	const auto run_length = [&](size_t bytes_per_pixel) {
		size_t pixel = 0;
		while (pixel < pixels) {
			const uint8_t packet = byte_at(src++);
			const int count = (packet & 0x7F) + 1;
			if ((packet & 0x80) == 0) {
				for (int n = 0; n < count; ++n) {
					if (pixel >= pixels) return;
					put(pixel++, byte_at(src), byte_at(src + 1), byte_at(src + 2),
							bytes_per_pixel == 4 ? byte_at(src + 3) : 0xFF);
					src += bytes_per_pixel;
				}
			} else {
				for (int n = 0; n < count && pixel < pixels; ++n)
					put(pixel++, byte_at(src), byte_at(src + 1), byte_at(src + 2),
							bytes_per_pixel == 4 ? byte_at(src + 3) : 0xFF);
				src += bytes_per_pixel;
			}
		}
	};
	// [orig: the image-type switch @ 0x56E6D2]
	switch (data[2]) {
		case 1:
			// 8-bit indices after a map of 24-bit entries; another entry size reads
			// as zeros [orig: @ 0x56E6D9; the map length @ 0x56E6DF; the zero fill
			// @ 0x56E979]. The index depth (byte 16) is not read.
			if (data[7] == 24) {
				const size_t map = src;
				const size_t indices = map + 3u * static_cast<size_t>(data[5] | (data[6] << 8));
				for (size_t i = 0; i < pixels; ++i) {
					const size_t entry = map + 3u * byte_at(indices + i);
					put(i, byte_at(entry), byte_at(entry + 1), byte_at(entry + 2), 0xFF);
				}
			}
			break;
		case 2:
			// [orig: 24-bit @ 0x56E74F, 32-bit copied whole @ 0x56E796..0x56E7A5;
			// another depth zero-filled @ 0x56E979]
			if (depth == 24) {
				for (size_t i = 0; i < pixels; ++i, src += 3)
					put(i, byte_at(src), byte_at(src + 1), byte_at(src + 2), 0xFF);
			} else if (depth == 32) {
				for (size_t i = 0; i < pixels; ++i, src += 4)
					put(i, byte_at(src), byte_at(src + 1), byte_at(src + 2), byte_at(src + 3));
			}
			break;
		case 3:
			// 8-bit grey [orig: @ 0x56E7AF]; another depth leaves retail's buffer
			// unfilled, which reads as zeros here.
			if (depth == 8) {
				for (size_t i = 0; i < pixels; ++i) {
					const uint8_t grey = byte_at(src + i);
					put(i, grey, grey, grey, 0xFF);
				}
			}
			break;
		case 10:
			if (depth == 24) run_length(3);
			else if (depth == 32) run_length(4);
			break;
		default:
			// 9 and 11 zero-filled [orig: @ 0x56E73F, @ 0x56E971 -> memset @ 0x56E97C];
			// any other type leaves retail's buffer unfilled (zeros here).
			break;
	}
	// Every row flipped, the descriptor never consulted [orig: @ 0x56E995..0x56E9EA].
	for (int row = 0; row < height / 2; ++row) {
		uint8_t *top = &bgra[static_cast<size_t>(row) * width * 4u];
		uint8_t *bottom = &bgra[static_cast<size_t>(height - row - 1) * width * 4u];
		for (size_t i = 0; i < static_cast<size_t>(width) * 4u; ++i) {
			const uint8_t swap = top[i];
			top[i] = bottom[i];
			bottom[i] = swap;
		}
	}
	out.width = width;
	out.height = height;
	out.rgba.resize(pixels * 4u);
	for (size_t i = 0; i < pixels; ++i) {
		out.rgba[4 * i + 0] = bgra[4 * i + 2];
		out.rgba[4 * i + 1] = bgra[4 * i + 1];
		out.rgba[4 * i + 2] = bgra[4 * i + 0];
		out.rgba[4 * i + 3] = bgra[4 * i + 3];
	}
	return true;
}

} // namespace opennova::tga
