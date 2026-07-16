#include <til/til.h>

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool expect_close(float actual, float expected, const char *message) {
	if (std::fabs(actual - expected) <= 0.00001f) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s (actual=%f expected=%f)\n", message, actual, expected);
	return false;
}

} // namespace

int main() {
	const opennova::TilAtlasLayout layout = opennova::til_make_atlas_layout(256, 128);
	if (!expect(layout.tiles_x == 4 && layout.tiles_y == 2, "atlas layout should derive tile rows/columns from strip size")) return 1;
	if (!expect_close(layout.step_u, 0.25f, "atlas U step should be 1 / tiles_x")) return 1;
	if (!expect_close(layout.step_v, 0.5f, "atlas V step should be 1 / tiles_y")) return 1;

	const opennova::TilUvQuad rotated =
	    opennova::til_build_entry_uv_quad(6,
	                                      static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_X |
	                                                           opennova::TIL_FLAG_FLIP_Y |
	                                                           opennova::TIL_FLAG_ROTATE_90),
	                                      256,
	                                      128);
	if (!expect(rotated.valid, "valid atlas dimensions should produce a UV quad")) return 1;

	// FLIP_X then FLIP_Y then the retail CCW ROTATE_90
	// [orig: render_water_quad flag order @ 0x604782/0x6047a9/0x6047d4]:
	// TL (0,0) -> (1,0) -> (1,1) -> (0,1); tile 6 origin (0.5, 0.5).
	if (!expect_close(rotated.corners[0].u, 0.5f, "TL U should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;
	if (!expect_close(rotated.corners[0].v, 1.0f, "TL V should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;
	if (!expect_close(rotated.corners[1].u, 0.5f, "TR U should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;
	if (!expect_close(rotated.corners[1].v, 0.5f, "TR V should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;
	if (!expect_close(rotated.corners[2].u, 0.75f, "BL U should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;
	if (!expect_close(rotated.corners[2].v, 1.0f, "BL V should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;
	if (!expect_close(rotated.corners[3].u, 0.75f, "BR U should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;
	if (!expect_close(rotated.corners[3].v, 0.5f, "BR V should honor FLIP_X -> FLIP_Y -> ROTATE_90 ordering")) return 1;

	const opennova::TilUvQuad clamped =
	    opennova::til_build_entry_uv_quad(255, opennova::TIL_FLAG_FLIP_X, 128, 64);
	if (!expect(clamped.valid, "out-of-range tile indices should still return a fallback UV quad")) return 1;
	if (!expect_close(clamped.corners[0].u, 0.5f, "out-of-range tile indices should fall back to atlas tile 0")) return 1;
	if (!expect_close(clamped.corners[0].v, 0.0f, "fallback tile should start at atlas origin row")) return 1;

	const opennova::TilUvQuad invalid =
	    opennova::til_build_entry_uv_quad(0, 0, 63, 64);
	if (!expect(!invalid.valid, "atlas dimensions smaller than one tile should be rejected")) return 1;

	std::printf("OK: shared tile atlas helper matches engine UV ordering and fallback rules\n");
	return 0;
}
