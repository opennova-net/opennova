// Generator + guard for fixtures/cbin/credits_image.png: the 8x8 RGB image
// the credits (.kda) tests stage beside a .kda under the name its ~F image
// row references. A self-contained PNG writer (one stored zlib block, so
// no compressor is involved and every platform mints the same bytes); the
// pixels are an integer gradient. No retail image is carried.
//
// Default: rebuild in memory and byte-compare the committed file. `--write`
// (re)writes it.
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	out.resize(static_cast<size_t>(sz));
	if (!out.empty()) f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	return true;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/cbin/credits_image.png";
	const std::vector<uint8_t> bytes = make_png();
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), "committed fixtures/cbin/credits_image.png missing; run with --write")) return 1;
	static const char kLfsSentinel[] = "version https://git-lfs";
	if (committed.size() >= sizeof(kLfsSentinel) - 1 &&
	    std::memcmp(committed.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
		std::printf("[skip] credits_image.png is an unpulled LFS pointer\n");
		return 0;
	}
	if (!expect(committed == bytes, "credits_image.png differs from the generator output; regenerate with --write")) return 1;
	std::printf("OK: credits_image.png byte-reproducible (%zu bytes)\n", bytes.size());
	return 0;
}
