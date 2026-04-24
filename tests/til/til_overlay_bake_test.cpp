#include <til/til.h>
#include <til/til_overlay_bake.h>

#include <cstdio>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void set_pixel(std::vector<uint8_t> &rgba, int width, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	uint8_t *p = rgba.data() + 4 * (y * width + x);
	p[0] = r;
	p[1] = g;
	p[2] = b;
	p[3] = a;
}

const uint8_t *pixel(const std::vector<uint8_t> &rgba, int width, int x, int y) {
	return rgba.data() + 4 * (y * width + x);
}

} // namespace

int main() {
	const int atlas_w = 64;
	const int atlas_h = 64;
	std::vector<uint8_t> atlas(static_cast<size_t>(atlas_w) * atlas_h * 4u, 0u);
	for (int y = 0; y < atlas_h; ++y) {
		for (int x = 0; x < atlas_w; ++x) {
			if (x < atlas_w / 2) {
				set_pixel(atlas, atlas_w, x, y, 255, 0, 0, 255);
			} else {
				set_pixel(atlas, atlas_w, x, y, 0, 255, 0, 255);
			}
		}
	}

	opennova::TilFile til;
	til.entries.push_back(opennova::make_til_overlay_entry(0, 0, 0, opennova::TIL_FLAG_FLIP_X));

	std::vector<uint8_t> overlay;
	if (!expect(opennova::til_bake_overlay_rgba(til, atlas.data(), atlas_w, atlas_h, 32, 32, overlay),
	            "overlay bake should succeed")) return 1;
	if (!expect(overlay.size() == 32u * 32u * 4u, "overlay size should match requested RGBA dimensions")) return 1;

	// World z 0..16 maps to overlay rows 31..16 because terrain UVs use -Z.
	const uint8_t *left_edge = pixel(overlay, 32, 0, 31);
	const uint8_t *right_edge = pixel(overlay, 32, 15, 31);
	if (!expect(left_edge[1] > left_edge[0], "FLIP_X should place the source right half on the destination left edge")) return 1;
	if (!expect(right_edge[0] > right_edge[1], "FLIP_X should place the source left half on the destination right edge")) return 1;
	if (!expect(left_edge[3] == 255 && right_edge[3] == 255, "opaque atlas pixels should bake as opaque overlay pixels")) return 1;

	const uint8_t *outside = pixel(overlay, 32, 16, 31);
	if (!expect(outside[3] == 0, "pixels outside the 16x16 tile cell should remain transparent")) return 1;

	std::printf("OK: tile overlay bake follows fixed placement, -Z mapping, UV flags, and alpha\n");
	return 0;
}
