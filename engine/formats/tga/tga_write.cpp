#include <formats/tga/tga_write.h>

#include <cstdio>

namespace opennova::tga {

bool tga_encode(const TgaImage &img, std::vector<uint8_t> &out)
{
	if (img.width <= 0 || img.height <= 0 || img.width > 0xFFFF || img.height > 0xFFFF) {
		return false;
	}
	if (img.bpp != 24 && img.bpp != 32) {
		return false;
	}
	const size_t bytes_per_pixel = static_cast<size_t>(img.bpp / 8);
	const size_t row_bytes = static_cast<size_t>(img.width) * bytes_per_pixel;
	const size_t payload = row_bytes * static_cast<size_t>(img.height);
	if (img.pixels.size() != payload) {
		return false;
	}
	out.assign(TGA_HEADER_SIZE, 0);
	out[2] = TGA_IMAGE_TYPE_TRUECOLOR;
	out[12] = static_cast<uint8_t>(img.width & 0xFF);
	out[13] = static_cast<uint8_t>((img.width >> 8) & 0xFF);
	out[14] = static_cast<uint8_t>(img.height & 0xFF);
	out[15] = static_cast<uint8_t>((img.height >> 8) & 0xFF);
	out[16] = static_cast<uint8_t>(img.bpp);
	out[17] = img.bpp == 32 ? 8 : 0; // alpha channel bits; bit 5 clear = bottom-up rows
	out.reserve(TGA_HEADER_SIZE + payload);
	for (int row = img.height - 1; row >= 0; --row) {
		const uint8_t *src = img.pixels.data() + static_cast<size_t>(row) * row_bytes;
		out.insert(out.end(), src, src + row_bytes);
	}
	return true;
}

int tga_write(const char *path, const TgaImage &img)
{
	if (!path) {
		return -1;
	}
	std::vector<uint8_t> bytes;
	if (!tga_encode(img, bytes)) {
		return -1;
	}
	FILE *f = std::fopen(path, "wb");
	if (!f) {
		return -1;
	}
	const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
	return written == bytes.size() ? 0 : -1;
}

} // namespace opennova::tga
