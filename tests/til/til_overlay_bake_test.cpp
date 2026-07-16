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

	// --- Within-tile V orientation -------------------------------------------
	// A vertically split tile (atlas top half red, bottom half green) pins
	// interpolate_uv's local_z direction. For the cell (0, 0) entry the bake
	// spans world_z in [0, 16] and out_y = wrap_floor(-world_z, 32), so the
	// tile's v_lo edge (atlas top, local_z -> 0, world_z = 0.5) lands on the
	// overlay row with the LARGER -world_z: row 31. The v_hi edge
	// (world_z = 15.5) lands on row 16. Derivation from til_overlay_bake.cpp
	// with the +half-texel render quad (v = 1/128 + local_z):
	//   world_z  0.5: local_z = 0.03125,  sy = 2.0  -> atlas row  2 (red)
	//   world_z  7.5: local_z = 0.46875,  sy = 30.0 -> atlas row 30 (red)
	//   world_z  8.5: local_z = 0.53125,  sy = 34.0 -> atlas row 34 (green)
	//   world_z 15.5: local_z = 0.96875,  sy = 62.0 -> atlas row 62 (green)
	// A vertical flip of local_z would swap the red and green rows.
	std::vector<uint8_t> v_atlas(static_cast<size_t>(atlas_w) * atlas_h * 4u, 0u);
	for (int y = 0; y < atlas_h; ++y) {
		for (int x = 0; x < atlas_w; ++x) {
			if (y < atlas_h / 2) {
				set_pixel(v_atlas, atlas_w, x, y, 255, 0, 0, 255);
			} else {
				set_pixel(v_atlas, atlas_w, x, y, 0, 255, 0, 255);
			}
		}
	}

	opennova::TilFile v_til;
	v_til.entries.push_back(opennova::make_til_overlay_entry(0, 0, 0, 0));

	std::vector<uint8_t> v_overlay;
	if (!expect(opennova::til_bake_overlay_rgba(v_til, v_atlas.data(), atlas_w, atlas_h, 32, 32, v_overlay),
	            "vertical-split overlay bake should succeed")) return 1;

	const uint8_t *v_lo_edge = pixel(v_overlay, 32, 5, 31);
	if (!expect(v_lo_edge[0] == 255 && v_lo_edge[1] == 0 && v_lo_edge[3] == 255,
	            "the tile's v_lo (atlas top) edge bakes at world_z 0.5 = overlay row 31")) return 1;
	const uint8_t *v_hi_edge = pixel(v_overlay, 32, 5, 16);
	if (!expect(v_hi_edge[1] == 255 && v_hi_edge[0] == 0 && v_hi_edge[3] == 255,
	            "the tile's v_hi (atlas bottom) edge bakes at world_z 15.5 = overlay row 16")) return 1;
	const uint8_t *above_split = pixel(v_overlay, 32, 5, 24);
	if (!expect(above_split[0] == 255 && above_split[1] == 0,
	            "world_z 7.5 (overlay row 24) still samples the red top half")) return 1;
	const uint8_t *below_split = pixel(v_overlay, 32, 5, 23);
	if (!expect(below_split[1] == 255 && below_split[0] == 0,
	            "world_z 8.5 (overlay row 23) crosses into the green bottom half")) return 1;
	const uint8_t *north_of_cell = pixel(v_overlay, 32, 5, 15);
	if (!expect(north_of_cell[3] == 0,
	            "rows beyond the cell's world_z span stay transparent")) return 1;

	std::printf("OK: tile overlay bake follows fixed placement, -Z mapping, UV flags, and alpha\n");
	return 0;
}
