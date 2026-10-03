// The game's TGA reader, a structural port of CTerrainTileData_LoadTGAFromArchive
// (its menu twin CUIImage_LoadTGA @ 0x6647D0 runs the same decode and the same
// unconditional flip, @ 0x66499D..0x6649DD) and of the particle manager's loose leg
// (CTextureData_LoadTGA @ 0x5F7B20).
#include <formats/tga/tga_read.h>

#include <algorithm>
#include <cstring>

namespace opennova::tga {

namespace {

constexpr size_t kHeaderSize = 18;
// The most pixels a byte describes: a run-length packet of a count byte and one
// 24-bit pixel repeats 128 pixels (128 / 4).
constexpr size_t kMaxPixelsPerByte = 32;

} // namespace

// [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570 — the sides multiplied signed
//  @ 0x56E661..0x56E677, the buffer allocated @ 0x56E694 (load code 2 when it fails);
//  CTextureData_LoadTGA @ 0x5F7B20 reads them unsigned]
bool tga_retail_size(const uint8_t *data, size_t size, int &width, int &height, std::string &error,
		TgaReaderForm form) {
	width = height = 0;
	if (data == nullptr || size < kHeaderSize) {
		error = "TGA header truncated";
		return false;
	}
	const uint16_t raw_width = static_cast<uint16_t>(data[12] | (data[13] << 8));
	const uint16_t raw_height = static_cast<uint16_t>(data[14] | (data[15] << 8));
	const int w = form == TgaReaderForm::ParticleLoose ? raw_width : static_cast<int16_t>(raw_width);
	const int h = form == TgaReaderForm::ParticleLoose ? raw_height : static_cast<int16_t>(raw_height);
	if (w <= 0 || h <= 0) {
		error = "TGA has no pixels";
		return false;
	}
	const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
	const size_t start = kHeaderSize + data[0];
	const size_t present = size > start ? size - start : 0;
	if (pixels > kMaxTgaPixels || pixels > present * kMaxPixelsPerByte) {
		error = "TGA names more pixels than its data can describe";
		return false;
	}
	width = w;
	height = h;
	return true;
}

// [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570; CTextureData_LoadTGA @ 0x5F7B20]
bool tga_decode_retail_into(const uint8_t *data, size_t size, uint8_t *rgba, std::string &error,
		TgaReaderForm form) {
	int width = 0, height = 0;
	if (rgba == nullptr || !tga_retail_size(data, size, width, height, error, form)) return false;
	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	std::memset(rgba, 0, pixels * 4u);
	const auto byte_at = [&](size_t offset) -> uint8_t { return offset < size ? data[offset] : 0; };
	// The pixels start after the header and the image ID, colour map or not
	// [orig: @ 0x56E6B4..0x56E6BA].
	size_t src = kHeaderSize + data[0];
	const uint8_t depth = data[16];
	// The reader's A8R8G8B8 words, written as R, G, B, A.
	const auto put = [&](size_t pixel, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
		uint8_t *dst = rgba + pixel * 4u;
		dst[0] = r;
		dst[1] = g;
		dst[2] = b;
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
	const size_t row_bytes = static_cast<size_t>(width) * 4u;
	if (form == TgaReaderForm::Archive) {
		// Every row flipped, the descriptor never consulted [orig: @ 0x56E995..0x56E9EA].
		for (int row = 0; row < height / 2; ++row) {
			uint8_t *top = rgba + static_cast<size_t>(row) * row_bytes;
			uint8_t *bottom = rgba + static_cast<size_t>(height - row - 1) * row_bytes;
			std::swap_ranges(top, top + row_bytes, bottom);
		}
		return true;
	}
	// The loose leg's flip XOR-swaps the top row's words with the words HEIGHT x
	// (height - row - 1) words in, not width x (height - row - 1), for height / 2 rows
	// once the height is at least 2 [orig: CTextureData_LoadTGA @ 0x5F7FB2 (the guard),
	// the bottom offset @ 0x5F7FCD..0x5F7FDE, the XOR swap @ 0x5F7FE5..0x5F7FF2]; a
	// square image flips, any other comes out garbled. Retail reads and writes past its
	// buffer when the height exceeds the width; this port leaves those words alone.
	if (height < 2) return true;
	for (int row = 0; row < height / 2; ++row) {
		const size_t top = static_cast<size_t>(row) * static_cast<size_t>(width);
		const size_t bottom = static_cast<size_t>(height) * static_cast<size_t>(height - row - 1);
		for (int col = 0; col < width; ++col) {
			const size_t t = top + static_cast<size_t>(col);
			const size_t b = bottom + static_cast<size_t>(col);
			if (t >= pixels || b >= pixels) continue;
			uint8_t *tw = rgba + t * 4u;
			uint8_t *bw = rgba + b * 4u;
			for (int k = 0; k < 4; ++k) {
				tw[k] ^= bw[k];
				bw[k] ^= tw[k];
				tw[k] ^= bw[k];
			}
		}
	}
	return true;
}

bool tga_decode_retail(const uint8_t *data, size_t size, TgaImage &out, std::string &error,
		TgaReaderForm form) {
	out = TgaImage{};
	int width = 0, height = 0;
	if (!tga_retail_size(data, size, width, height, error, form)) return false;
	std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	if (!tga_decode_retail_into(data, size, rgba.data(), error, form)) return false;
	out.width = width;
	out.height = height;
	out.rgba = std::move(rgba);
	return true;
}

} // namespace opennova::tga
