// Generator + guard for fixtures/cbin/credits_image.png (the 8x8 RGB image
// the credits (.kda) tests stage beside a .kda under the name its ~F image
// row references; a self-contained PNG writer with one stored zlib block, so
// no compressor is involved and every platform mints the same bytes; the
// pixels are an integer gradient) and fixtures/cbin/particle_dot.tga (the
// 8x8 BGRA sprite the effect-world test's synthetic particle file names as
// its layer texture: a soft white dot on transparent). No retail image is
// carried.
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace {

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 8;

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

uint32_t crc32_of(const uint8_t *data, size_t size) {
	static uint32_t table[256];
	static bool built = false;
	if (!built) {
		for (uint32_t n = 0; n < 256; ++n) {
			uint32_t c = n;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[n] = c;
		}
		built = true;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

void put_be32(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v >> 24));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
	put_be32(out, static_cast<uint32_t>(data.size()));
	std::vector<uint8_t> crc_input(type, type + 4);
	crc_input.insert(crc_input.end(), data.begin(), data.end());
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data.begin(), data.end());
	put_be32(out, crc32_of(crc_input.data(), crc_input.size()));
}

std::vector<uint8_t> make_png() {
	std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

	std::vector<uint8_t> ihdr;
	put_be32(ihdr, kWidth);
	put_be32(ihdr, kHeight);
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
	put_chunk(out, "IHDR", ihdr);

	// Raw scanlines: filter byte 0 + RGB per pixel; an integer gradient.
	std::vector<uint8_t> raw;
	for (uint32_t y = 0; y < kHeight; ++y) {
		raw.push_back(0);
		for (uint32_t x = 0; x < kWidth; ++x) {
			raw.push_back(static_cast<uint8_t>(x * 32));
			raw.push_back(static_cast<uint8_t>(y * 32));
			raw.push_back(static_cast<uint8_t>(255 - (x + y) * 16));
		}
	}
	// zlib: header, one final stored block, adler32.
	std::vector<uint8_t> idat = {0x78, 0x01, 0x01};
	const uint16_t len = static_cast<uint16_t>(raw.size());
	idat.push_back(static_cast<uint8_t>(len & 0xFF));
	idat.push_back(static_cast<uint8_t>(len >> 8));
	idat.push_back(static_cast<uint8_t>(~len & 0xFF));
	idat.push_back(static_cast<uint8_t>((~len >> 8) & 0xFF));
	idat.insert(idat.end(), raw.begin(), raw.end());
	uint32_t a = 1, b = 0;
	for (uint8_t byte : raw) {
		a = (a + byte) % 65521u;
		b = (b + a) % 65521u;
	}
	put_be32(idat, (b << 16) | a);
	put_chunk(out, "IDAT", idat);
	put_chunk(out, "IEND", {});
	return out;
}

// An uncompressed 32-bit true-color TGA (image type 2, bottom-left origin,
// 8 alpha bits): the 18-byte header, then BGRA per pixel. The dot is a
// radial alpha falloff on white, so a blended particle draw has coverage.
std::vector<uint8_t> make_tga() {
	std::vector<uint8_t> out(18, 0);
	out[2] = 2;  // uncompressed true-color
	out[12] = static_cast<uint8_t>(kWidth & 0xFF);
	out[13] = static_cast<uint8_t>(kWidth >> 8);
	out[14] = static_cast<uint8_t>(kHeight & 0xFF);
	out[15] = static_cast<uint8_t>(kHeight >> 8);
	out[16] = 32;  // bits per pixel
	out[17] = 8;   // 8 alpha bits, bottom-left origin
	for (uint32_t y = 0; y < kHeight; ++y) {
		for (uint32_t x = 0; x < kWidth; ++x) {
			// Integer radial falloff from the center (3.5, 3.5): 255 at the
			// middle four texels, 0 at the corners.
			const int dx = static_cast<int>(x) * 2 - 7;
			const int dy = static_cast<int>(y) * 2 - 7;
			const int distance_sq = dx * dx + dy * dy;  // 2 .. 98
			int alpha = 255 - (distance_sq - 2) * 255 / 96;
			if (alpha < 0) alpha = 0;
			out.push_back(255);  // B
			out.push_back(255);  // G
			out.push_back(255);  // R
			out.push_back(static_cast<uint8_t>(alpha));
		}
	}
	return out;
}

using test_io::read_file;

// 0 = byte-identical (or written), 1 = mismatch/missing.
int guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str())) return 1;
	static const char kLfsSentinel[] = "version https://git-lfs";
	if (committed.size() >= sizeof(kLfsSentinel) - 1 &&
	    std::memcmp(committed.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str())) return 1;
	std::printf("OK: %s byte-reproducible (%zu bytes)\n", path.c_str(), bytes.size());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/cbin";
	int failures = 0;
	failures += guard(dir + "/credits_image.png", make_png(), write_mode);
	failures += guard(dir + "/particle_dot.tga", make_tga(), write_mode);
	return failures == 0 ? 0 : 1;
}
