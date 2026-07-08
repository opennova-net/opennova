// The ":fd" bake [orig: Foliage_LoadDefAssets @ 0x601260 tail]: 3x3 alpha
// smoothing (center 4, edges 1, corners 2, >> 4, pow2 wraparound) + the
// exact-0x808080 RGB fold. Expected alpha bytes below are hand-computed for
// the 4x4 / 2x2 vectors, independent of the production kernel.
#include <foliage/fd_bake.h>

#include <cstdio>
#include <cstring>
#include <vector>

using opennova::foliage::bake_fd_rgba;

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Fill a w*h RGBA8 buffer with varied RGB and the given alpha plane.
std::vector<uint8_t> make_rgba(int w, int h, const uint8_t *alpha) {
	std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
	for (int i = 0; i < w * h; ++i) {
		px[i * 4 + 0] = static_cast<uint8_t>(7 + i * 13);   // arbitrary non-0x80 RGB
		px[i * 4 + 1] = static_cast<uint8_t>(201 - i * 5);
		px[i * 4 + 2] = static_cast<uint8_t>(i * 31);
		px[i * 4 + 3] = alpha[i];
	}
	return px;
}

} // namespace

int main() {
	// --- 4x4 hand vector (wraparound exercised at every border pixel) -------
	{
		const uint8_t alpha[16] = {
		    16, 32, 48, 64,
		    80, 96, 112, 128,
		    144, 160, 176, 192,
		    208, 224, 240, 255,
		};
		// Hand-computed: (4*c + edges + 2*corners) >> 4 with (w-1)&/(h-1)& wrap.
		const uint8_t expected[16] = {
		    115, 112, 127, 123,
		    100, 96, 112, 108,
		    163, 160, 175, 171,
		    147, 144, 159, 155,
		};
		auto px = make_rgba(4, 4, alpha);
		if (!expect(bake_fd_rgba(px.data(), 4, 4), "4x4 pow2 input bakes")) return 1;
		for (int i = 0; i < 16; ++i) {
			if (!expect(px[i * 4 + 3] == expected[i], "4x4 smoothed alpha matches the hand vector")) {
				std::fprintf(stderr, "  pixel %d: got %d want %d\n", i, px[i * 4 + 3], expected[i]);
				return 1;
			}
			// The gray fold: every output RGB byte is exactly 0x80.
			if (!expect(px[i * 4 + 0] == 0x80 && px[i * 4 + 1] == 0x80 && px[i * 4 + 2] == 0x80,
			            "RGB folds to exactly 0x808080")) return 1;
		}
	}

	// --- 2x2: wrap makes each 3x3 tap alias onto the 4 texels ---------------
	{
		const uint8_t alpha[4] = {100, 200, 50, 250};
		const uint8_t expected[4] = {181, 118, 156, 143};
		auto px = make_rgba(2, 2, alpha);
		if (!expect(bake_fd_rgba(px.data(), 2, 2), "2x2 pow2 input bakes")) return 1;
		for (int i = 0; i < 4; ++i) {
			if (!expect(px[i * 4 + 3] == expected[i], "2x2 smoothed alpha matches the hand vector")) {
				std::fprintf(stderr, "  pixel %d: got %d want %d\n", i, px[i * 4 + 3], expected[i]);
				return 1;
			}
		}
	}

	// --- flat alpha is a fixed point of the kernel (16a >> 4 = a) -----------
	{
		const uint8_t alpha[16] = {255, 255, 255, 255, 255, 255, 255, 255,
		                           255, 255, 255, 255, 255, 255, 255, 255};
		auto px = make_rgba(4, 4, alpha);
		if (!expect(bake_fd_rgba(px.data(), 4, 4), "flat 4x4 bakes")) return 1;
		for (int i = 0; i < 16; ++i) {
			if (!expect(px[i * 4 + 3] == 255, "opaque alpha stays exactly 255")) return 1;
		}
	}

	// --- non-pow2 rejected, buffer untouched ---------------------------------
	{
		const uint8_t alpha[6] = {1, 2, 3, 4, 5, 6};
		auto px = make_rgba(3, 2, alpha);
		auto before = px;
		if (!expect(!bake_fd_rgba(px.data(), 3, 2), "3x2 (non-pow2 width) is rejected")) return 1;
		if (!expect(px == before, "rejected input is left untouched")) return 1;
		if (!expect(!bake_fd_rgba(px.data(), 0, 2), "zero width is rejected")) return 1;
		if (!expect(!bake_fd_rgba(nullptr, 4, 4), "null pixels are rejected")) return 1;
	}

	std::printf("OK: the :fd bake (3x3 smoothed alpha, wrap, 0x808080 fold)\n");
	return 0;
}
