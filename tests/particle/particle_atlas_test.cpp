// Atlas pack parity — exercises bake_atlas_layout. Engine reference:
// CParticleManager_BuildTextureAtlases @ 0x5e8db0 packs per-graphic textures
// and writes the runtime UV rect pointer array at graphic+724. The portable
// implementation uses a horizontal shelf pack (layers laid left-to-right;
// atlas height = max layer height).

#include <particle/particle.h>

#include <array>
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

bool nearly_equal(float a, float b, float tol = 0.001f) {
	return std::fabs(a - b) <= tol;
}

bool test_single_layer_atlas_full_uv_range() {
	// 1 layer present at 64×64 with frames=1 → atlas 64×64; layer region
	// covers the full UV space (0..1)×(0..1) since it is the only layer.
	using namespace opennova::particle;
	ParticleDef def;
	def.graphics[0].present = true;
	def.graphics[0].flip_frames = 1;

	std::array<AtlasInputSize, 4> sizes{};
	sizes[0] = {64, 64};

	const AtlasLayout layout = bake_atlas_layout(def, sizes);

	if (!expect(layout.atlas_width == 64, "single 64-wide layer → atlas 64w")) return false;
	if (!expect(layout.atlas_height == 64, "single 64-tall layer → atlas 64h")) return false;
	if (!expect(layout.layer_x_offset[0] == 0, "first layer x_offset 0")) return false;

	if (!expect(def.graphics[0].baked_uv_rects.size() == 1, "frames=1 yields 1 rect")) return false;
	const UvRect &rect = def.graphics[0].baked_uv_rects[0];
	if (!expect(nearly_equal(rect.u_min, 0.0f) && nearly_equal(rect.u_max, 1.0f),
			"single layer u spans (0..1)")) {
		std::fprintf(stderr, "  got u=(%f, %f)\n", rect.u_min, rect.u_max);
		return false;
	}
	if (!expect(nearly_equal(rect.v_min, 0.0f) && nearly_equal(rect.v_max, 1.0f),
			"single layer v spans (0..1)")) return false;
	return true;
}

bool test_two_layer_atlas_horizontal_pack() {
	// 2 layers present, both 64×64, frames=1 → atlas 128×64.
	// Layer 0 region (0..0.5)×(0..1); layer 1 region (0.5..1)×(0..1).
	using namespace opennova::particle;
	ParticleDef def;
	def.graphics[0].present = true;
	def.graphics[0].flip_frames = 1;
	def.graphics[1].present = true;
	def.graphics[1].flip_frames = 1;

	std::array<AtlasInputSize, 4> sizes{};
	sizes[0] = {64, 64};
	sizes[1] = {64, 64};

	const AtlasLayout layout = bake_atlas_layout(def, sizes);

	if (!expect(layout.atlas_width == 128, "2×64 layers → 128w atlas")) return false;
	if (!expect(layout.atlas_height == 64, "max(64, 64) = 64h atlas")) return false;
	if (!expect(layout.layer_x_offset[0] == 0, "layer 0 starts at x=0")) return false;
	if (!expect(layout.layer_x_offset[1] == 64, "layer 1 starts at x=64")) return false;

	const UvRect &r0 = def.graphics[0].baked_uv_rects[0];
	if (!expect(nearly_equal(r0.u_min, 0.0f) && nearly_equal(r0.u_max, 0.5f),
			"layer 0 in left half (u 0..0.5)")) {
		std::fprintf(stderr, "  got u=(%f, %f)\n", r0.u_min, r0.u_max);
		return false;
	}
	const UvRect &r1 = def.graphics[1].baked_uv_rects[0];
	if (!expect(nearly_equal(r1.u_min, 0.5f) && nearly_equal(r1.u_max, 1.0f),
			"layer 1 in right half (u 0.5..1)")) {
		std::fprintf(stderr, "  got u=(%f, %f)\n", r1.u_min, r1.u_max);
		return false;
	}
	if (!expect(nearly_equal(r0.v_max, 1.0f) && nearly_equal(r1.v_max, 1.0f),
			"both layers v_max = 1 (equal heights)")) return false;
	return true;
}

bool test_atlas_uv_split_by_frames() {
	// 1 layer 128×64 with frames=4 → atlas 128×64, single layer fills full
	// width. Each frame splits the layer's u-band into 4 equal sub-bands.
	using namespace opennova::particle;
	ParticleDef def;
	def.graphics[0].present = true;
	def.graphics[0].flip_frames = 4;

	std::array<AtlasInputSize, 4> sizes{};
	sizes[0] = {128, 64};

	const AtlasLayout layout = bake_atlas_layout(def, sizes);
	if (!expect(layout.atlas_width == 128, "atlas 128w")) return false;
	if (!expect(layout.atlas_height == 64, "atlas 64h")) return false;

	if (!expect(def.graphics[0].baked_uv_rects.size() == 4, "frames=4 yields 4 rects")) return false;

	const UvRect &f0 = def.graphics[0].baked_uv_rects[0];
	if (!expect(nearly_equal(f0.u_min, 0.0f) && nearly_equal(f0.u_max, 0.25f),
			"frame 0 spans u (0..0.25)")) {
		std::fprintf(stderr, "  got u=(%f, %f)\n", f0.u_min, f0.u_max);
		return false;
	}
	const UvRect &f3 = def.graphics[0].baked_uv_rects[3];
	if (!expect(nearly_equal(f3.u_min, 0.75f) && nearly_equal(f3.u_max, 1.0f),
			"frame 3 spans u (0.75..1)")) {
		std::fprintf(stderr, "  got u=(%f, %f)\n", f3.u_min, f3.u_max);
		return false;
	}
	return true;
}

bool test_atlas_layout_handles_different_heights() {
	// 2 layers with different heights → atlas height = max. The shorter
	// layer's v_max is height_i / max_height instead of 1.0.
	using namespace opennova::particle;
	ParticleDef def;
	def.graphics[0].present = true;
	def.graphics[0].flip_frames = 1;
	def.graphics[1].present = true;
	def.graphics[1].flip_frames = 1;

	std::array<AtlasInputSize, 4> sizes{};
	sizes[0] = {64, 32};   // shorter
	sizes[1] = {64, 64};   // taller

	const AtlasLayout layout = bake_atlas_layout(def, sizes);
	if (!expect(layout.atlas_width == 128, "sum of widths = 128")) return false;
	if (!expect(layout.atlas_height == 64, "max(32, 64) = 64")) return false;

	const UvRect &r0 = def.graphics[0].baked_uv_rects[0];
	if (!expect(nearly_equal(r0.v_max, 0.5f), "shorter layer v_max = 32/64 = 0.5")) {
		std::fprintf(stderr, "  got v_max=%f\n", r0.v_max);
		return false;
	}
	const UvRect &r1 = def.graphics[1].baked_uv_rects[0];
	if (!expect(nearly_equal(r1.v_max, 1.0f), "taller layer v_max = 64/64 = 1.0")) {
		std::fprintf(stderr, "  got v_max=%f\n", r1.v_max);
		return false;
	}
	return true;
}

bool test_absent_layer_keeps_horizontal_strip_default() {
	// Layers with size {0, 0} are treated as absent. Their baked_uv_rects
	// reset to the horizontal-strip default (full 0..1 u-range split by
	// flip_frames) so the renderer's fallback path still works.
	using namespace opennova::particle;
	ParticleDef def;
	def.graphics[0].present = true;
	def.graphics[0].flip_frames = 1;
	def.graphics[1].present = true;
	def.graphics[1].flip_frames = 2;

	std::array<AtlasInputSize, 4> sizes{};
	sizes[0] = {64, 64};
	// sizes[1] left at {0, 0} — absent

	const AtlasLayout layout = bake_atlas_layout(def, sizes);
	if (!expect(layout.atlas_width == 64, "atlas only includes layer 0")) return false;

	// Layer 1 should fall back to horizontal strip in (0..1).
	const UvRect &r1_f0 = def.graphics[1].baked_uv_rects[0];
	if (!expect(nearly_equal(r1_f0.u_min, 0.0f) && nearly_equal(r1_f0.u_max, 0.5f),
			"absent layer frame 0 falls back to (0..0.5)")) return false;
	const UvRect &r1_f1 = def.graphics[1].baked_uv_rects[1];
	if (!expect(nearly_equal(r1_f1.u_min, 0.5f) && nearly_equal(r1_f1.u_max, 1.0f),
			"absent layer frame 1 falls back to (0.5..1)")) return false;
	if (!expect(nearly_equal(r1_f0.v_max, 1.0f),
			"absent layer v_max stays 1.0")) return false;
	return true;
}

bool test_gutter_layout_offsets_content_uvs() {
	// A 1-pixel gutter surrounds each packed layer. The content x offset skips
	// the leading gutter, and UVs point at the content region, not the padded
	// duplicate pixels.
	using namespace opennova::particle;
	ParticleDef def;
	def.graphics[0].present = true;
	def.graphics[0].flip_frames = 1;
	def.graphics[1].present = true;
	def.graphics[1].flip_frames = 1;

	std::array<AtlasInputSize, 4> sizes{};
	sizes[0] = {64, 64};
	sizes[1] = {64, 64};

	const AtlasLayout layout = bake_atlas_layout(def, sizes, AtlasBakeOptions{1});

	if (!expect(layout.atlas_width == 132, "2 layers with 1px gutters -> 132w atlas")) return false;
	if (!expect(layout.atlas_height == 66, "64h layer with 1px gutters -> 66h atlas")) return false;
	if (!expect(layout.layer_x_offset[0] == 1, "layer 0 content starts after left gutter")) return false;
	if (!expect(layout.layer_x_offset[1] == 67, "layer 1 content starts after its left gutter")) return false;

	const UvRect &r0 = def.graphics[0].baked_uv_rects[0];
	if (!expect(nearly_equal(r0.u_min, 1.0f / 132.0f) &&
			nearly_equal(r0.u_max, 65.0f / 132.0f),
			"layer 0 UVs address content inside gutter")) {
		std::fprintf(stderr, "  got u=(%f, %f)\n", r0.u_min, r0.u_max);
		return false;
	}
	if (!expect(nearly_equal(r0.v_min, 1.0f / 66.0f) &&
			nearly_equal(r0.v_max, 65.0f / 66.0f),
			"layer 0 V UVs address content inside gutter")) {
		std::fprintf(stderr, "  got v=(%f, %f)\n", r0.v_min, r0.v_max);
		return false;
	}
	return true;
}

} // namespace

int main() {
	int failures = 0;
	if (!test_single_layer_atlas_full_uv_range())       ++failures;
	if (!test_two_layer_atlas_horizontal_pack())        ++failures;
	if (!test_atlas_uv_split_by_frames())               ++failures;
	if (!test_atlas_layout_handles_different_heights()) ++failures;
	if (!test_absent_layer_keeps_horizontal_strip_default()) ++failures;
	if (!test_gutter_layout_offsets_content_uvs())      ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
