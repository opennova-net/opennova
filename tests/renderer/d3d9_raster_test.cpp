// The D3D9 pixel-centre rule (runtime/renderer/d3d9_raster.h): retail rasterises
// through Direct3D 9, pixel i's centre on the integer window coordinate, so a
// raster whose centres sit at i + 0.5 draws its image half a pixel right and down
// [orig: Render_SetViewAndProjectionMatrices @0x58D9DE; Render_SetViewport
// @0x58A720; GDynamicVB_DrawPrimitive @0x6788E0].

#include <runtime/renderer/d3d9_raster.h>

#include <cmath>
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

bool near(double a, double b, double tolerance = 1.0e-9) {
	return std::fabs(a - b) <= tolerance;
}

// A D3D9 window coordinate is half a pixel short of the same point on an
// i + 0.5 raster.
void test_pixel_centre() {
	CHECK(kD3d9PixelCentre == 0.5f);
}

// Half a pixel right and half a pixel down, in NDC with y up: 2 * 0.5 / W and
// -2 * 0.5 / H; a raster with no pixels takes no shift.
void test_ndc_shift() {
	const NdcShift xga = d3d9_raster_ndc_shift(1024, 768);
	CHECK(xga.x == 1.0f / 1024.0f);
	CHECK(xga.y == -1.0f / 768.0f);
	const NdcShift square = d3d9_raster_ndc_shift(512, 512);
	CHECK(square.x == 1.0f / 512.0f && square.y == -1.0f / 512.0f);
	const NdcShift none = d3d9_raster_ndc_shift(0, 768);
	CHECK(none.x == 0.0f && none.y == 0.0f);
	const NdcShift negative = d3d9_raster_ndc_shift(1024, -1);
	CHECK(negative.x == 0.0f && negative.y == 0.0f);
}

// A frustum moved by near_plane_offset draws every point exactly `shift` away in
// NDC: x_ndc = (2 x_near - (r + l)) / (r - l) with l, r moved by the offset.
void test_near_plane_offset_moves_the_image() {
	const double n = 0.2;
	const double tan_half_h = std::tan(40.0 * 3.14159265358979323846 / 180.0);
	const double width = 2.0 * n * tan_half_h;
	const double height = width * 768.0 / 1024.0;
	const NdcShift shift = d3d9_raster_ndc_shift(1024, 768);
	const NearPlaneOffset offset =
			near_plane_offset(shift, static_cast<float>(width), static_cast<float>(height));
	CHECK(offset.x < 0.0f); // the window moves left, the image right
	CHECK(offset.y > 0.0f); // the window moves up, the image down
	const double l = -width * 0.5 + offset.x;
	const double r = width * 0.5 + offset.x;
	const double b = -height * 0.5 + offset.y;
	const double t = height * 0.5 + offset.y;
	// A view-space point (x, y, -z) projected onto the near plane.
	const double px = 0.37 * n / 3.0;
	const double py = -0.21 * n / 3.0;
	const double centred_x = 2.0 * px / width;
	const double centred_y = 2.0 * py / height;
	const double moved_x = (2.0 * px - (r + l)) / (r - l);
	const double moved_y = (2.0 * py - (t + b)) / (t - b);
	CHECK(near(moved_x - centred_x, shift.x, 1.0e-7));
	CHECK(near(moved_y - centred_y, shift.y, 1.0e-7));
	// In window pixels, the D3D9 half pixel: x right, y down.
	CHECK(near((moved_x - centred_x) * 1024.0 * 0.5, 0.5, 1.0e-4));
	CHECK(near(-(moved_y - centred_y) * 768.0 * 0.5, 0.5, 1.0e-4));
}

} // namespace

int main() {
	test_pixel_centre();
	test_ndc_shift();
	test_near_plane_offset_moves_the_image();
	if (failures != 0) {
		std::printf("d3d9_raster: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("OK: d3d9_raster\n");
	return 0;
}
