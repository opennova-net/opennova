// The terrain leg of the dynamic light pool: the 0.66 factor, the 0.5 that
// exists because of the stage-0 doubling, the per-channel factor's default and
// the two projected-texture scales.
// [orig: Light_SetupTerrainProjectedPass @0x5AA830 (ex render_foliage_instance
//  — the arg is a Light_InstanceTable slot @0x5AA857); the literals @0x7D3E68 /
//  @0x7C3618; the scale @0x5AA864..0x5AA873]

#include <renderer/light_terrain_pass.h>

#include <cmath>
#include <cstdio>

using namespace opennova::renderer;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

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

	// A brighter terrain colour scales above 1 — it is a factor, not a clamp.
	const auto bright = terrain_per_channel_factor(0xFFFFFFu);
	CHECK(bright[0] > 1.0f, "a white terrain colour exceeds unity");
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

} // namespace

int main() {
	test_the_half_is_undone_by_modulate2x();
	test_published_scales_with_inputs();
	test_per_channel_factor_default_is_unity();
	test_ambient_composition();
	test_projection_scale();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("light_terrain_pass_test OK\n");
	return 0;
}
