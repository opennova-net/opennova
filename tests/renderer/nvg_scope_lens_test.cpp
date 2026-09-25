// The NVG view's scoped lens and its polar unwrap
// (runtime/renderer/nvg_scope_lens.h), pinned against the retail witnesses:
// NVG_DrawScopedLens @0x5d1d10 and NVG_RenderSceneToTarget's tail
// @0x5d0a0e..0x5d0eb4.

#include <runtime/renderer/nvg_scope_lens.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
			++failures;                                                           \
		}                                                                       \
	} while (0)

using namespace opennova::renderer;

constexpr double kTwoPi = 6.283185307179586;

bool near(double a, double b, double tolerance = 2.0e-5) {
	return std::fabs(a - b) <= tolerance;
}

// The table's truncated Q22 entries, stated independently of the port.
double q22_sin(int index) {
	const double v = std::sin(static_cast<double>(index & 1023) * kTwoPi / 1024.0);
	return static_cast<double>(static_cast<std::int32_t>(v * 4194304.0)) / 4194304.0;
}

double q22_cos(int index) {
	const double v = std::cos(static_cast<double>(index & 1023) * kTwoPi / 1024.0);
	return static_cast<double>(static_cast<std::int32_t>(v * 4194304.0)) / 4194304.0;
}

// [orig: NVG_RenderSceneToTarget @0x5d0a75..0x5d0e9d]
void test_polar_unwrap_passes() {
	const auto passes = nvg_polar_unwrap_passes();
	for (int p = 0; p < kNvgPolarPasses; ++p) {
		const NvgLensStrip &strip = passes[static_cast<std::size_t>(p)];
		CHECK(strip.size() == static_cast<std::size_t>(kNvgLensStripVertices));
		if (strip.size() != static_cast<std::size_t>(kNvgLensStripVertices))
			continue;
		// Alpha 255 / (p + 1), rgb 0x3F.
		const std::uint32_t color = (static_cast<std::uint32_t>(255 / (p + 1)) << 24) | 0x3F3F3Fu;
		const double row0 = 0.48 - 0.01 * p;
		const double row16 = 0.5 - 0.005 * p;
		for (int k = 0; k <= 64; ++k) {
			const NvgLensVertex &top = strip[static_cast<std::size_t>(2 * k)];
			const NvgLensVertex &bottom = strip[static_cast<std::size_t>(2 * k + 1)];
			// Stop k samples table index 16k - 8 + 2p: a half-stop back, an
			// eighth of a stop further per pass.
			const int index = 16 * k - 8 + 2 * p;
			CHECK(top.x == static_cast<float>(k) && bottom.x == static_cast<float>(k));
			CHECK(top.y == 0.0f && bottom.y == 16.0f);
			CHECK(top.argb == color && bottom.argb == color);
			CHECK(near(top.u0, 0.5 + q22_cos(index) * row0));
			CHECK(near(top.v0, 0.5 - q22_sin(index) * row0));
			CHECK(near(bottom.u0, 0.5 + q22_cos(index) * row16));
			CHECK(near(bottom.v0, 0.5 - q22_sin(index) * row16));
			CHECK(top.u1 == top.u0 && top.v1 == top.v0);
			CHECK(bottom.u1 == bottom.u0 && bottom.v1 == bottom.v0);
		}
	}
	CHECK(passes[0][0].argb == 0xFF3F3F3Fu);
	CHECK(passes[7][0].argb == 0x1F3F3F3Fu);
	// Stop 0 of pass 0 reads index 1016, just below the +X axis.
	CHECK(near(passes[0][0].u0, 0.5 + q22_cos(1016) * 0.48));
	CHECK(passes[0][0].v0 > 0.5f);
}

// [orig: NVG_DrawScopedLens @0x5d1d65..0x5d2328]
void test_lens_bands() {
	// A 640 x 480 screen's overlay rect is (0, 0)..(639, 479): centre
	// (319, 239), ring (479 >> 3) + (479 >> 1) = 298.
	const NvgScopeLens lens = build_nvg_scope_lens(0, 0, 639, 479, 512);
	CHECK(lens.center_x == 319.0f && lens.center_y == 239.0f);
	CHECK(lens.ring_size == 298);
	const double r = 298.0;
	const double radii[5] = {0.0, r * 0.5, r * static_cast<double>(0.7f),
		r * static_cast<double>(0.705f), r * static_cast<double>(0.71f)};
	const double texture[5] = {0.0, 0.324, 0.475, 0.4775, 0.48};
	const int steps[5] = {0, 8, 26, 32, 36};
	const double half_texel = 0.5 / 512.0;
	for (int pass = 0; pass < kNvgLensPasses; ++pass) {
		const int q = pass == 0 ? 0 : pass - 1;
		const double du = (pass & 1) ? half_texel : -half_texel;
		const double dv = ((pass & 2) ? half_texel : -half_texel) + 0.5;
		const double shrink = q * 0.00018;
		for (int band = 0; band < kNvgLensBands; ++band) {
			const NvgLensStrip &strip = lens.passes[static_cast<std::size_t>(pass)]
					[static_cast<std::size_t>(band)];
			CHECK(strip.size() == static_cast<std::size_t>(kNvgLensStripVertices));
			if (strip.size() != static_cast<std::size_t>(kNvgLensStripVertices))
				continue;
			// The rotation: q 2 adds, q 3 subtracts the inner step count << 17
			// of BAM, both rounded to the nearest stop.
			int shift = 0;
			const std::int64_t rotation = static_cast<std::int64_t>(steps[band]) << 17;
			if (q == 2)
				shift = static_cast<int>((rotation + 0x200000) >> 22);
			else if (q == 3)
				shift = static_cast<int>(std::floor(static_cast<double>(0x200000 - rotation) / 4194304.0));
			for (int k = 0; k <= 64; ++k) {
				const int position = 16 * k;
				const int lookup = 16 * k + shift;
				for (int side = 0; side < 2; ++side) {
					const NvgLensVertex &v = strip[static_cast<std::size_t>(2 * k + side)];
					const int edge = band + side;
					const double radius = radii[edge];
					const double t = texture[edge] - shrink * steps[edge];
					CHECK(near(v.x, 319.0 + q22_cos(position) * radius, 1.0e-3));
					CHECK(near(v.y, 239.0 - q22_sin(position) * radius, 1.0e-3));
					CHECK(near(v.u0, q22_cos(lookup) * t + du + 0.5));
					CHECK(near(v.v0, dv - q22_sin(lookup) * t));
					std::uint32_t color = pass == 0 ? kNvgLensTintColor : kNvgLensGlowColor;
					if (pass > 0 && edge < 2)
						color &= 0x00FFFFFFu;
					CHECK(v.argb == color);
				}
			}
		}
	}
	// The rotation lands on whole stops: band 1 (8 steps) keeps its lookup,
	// band 2 (26) moves one entry either way.
	CHECK(near(lens.passes[3][1][0].u0, 1.0 * (0.324 - 2 * 0.00018 * 8) + half_texel + 0.5));
	CHECK(near(lens.passes[4][2][0].u0,
			q22_cos(1023) * (0.475 - 3 * 0.00018 * 26) - half_texel + 0.5));
	// The disc's inner stop is the centre, sampling the scene's centre plus
	// the pass's half-texel offset.
	CHECK(lens.passes[0][0][0].x == 319.0f && lens.passes[0][0][0].y == 239.0f);
	CHECK(near(lens.passes[0][0][0].u0, 0.5 - half_texel));
	CHECK(near(lens.passes[0][0][0].v0, 0.5 - half_texel));
}

// [orig: NVG_DrawScopedLens @0x5d23be..0x5d2770]
void test_lens_ring() {
	const NvgScopeLens lens = build_nvg_scope_lens(0, 0, 639, 479, 512);
	CHECK(lens.ring.size() == static_cast<std::size_t>(kNvgLensStripVertices));
	if (lens.ring.size() != static_cast<std::size_t>(kNvgLensStripVertices))
		return;
	const double inner = static_cast<double>(static_cast<float>(298.0 * static_cast<double>(0.71f)));
	for (int k = 0; k <= 64; ++k) {
		const NvgLensVertex &a = lens.ring[static_cast<std::size_t>(2 * k)];
		const NvgLensVertex &b = lens.ring[static_cast<std::size_t>(2 * k + 1)];
		const float u = static_cast<float>(k) / 64.0f;
		CHECK(a.argb == kNvgLensRingInnerColor && b.argb == kNvgLensRingOuterColor);
		CHECK(a.u0 == u && a.v0 == 0.0f && a.u1 == u && a.v1 == 0.0f);
		CHECK(b.u0 == u && b.v0 == 1.0f && b.u1 == u && b.v1 == 0.0f);
		CHECK(near(a.x, 319.0 + q22_cos(16 * k) * inner, 1.0e-3));
		CHECK(near(a.y, 239.0 - q22_sin(16 * k) * inner, 1.0e-3));
		CHECK(near(b.x, 319.0 + q22_cos(16 * k) * 447.0, 1.0e-3));
		CHECK(near(b.y, 239.0 - q22_sin(16 * k) * 447.0, 1.0e-3));
	}
	// Stop 0 is due right: the outer vertex at 1.5 x 298 = 447.
	CHECK(lens.ring[1].x == 766.0f && lens.ring[1].y == 239.0f);
}

// [orig: NVG_RenderScopedScene @0x5d29e4..0x5d2a2a]
void test_scoped_scene_fov() {
	// 4:3 at 4x: 0.75 x 160 x 0.25 x 0.5 = 15 degrees exactly.
	CHECK(nvg_scoped_scene_fov_q16(0.75f, 4) == 15 << 16);
	// 1 / 3 is stored as a float first, then truncated: 20 degrees.
	CHECK(nvg_scoped_scene_fov_q16(0.75f, 3) == 20 << 16);
	CHECK(nvg_scoped_scene_fov_q16(0.5625f, 1) == 45 << 16);
	CHECK(nvg_scoped_scene_fov_q16(0.75f, 0) == nvg_scoped_scene_fov_q16(0.75f, 1));
}

// [orig: NVG_RenderSightedScene @0x5d2ab8..0x5d2acf]
void test_sighted_scene_fov() {
	CHECK(nvg_sighted_scene_fov_q16(1) == 80 << 16);
	CHECK(nvg_sighted_scene_fov_q16(4) == 20 << 16);
	// 5242880 / 3 = 1747626.67, truncated.
	CHECK(nvg_sighted_scene_fov_q16(3) == 1747626);
	CHECK(nvg_sighted_scene_fov_q16(0) == 80 << 16);
}

} // namespace

int main() {
	test_polar_unwrap_passes();
	test_lens_bands();
	test_lens_ring();
	test_scoped_scene_fov();
	test_sighted_scene_fov();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_nvg_scope_lens OK\n");
	return 0;
}
