// The ":fd" foliage texture bake - flat-0x808080 RGB + 3x3-smoothed alpha.
// [orig: Foliage_LoadDefAssets @ 0x601260 tail - kernel center 4, N/S/E/W
// edges 1, corners 2, sum >> 4, power-of-two wraparound; RGB fold
// out = 0x808080 | (px & 0xFF808080). See docs/foliage/foliage-re.md.]
#include "foliage/fd_bake.h"

#include <vector>

namespace opennova::foliage {

namespace {

inline bool is_pow2(int v) noexcept {
	return v > 0 && (v & (v - 1)) == 0;
}

} // namespace

bool bake_fd_rgba(uint8_t *pixels, int width, int height) noexcept {
	if (pixels == nullptr || !is_pow2(width) || !is_pow2(height)) {
		return false;
	}

	// Snapshot the source alpha plane: the witnessed kernel is a convolution
	// over the ORIGINAL pixels, not a progressive smear.
	std::vector<uint8_t> src_alpha(static_cast<size_t>(width) * height);
	for (size_t i = 0; i < src_alpha.size(); ++i) {
		src_alpha[i] = pixels[i * 4 + 3];
	}

	const int xm = width - 1;   // pow2 wrap masks [orig: (w-1)& / (h-1)&]
	const int ym = height - 1;
	auto a_at = [&](int x, int y) -> uint32_t {
		return src_alpha[static_cast<size_t>(y & ym) * width + (x & xm)];
	};

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			// center 4, N/S/E/W edges 1, corners 2; 4 + 4*1 + 4*2 = 16, >> 4.
			uint32_t sum = 4u * a_at(x, y);
			sum += a_at(x - 1, y) + a_at(x + 1, y) + a_at(x, y - 1) + a_at(x, y + 1);
			sum += 2u * (a_at(x - 1, y - 1) + a_at(x + 1, y - 1) +
			             a_at(x - 1, y + 1) + a_at(x + 1, y + 1));
			uint8_t *px = pixels + (static_cast<size_t>(y) * width + x) * 4;
			// RGB flattened to exactly 0x808080; alpha = the smoothed value.
			px[0] = 0x80;
			px[1] = 0x80;
			px[2] = 0x80;
			px[3] = static_cast<uint8_t>(sum >> 4);
		}
	}
	return true;
}

} // namespace opennova::foliage
