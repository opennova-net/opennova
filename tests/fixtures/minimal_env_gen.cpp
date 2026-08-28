// Generator + guard for the two sky maps fixtures/env/full_00.env names
// (sky_map1 Cloud01.pcx, "full range cloud image, square rooted (bright
// gamma)"; sky_map2 Cloud01b.pcx, "subtile modulation layer, centered at
// 128"): 64x64 indexed PCX files minted by our own writer
// (encode_pcx_indexed) from integer data. The sky dome / env tests resolve
// them beside the .env through the shared texture resolver and edit the
// bindings; no retail image is carried.
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using opennova::IndexedImage8;

#include "common/file_io.h"

namespace {

constexpr int kSize = 64;

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

// A grayscale ramp palette: index == luminance.
void gray_palette(IndexedImage8 &image) {
	for (int i = 0; i < 256; ++i) {
		image.palette[i][0] = static_cast<uint8_t>(i);
		image.palette[i][1] = static_cast<uint8_t>(i);
		image.palette[i][2] = static_cast<uint8_t>(i);
	}
}

// Two overlapping integer-phase sines, square rooted into the bright gamma
// the retail comment describes: a full-range cloud field with soft lobes.
IndexedImage8 make_cloud_map() {
	IndexedImage8 image;
	image.width = kSize;
	image.height = kSize;
	image.indices.resize(static_cast<size_t>(kSize) * kSize);
	gray_palette(image);
	for (int y = 0; y < kSize; ++y) {
		for (int x = 0; x < kSize; ++x) {
			const double u = static_cast<double>(x) / kSize * 6.283185307179586;
			const double v = static_cast<double>(y) / kSize * 6.283185307179586;
			const double field = 0.5 + 0.25 * std::sin(u * 2.0 + v) + 0.25 * std::sin(v * 3.0 - u * 0.5);
			const double bright = std::sqrt(field < 0.0 ? 0.0 : field);
			const int value = static_cast<int>(std::lround(bright * 255.0));
			image.indices[static_cast<size_t>(y) * kSize + x] =
					static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		}
	}
	return image;
}

// The modulation layer: a low-amplitude ripple centered at 128.
IndexedImage8 make_modulation_map() {
	IndexedImage8 image;
	image.width = kSize;
	image.height = kSize;
	image.indices.resize(static_cast<size_t>(kSize) * kSize);
	gray_palette(image);
	for (int y = 0; y < kSize; ++y) {
		for (int x = 0; x < kSize; ++x) {
			const double u = static_cast<double>(x) / kSize * 6.283185307179586;
			const double v = static_cast<double>(y) / kSize * 6.283185307179586;
			const double ripple = 24.0 * std::sin(u * 4.0) * std::cos(v * 4.0);
			image.indices[static_cast<size_t>(y) * kSize + x] =
					static_cast<uint8_t>(128 + static_cast<int>(std::lround(ripple)));
		}
	}
	return image;
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
	return expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/env";
	int failures = 0;
	std::string err;

	std::vector<uint8_t> cloud_bytes;
	failures += !expect(opennova::encode_pcx_indexed(make_cloud_map(), cloud_bytes, err), ("encode_pcx_indexed: " + err).c_str());
	IndexedImage8 cloud_back;
	failures += !expect(opennova::decode_pcx_indexed(cloud_bytes.data(), cloud_bytes.size(), cloud_back, err) &&
	                            cloud_back.width == kSize && cloud_back.height == kSize &&
	                            cloud_back.indices == make_cloud_map().indices,
	                    "cloud01.pcx decodes back to the minted field");
	failures += guard(dir + "/cloud01.pcx", cloud_bytes, write_mode);

	std::vector<uint8_t> modulation_bytes;
	failures += !expect(opennova::encode_pcx_indexed(make_modulation_map(), modulation_bytes, err), ("encode_pcx_indexed (b): " + err).c_str());
	IndexedImage8 modulation_back;
	failures += !expect(opennova::decode_pcx_indexed(modulation_bytes.data(), modulation_bytes.size(), modulation_back, err) &&
	                            modulation_back.indices[0] == 128,
	                    "cloud01b.pcx decodes with its 128 center");
	failures += guard(dir + "/cloud01b.pcx", modulation_bytes, write_mode);

	if (failures == 0 && !write_mode) std::printf("OK: fixtures/env cloud maps byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
