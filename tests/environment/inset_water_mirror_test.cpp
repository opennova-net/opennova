// The weapon Inset pass's water: the water pass latch both scenes share, the Inset's own mirror
// source, the mirror image the bloom's water redraw samples, and the stride the Inset's mirror
// particles draw in (environment/water_frame.h, environment/water_mirror.h,
// renderer/particle_density.h carry the witnesses).
//
// The oracles are retail's order and arithmetic: a scene's water pass stores its collect's
// g_BlinkWaterVisible only past its water letter [orig: Terrain_RenderWorldScene, the label
// @0x5c93b8..0x5c93cd; Terrain_RenderWaterPass, the stores @0x610666 / @0x610671]; the Inset's
// prerender mirrors the view block at aspect 1.0 [orig: Render_MainScene @0x5c125c] with the
// mirror's z' = 2 wh - z, pitch and roll negated [orig: @0x5c1376..0x5c139c]; the bloom runs
// after the Inset pass [orig: Render_ProcessMainSceneFrame, the calls @0x5ca949 then @0x5caa97];
// the Inset's prerender draws ahead of its own density set [orig: Render_WeaponInsetScene, the
// calls @0x5c99b7 then @0x5c9dd9].
#include <runtime/environment/water_frame.h>
#include <runtime/environment/water_mirror.h>
#include <runtime/renderer/particle_density.h>
#include <runtime/terrain/quadtree.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

using namespace opennova;

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

bool near(float a, float b) {
	return std::fabs(a - b) < 1e-5f;
}

// The frame's order: the main scene's g_WaterActive reads the latch the last frame left, the
// main collect's water pass stores into it, the Inset's g_WaterActive reads that store, then
// the Inset collect's water pass stores its own for the next frame.
void test_the_latch_is_one_word_for_both_scenes() {
	bool latch = false;
	// Frame 1: the main collect sees the water (the letter clear), the Inset's does not.
	const bool main_reads_1 = latch;
	latch = env::water_pass_latch(latch, true, true);
	const bool inset_reads_1 = latch;
	latch = env::water_pass_latch(latch, true, false);
	CHECK(!main_reads_1);
	CHECK(inset_reads_1);
	// Frame 2: the main scene reads the Inset collect's verdict, not its own last one.
	const bool main_reads_2 = latch;
	CHECK(!main_reads_2);
	// A scene whose water letter skips its passes leaves the latch as it was.
	latch = env::water_pass_latch(true, false, false);
	CHECK(latch);
	latch = env::water_pass_latch(false, false, true);
	CHECK(!latch);
	// The latch forces g_WaterActive with every visible sector above the water
	// [orig: Terrain_SetupViewAndLighting, the test @0x60ff20, the store @0x60ff33].
	CHECK(!water_pass_active(true, 10.0f, 20.0f, 5.0f, false));
	CHECK(water_pass_active(true, 10.0f, 20.0f, 5.0f, true));
}

void test_the_inset_mirror_reflects_its_view_block_square() {
	env::MirrorSourceView block;
	block.basis_x = {1.0f, 0.0f, 0.0f};
	block.basis_y = {0.0f, 0.8f, 0.6f};
	block.basis_z = {0.0f, -0.6f, 0.8f};
	block.origin = {12.0f, 9.0f, -4.0f};
	// The Inset's fov, across the width; the source's own aspect and offsets do not carry.
	block.fov_deg = 9.5f;
	block.keep_aspect_height = true;
	block.aspect = 16.0f / 9.0f;
	block.frustum_offset_y = 0.25f;
	block.v_offset = 0.5f;
	const env::MirrorSourceView source = env::inset_mirror_source(block);
	CHECK(near(source.aspect, 1.0f));
	CHECK(near(env::kInsetMirrorAspect, 1.0f));
	CHECK(!source.keep_aspect_height);
	CHECK(source.projection == env::MirrorProjection::kPerspective);
	CHECK(near(source.fov_deg, 9.5f));
	const env::WaterMirrorView view = env::build_water_mirror_view(source, 3.0f);
	CHECK(!view.below_water);
	// z' = 2 wh - z in mission space is y' = 2 wh - y here.
	CHECK(near(view.origin.x, 12.0f));
	CHECK(near(view.origin.y, 2.0f * 3.0f - 9.0f));
	CHECK(near(view.origin.z, -4.0f));
	CHECK(near(view.aspect, 1.0f));
	CHECK(near(view.fov_deg, 9.5f));
	CHECK(near(view.v_offset, 0.0f));
	CHECK(near(view.frustum_offset_y, 0.0f));
	// The proper mirror: the forward column's vertical flips, the up column's horizontals flip.
	CHECK(near(view.basis_z.y, 0.6f));
	CHECK(near(view.basis_y.z, -0.6f));
	// Below the plane the block draws unmirrored, square all the same.
	const env::WaterMirrorView below = env::build_water_mirror_view(source, 20.0f);
	CHECK(below.below_water);
	CHECK(near(below.origin.y, 9.0f));
	CHECK(near(below.aspect, 1.0f));
}

void test_the_bloom_water_samples_the_last_mirror_rendered() {
	CHECK(env::bloom_water_mirror_image(false) == env::WaterMirrorImage::kMain);
	CHECK(env::bloom_water_mirror_image(true) == env::WaterMirrorImage::kInset);
}

void test_the_inset_mirror_draws_in_the_main_scene_stride() {
	// Session density 0: the main scene's scale 0x10000 >> 2 is 0.25, a stride of 4.
	const renderer::ParticleFrameStrides strides = renderer::particle_frame_strides(0, false, 1);
	CHECK(strides.main_view == 4u);
	CHECK(strides.inset_mirror == strides.main_view);
	// The frame's mirror keeps the stride the previous frame's last pass left.
	CHECK(strides.mirror == 1u);
	const renderer::ParticleFrameStrides dense = renderer::particle_frame_strides(2, false, 4);
	CHECK(dense.inset_mirror == 1u);
	CHECK(dense.mirror == 4u);
}

} // namespace

int main() {
	test_the_latch_is_one_word_for_both_scenes();
	test_the_inset_mirror_reflects_its_view_block_square();
	test_the_bloom_water_samples_the_last_mirror_rendered();
	test_the_inset_mirror_draws_in_the_main_scene_stride();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("inset_water_mirror: ok\n");
	return 0;
}
