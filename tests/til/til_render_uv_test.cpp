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
	const opennova::TilUvQuad base = opennova::til_build_entry_render_uv_quad(0, 0, 128, 64);
	if (!expect(base.valid, "valid atlas should produce render UVs")) return 1;
	if (!expect_close(base.corners[0].u, 0.5f / 128.0f, "TL U should include positive half-texel shift")) return 1;
	if (!expect_close(base.corners[0].v, 0.5f / 64.0f, "TL V should include positive half-texel shift")) return 1;
	if (!expect_close(base.corners[3].u, 0.5f + 0.5f / 128.0f, "BR U should keep the same render-axis shift")) return 1;
	if (!expect_close(base.corners[3].v, 1.0f + 0.5f / 64.0f, "BR V should keep the same render-axis shift")) return 1;

	const opennova::TilUvQuad flipped =
	    opennova::til_build_entry_render_uv_quad(1, opennova::TIL_FLAG_FLIP_X, 128, 64);
	if (!expect(flipped.valid, "valid atlas should produce flipped render UVs")) return 1;
	if (!expect_close(flipped.corners[0].u, 1.0f - 0.5f / 128.0f, "FLIP_X should reverse the half-texel U direction")) return 1;
	if (!expect_close(flipped.corners[1].u, 0.5f - 0.5f / 128.0f, "FLIP_X right corner should shift inward in reversed direction")) return 1;
	if (!expect_close(flipped.corners[0].v, 0.5f / 64.0f, "FLIP_X should not reverse V half-texel direction")) return 1;

	// Retail rotate is the CCW corner cycle NW<-NE, NE<-SE, SE<-SW, SW<-NW:
	// TL local (0,0) -> (1,0) [orig: render_water_quad @ 0x6047d4..0x604806].
	const opennova::TilUvQuad rotated =
	    opennova::til_build_entry_render_uv_quad(0, opennova::TIL_FLAG_ROTATE_90, 128, 64);
	if (!expect(rotated.valid, "valid atlas should produce rotated render UVs")) return 1;
	if (!expect_close(rotated.corners[0].u, 0.5f - 0.5f / 128.0f, "ROTATE_90 TL U should land on the cell right edge, biased inward")) return 1;
	if (!expect_close(rotated.corners[0].v, 0.5f / 64.0f, "ROTATE_90 TL V should stay on the cell top edge, biased positive")) return 1;

	std::printf("OK: tile render UVs include original half-texel correction\n");
	return 0;
}
