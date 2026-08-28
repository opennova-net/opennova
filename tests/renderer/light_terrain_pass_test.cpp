// The terrain leg of the dynamic light pool: the 0.66 factor, the 0.5 that
// exists because of the stage-0 doubling, the per-channel factor's default,
// the two projected-texture scales, the projection contract, the procedural
// textures, and the per-patch collect + gate + constant build.
// [orig: Light_SetupTerrainProjectedPass @0x5AA830 (ex render_foliage_instance
//  — the arg is a Light_InstanceTable slot @0x5AA857); the literals @0x7D3E68 /
//  @0x7C3618; the scale @0x5AA864..0x5AA873; render_terrain_sector_batch
//  @0x6095f9..0x6098bc; Lighting_InitTextures @0x5A94F0;
//  Texture_GenerateProceduralFalloffTexture @0x5A92C0]

#include <runtime/renderer/light_terrain_pass.h>

#include <cmath>
#include <cstdio>

using namespace opennova::renderer;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

bool near(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

// THE 0.5 EXISTS BECAUSE OF STAGE-0's 2x. 0.66 * 0.5 * 2 == 0.66 — so the
// half is not an attenuation, it is a pre-division that the device undoes.
// Dropping either the 0.5 or the 2x alone changes the result by 2x.
void test_the_half_is_undone_by_modulate2x() {
	const float ambient = terrain_light_ambient(1.0f, 1.0f, 1.0f, 1.0f);
	CHECK(near(ambient, kTerrainLightFactor * kTerrainAmbientHalf),
			"the Ambient term carries 0.66 * 0.5");
	// Stage-0's 2x brings it back to the bare factor.
	CHECK(near(ambient * 2.0f, kTerrainLightFactor),
			"and stage-0's 2x restores exactly 0.66");
	// The PUBLISHED value has no half — that is the other stage's business.
	CHECK(near(terrain_light_published(1.0f, 1.0f), kTerrainLightFactor),
			"the published value is the bare factor");
	CHECK(!near(terrain_light_published(1.0f, 1.0f), ambient),
			"published and ambient are NOT the same term");
}

void test_published_scales_with_inputs() {
	CHECK(near(terrain_light_published(0.5f, 1.0f), kTerrainLightFactor * 0.5f),
			"channel scales the published value");
	CHECK(near(terrain_light_published(1.0f, 0.25f), kTerrainLightFactor * 0.25f),
			"blend scales it too");
	CHECK(near(terrain_light_published(0.0f, 1.0f), 0.0f),
			"a black light publishes nothing");
}

// THE PER-CHANNEL FACTOR's default divides to exactly (1,1,1). If it did not,
// every terrain light would be tinted by default.
void test_per_channel_factor_default_is_unity() {
	const auto f = terrain_per_channel_factor(kTerrainFactorDefaultPacked);
	CHECK(near(f[0], 1.0f) && near(f[1], 1.0f) && near(f[2], 1.0f),
			"the 0x808080 default is exactly unity");

	// A brighter recip byte scales above 1 — it is a factor, not a clamp.
	const auto bright = terrain_per_channel_factor(0xFFFFFFu);
	CHECK(bright[0] > 1.0f, "a 255 recip byte exceeds unity");
	CHECK(near(bright[0], 255.0f / 128.0f), "and is not clamped to 1");

	// Channels are independent and in RGB order: byte 2 red, byte 1 green,
	// byte 0 blue — the order the per-tick unpack writes flt_2732DAC/DA8/DA4.
	const auto tinted = terrain_per_channel_factor(0x804000u);
	CHECK(near(tinted[0], 1.0f), "red channel from the high byte");
	CHECK(near(tinted[1], 0.5f), "green from the middle");
	CHECK(near(tinted[2], 0.0f), "blue from the low byte");
}

// The Ambient term composes all four factors, so a non-unity per-channel
// factor tints it.
void test_ambient_composition() {
	const float base = terrain_light_ambient(1.0f, 1.0f, 1.0f, 1.0f);
	const float halved = terrain_light_ambient(1.0f, 1.0f, 1.0f, 0.5f);
	CHECK(near(halved, base * 0.5f), "the per-channel factor scales Ambient");

	const float scaled = terrain_light_ambient(1.0f, 1.0f, 2.0f, 1.0f);
	CHECK(near(scaled, base * 2.0f), "the ambient scale scales it too");
}

// The projected texture scales INVERSELY with radius: a bigger light projects
// a wider, not a brighter, pool. The normal pass spans one diameter (0.5/r);
// the bit-0x100 pass is the wider 0.4/r.
void test_projection_scale() {
	CHECK(near(terrain_project_scale(1.0f), 0.5f),
			"unit radius gives 0.5 — one diameter across the texture");
	CHECK(near(terrain_project_scale(2.0f), 0.25f),
			"a bigger light projects a wider pool");
	CHECK(terrain_project_scale(4.0f) < terrain_project_scale(2.0f),
			"the scale falls as radius grows");
	CHECK(terrain_project_scale(0.0f) == 0.0f, "a zero radius is inert");
	CHECK(near(terrain_project_scale(1.0f, true), 0.4f),
			"the alternate pass is the 0.4 / r form");
	CHECK(terrain_project_scale(1.0f, true) < terrain_project_scale(1.0f),
			"the two passes are NOT one scale");
	CHECK(kLightFlagNoTerrain == 0x400u, "flag 1024 keeps a light off the terrain");
}

// THE PROJECTION: the camera terms cancel, so each stage is the light-relative
// offset times inv plus 0.5, through the Y-negated mission->D3D fold.
void test_projection_uv() {
	const std::array<float, 3> light = {10.0f, 20.0f, 5.0f};
	const float inv = terrain_project_scale(8.0f); // 1/16
	// A point AT the light samples the disc's centre on both stages.
	TerrainLightUv uv = terrain_light_uv_disc(light, light, inv);
	CHECK(near(uv.u, 0.5f) && near(uv.v, 0.5f), "the light itself is the disc centre");
	uv = terrain_light_uv_height(light, light, inv);
	CHECK(near(uv.u, 0.5f) && near(uv.v, 0.5f), "and the strip's middle");
	// +r along mission x reaches the disc's far edge on v (d3d.z = mission.x);
	// a full radius is half the texture span.
	uv = terrain_light_uv_disc({18.0f, 20.0f, 5.0f}, light, inv);
	CHECK(near(uv.v, 1.0f) && near(uv.u, 0.5f), "+r on mission x -> v = 1");
	// +r along mission y runs AGAINST u (d3d.x = -mission.y): the Y-negation
	// is carried, not dropped.
	uv = terrain_light_uv_disc({10.0f, 28.0f, 5.0f}, light, inv);
	CHECK(near(uv.u, 0.0f) && near(uv.v, 0.5f), "+r on mission y -> u = 0");
	// Height does not move the disc at all...
	uv = terrain_light_uv_disc({10.0f, 20.0f, 9.0f}, light, inv);
	CHECK(near(uv.u, 0.5f) && near(uv.v, 0.5f), "height leaves the disc alone");
	// ...it is the strip's axis: +r above the light is the strip's far end.
	uv = terrain_light_uv_height({10.0f, 20.0f, 13.0f}, light, inv);
	CHECK(near(uv.u, 1.0f) && near(uv.v, 0.5f), "+r up -> strip u = 1, v stays 0.5");
	uv = terrain_light_uv_height({10.0f, 20.0f, 1.0f}, light, inv);
	CHECK(near(uv.u, 0.25f), "half a radius below -> u = 0.25");
	// The alt pass stretches the same offset to fewer texels.
	uv = terrain_light_uv_disc({18.0f, 20.0f, 5.0f}, light, terrain_project_scale(8.0f, true));
	CHECK(near(uv.v, 0.9f), "the 0.4/r pass lands +r at v = 0.9");
}

// THE 64x64 DISC (mode 2): zero border, a 254 centre (the texel grid never
// lands exactly on x = 0), radially falling, symmetric.
void test_falloff_light2d() {
	CHECK(falloff_texture_light2d_argb(0, 10) == 0u, "column 0 is the zero border");
	CHECK(falloff_texture_light2d_argb(63, 10) == 0u, "column 63 too");
	CHECK(falloff_texture_light2d_argb(10, 0) == 0u, "row 0");
	CHECK(falloff_texture_light2d_argb(10, 63) == 0u, "row 63");
	// Centre texel (32,32): x = y = 63/62 - 1 = 0.0161; exp(-4x^2)^2 * 255 =
	// 254.47 -> 254 (ftol truncates).
	CHECK(falloff_texture_light2d_argb(32, 32) == 0xFFFEFEFEu,
			"the centre texel truncates to 254, opaque");
	// Texel (1,1): x = y = -0.984; exp(-3.872)^2 * 255 = 0.11 -> 0, still opaque.
	CHECK(falloff_texture_light2d_argb(1, 1) == 0xFF000000u,
			"the interior corner is black but opaque (no border zero)");
	// Falls with distance from the centre along a row.
	const uint32_t c16 = falloff_texture_light2d_argb(16, 32) & 0xFFu;
	const uint32_t c24 = falloff_texture_light2d_argb(24, 32) & 0xFFu;
	const uint32_t c31 = falloff_texture_light2d_argb(31, 32) & 0xFFu;
	CHECK(c16 < c24 && c24 < c31, "intensity rises toward the centre");
	// Symmetric about the centre pair (31|32 straddle x = 0).
	CHECK(falloff_texture_light2d_argb(31, 32) == falloff_texture_light2d_argb(32, 32),
			"columns 31 and 32 straddle x = 0 symmetrically");
	CHECK(falloff_texture_light2d_argb(5, 20) == falloff_texture_light2d_argb(58, 43),
			"point symmetry through the centre");
	// Every texel is gray with full alpha.
	const uint32_t t = falloff_texture_light2d_argb(20, 40);
	CHECK((t >> 24) == 0xFFu && ((t >> 16) & 0xFFu) == (t & 0xFFu) &&
					((t >> 8) & 0xFFu) == (t & 0xFFu),
			"gray under alpha 255");
}

// THE 64x64 SPOT DISC (mode 1): 1 - r^2, and the ALPHA byte carries the
// intensity too (0x01010101 * i, no forced 0xFF).
void test_falloff_spot2d() {
	CHECK(falloff_texture_spot2d_argb(0, 5) == 0u, "zero border");
	// Centre: 1 - 2 * 0.0161^2 = 0.99948 * 255 = 254.87 -> 254 on all four bytes.
	CHECK(falloff_texture_spot2d_argb(32, 32) == 0xFEFEFEFEu,
			"the centre is 254 on every byte, alpha included");
	// Corner (1,1): 1 - 2 * 0.984^2 = -0.936 -> clamped 0 -> fully transparent black.
	CHECK(falloff_texture_spot2d_argb(1, 1) == 0u, "outside the unit circle clamps to 0");
	// Linear in r^2: column 16 (x = -0.5) on the centre row -> 1 - 0.25 - tiny.
	const uint32_t c16 = falloff_texture_spot2d_argb(16, 32) & 0xFFu;
	CHECK(c16 >= 190 && c16 <= 192, "x = -0.5 reads ~0.75 * 255 = 191");
}

// THE 64x8 STRIP: white end columns, gray = (col * (row + 1)) >> 1 inside.
void test_falloff_spot1d() {
	CHECK(falloff_texture_spot1d_argb(0, 4) == 0xFFFFFFFFu, "column 0 is white");
	CHECK(falloff_texture_spot1d_argb(63, 4) == 0xFFFFFFFFu, "column 63 is white");
	CHECK(falloff_texture_spot1d_argb(32, 4) == 0xFF505050u,
			"row 4 (v = 0.5), column 32 -> (32 * 5) >> 1 = 80");
	CHECK(falloff_texture_spot1d_argb(62, 7) == 0xFFF8F8F8u,
			"the last row's last interior column -> 248");
	CHECK(falloff_texture_spot1d_argb(1, 0) == 0xFF000000u,
			"row 0, column 1 -> 0 (the >> 1 truncates)");
	CHECK(kFalloffSpot1DRows == 8 && kFalloffTextureSize == 64,
			"64 columns x 8 rows [orig: @0x5A95FC/@0x5A95FE]");
}

// THE PATCH BOUNDS fold: render frame (x, y-up, z) + sector origin ->
// mission 16.16 (x, -z, y), per-axis min/max re-ordered.
void test_patch_bounds_fold() {
	const float aabb_min[3] = {0.0f, 2.0f, 0.0f};
	const float aabb_max[3] = {64.0f, 10.0f, 64.0f};
	const TerrainLightPatchBounds b = terrain_patch_light_bounds(aabb_min, aabb_max, 512.0f, 1024.0f);
	CHECK(b.aabb_min_fixed[0] == 512 * 65536 && b.aabb_max_fixed[0] == 576 * 65536,
			"x carries the sector origin straight through");
	// Render z 1024..1088 -> mission y -1088..-1024: the negation flips min/max.
	CHECK(b.aabb_min_fixed[1] == -1088 * 65536 && b.aabb_max_fixed[1] == -1024 * 65536,
			"render z folds to negated mission y with min/max re-ordered");
	CHECK(b.aabb_min_fixed[2] == 2 * 65536 && b.aabb_max_fixed[2] == 10 * 65536,
			"render height is mission z");
}

opennova::renderer::LightSpawnParams pool_light(int32_t x_fixed, int32_t y_fixed, int32_t z_fixed) {
	opennova::renderer::LightSpawnParams params;
	params.position_fixed = {x_fixed, y_fixed, z_fixed};
	params.radius_fixed = 8 << 16;
	params.rgb = {255, 255, 255};
	return params;
}

TerrainLightPatchBounds patch_around(int32_t x_fixed, int32_t y_fixed, int32_t half) {
	TerrainLightPatchBounds b;
	b.aabb_min_fixed = {x_fixed - half, y_fixed - half, -half};
	b.aabb_max_fixed = {x_fixed + half, y_fixed + half, half};
	return b;
}

// THE PER-PATCH ROWS: one row per passing light, no four-light cap, the
// sixteen-light collect, both groups cleared, the pixel constants.
void test_patch_rows() {
	opennova::renderer::LightScene scene;
	const opennova::renderer::LightHandle a = scene.spawn(pool_light(0, 0, 0));
	(void)a;
	const TerrainLightPatchBounds patch = patch_around(0, 0, 64 << 16);
	TerrainLightPassInputs inputs;
	TerrainLightPatchRows rows;
	size_t total = scene.collect_terrain_pass_rows(&patch, 1, inputs, &rows);
	CHECK(total == 1 && rows.count == 1, "one unowned light in the patch -> one row");
	if (rows.count == 1) {
		const TerrainLightRow &row = rows.rows[0];
		CHECK(near(row.position[0], 0.0f) && near(row.position[2], 0.0f),
				"the row carries the mission-space position");
		CHECK(near(row.inv_scale, 0.5f / 8.0f), "inv_scale = 0.5 / radius");
		// 255/256 * blend 1 * ambient 1 * factor 1 * 0.66 * 0.5.
		const float expected = (255.0f / 256.0f) * kTerrainLightFactor * kTerrainAmbientHalf;
		CHECK(near(row.pixel_rgb[0], expected) && near(row.pixel_rgb[1], expected) &&
						near(row.pixel_rgb[2], expected),
				"pixel constants = rgb/256 * 0.66 * 0.5");
	}
	// The alt pass and the two factor triples reach the row.
	inputs.alt_pass = true;
	inputs.ambient_scale = {2.0f, 1.0f, 1.0f};
	inputs.terrain_factor = {1.0f, 0.5f, 1.0f};
	scene.collect_terrain_pass_rows(&patch, 1, inputs, &rows);
	CHECK(rows.count == 1 && near(rows.rows[0].inv_scale, 0.4f / 8.0f),
			"the alt pass publishes 0.4 / radius");
	CHECK(near(rows.rows[0].pixel_rgb[0], rows.rows[0].pixel_rgb[2] * 2.0f),
			"the ambient scale multiplies per channel");
	CHECK(near(rows.rows[0].pixel_rgb[1], rows.rows[0].pixel_rgb[2] * 0.5f),
			"the recip factor multiplies per channel");
	inputs = TerrainLightPassInputs{};

	// A patch the light does not overlap collects nothing.
	const TerrainLightPatchBounds far = patch_around(1000 << 16, 0, 16 << 16);
	scene.collect_terrain_pass_rows(&far, 1, inputs, &rows);
	CHECK(rows.count == 0, "a non-overlapping patch collects no rows");

	// Both light groups are CLEARED for the terrain batch, so an owned light
	// never passes — unlike the object pass, where its owner's draw admits it.
	opennova::renderer::LightSpawnParams owned = pool_light(1 << 16, 0, 0);
	owned.owner_entity = 77;
	owned.owner_section = 3;
	scene.spawn(owned);
	scene.collect_terrain_pass_rows(&patch, 1, inputs, &rows);
	CHECK(rows.count == 1, "an owned light is refused by the cleared groups");
	for (size_t i = 0; i < rows.count; ++i) {
		CHECK(rows.rows[i].handle != opennova::renderer::LightHandle{}, "rows carry live handles");
	}

	// The authored terrain disable (flag 1024) is honoured; the object
	// disable is irrelevant here.
	opennova::renderer::LightSpawnParams no_terrain = pool_light(2 << 16, 0, 0);
	no_terrain.disable_terrain = true;
	scene.spawn(no_terrain);
	opennova::renderer::LightSpawnParams no_objects = pool_light(3 << 16, 0, 0);
	no_objects.disable_objects = true;
	scene.spawn(no_objects);
	scene.collect_terrain_pass_rows(&patch, 1, inputs, &rows);
	CHECK(rows.count == 2, "disable_terrain drops the row; disable_objects does not");

	// The two leg gates.
	inputs.pixel_shader_path = false;
	CHECK(scene.collect_terrain_pass_rows(&patch, 1, inputs, &rows) == 0 && rows.count == 0,
			"no pixel-shader terrain path -> no light leg");
	inputs.pixel_shader_path = true;
	inputs.light_pass_disabled = true;
	CHECK(scene.collect_terrain_pass_rows(&patch, 1, inputs, &rows) == 0,
			"the render mode that skips the loop -> no rows");
	inputs = TerrainLightPassInputs{};

	// No four-light cap: every survivor gets a re-draw. The collect itself
	// stops at SIXTEEN slot-order candidates, then sorts nearest-first.
	opennova::renderer::LightScene crowd;
	for (int i = 0; i < 20; ++i) {
		crowd.spawn(pool_light((20 - i) << 16, 0, 0)); // farthest first in slot order
	}
	TerrainLightPatchRows crowd_rows;
	const TerrainLightPatchBounds wide = patch_around(0, 0, 64 << 16);
	total = crowd.collect_terrain_pass_rows(&wide, 1, inputs, &crowd_rows);
	CHECK(total == 16 && crowd_rows.count == 16,
			"twenty overlapping lights collect the first sixteen, all drawn");
	// Slot order wins the cap: the four NEAREST (slots 16..19) were never
	// collected, so the nearest row is slot 15's x = 5.
	CHECK(near(crowd_rows.rows[0].position[0], 5.0f),
			"the cap is slot-order, not nearest-first");
	CHECK(crowd_rows.rows[0].position[0] < crowd_rows.rows[15].position[0],
			"and the collected set is sorted nearest-first");

	// Several patches in one call, each with its own count.
	const TerrainLightPatchBounds patches[2] = {patch, far};
	TerrainLightPatchRows multi[2];
	total = scene.collect_terrain_pass_rows(patches, 2, inputs, multi);
	CHECK(total == 2 && multi[0].count == 2 && multi[1].count == 0,
			"per-patch rows are independent");
}

} // namespace

int main() {
	test_the_half_is_undone_by_modulate2x();
	test_published_scales_with_inputs();
	test_per_channel_factor_default_is_unity();
	test_ambient_composition();
	test_projection_scale();
	test_projection_uv();
	test_falloff_light2d();
	test_falloff_spot2d();
	test_falloff_spot1d();
	test_patch_bounds_fold();
	test_patch_rows();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("light_terrain_pass_test OK\n");
	return 0;
}
