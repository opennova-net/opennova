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

// A quad covers the pixels whose centres (the integers) lie in [x0, x1) x [y0, y1):
// from ceil(x0) up to, not including, ceil(x1).
void test_quad_pixels() {
	const PixelRect whole = d3d9_quad_pixels(10.0f, 20.0f, 30.0f, 40.0f);
	CHECK(whole.x0 == 10 && whole.y0 == 20 && whole.x1 == 30 && whole.y1 == 40);
	// A menu frame's piece at 1024 x 768: (5.12, -0.0) to (15.36, 10.24).
	const PixelRect piece = d3d9_quad_pixels(5.12f, 0.0f, 15.36f, 10.24f);
	CHECK(piece.x0 == 6 && piece.y0 == 0 && piece.x1 == 16 && piece.y1 == 11);
	const PixelRect thin = d3d9_quad_pixels(5.2f, 1.0f, 5.9f, 2.0f);
	CHECK(thin.x1 - thin.x0 == 0); // no pixel centre inside: nothing drawn
}

// A line-list segment lights its first point's pixel through the one before its
// last point's: an outline of four segments round a rect lights every pixel of its
// perimeter once, the corners included [orig: CUIElement_DrawOutlineRect @ 0x647fc0].
void test_axis_line_pixels() {
	PixelRect px;
	CHECK(d3d9_axis_line_pixels(256.0f, 192.0f, 767.0f, 192.0f, &px));
	CHECK(px.x0 == 256 && px.x1 == 767 && px.y0 == 192 && px.y1 == 193); // top: 256..766
	CHECK(d3d9_axis_line_pixels(767.0f, 192.0f, 767.0f, 536.0f, &px));
	CHECK(px.x0 == 767 && px.x1 == 768 && px.y0 == 192 && px.y1 == 536); // right: 192..535
	CHECK(d3d9_axis_line_pixels(767.0f, 536.0f, 256.0f, 536.0f, &px));
	CHECK(px.x0 == 257 && px.x1 == 768 && px.y0 == 536 && px.y1 == 537); // bottom: 767..257
	CHECK(d3d9_axis_line_pixels(256.0f, 536.0f, 256.0f, 192.0f, &px));
	CHECK(px.x0 == 256 && px.x1 == 257 && px.y0 == 193 && px.y1 == 537); // left: 536..193
	// Off the integers, diagonal or of no length: the device draws its own way.
	CHECK(!d3d9_axis_line_pixels(10.5f, 4.0f, 20.0f, 4.0f, &px));
	CHECK(!d3d9_axis_line_pixels(10.0f, 4.0f, 20.0f, 8.0f, &px));
	CHECK(!d3d9_axis_line_pixels(10.0f, 4.0f, 10.0f, 4.0f, &px));
}

} // namespace

int main() {
	test_pixel_centre();
	test_ndc_shift();
	test_near_plane_offset_moves_the_image();
	test_quad_pixels();
	test_axis_line_pixels();
	if (failures != 0) {
		std::printf("d3d9_raster: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("OK: d3d9_raster\n");
	return 0;
}
