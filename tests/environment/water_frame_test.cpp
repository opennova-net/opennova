// The water pass orchestration (engine/runtime/environment/water_frame.h):
// which of a scene's water passes run from the eye's side and g_WaterActive
// (the beauty strip, the bloom's nightvision redraw, the mirror prerender),
// the weapon Inset pass's own, the per-side pass fog end, and the noise
// pair's frame counter. RE record: docs/env/env-tod-re.md (env #29, #30).
#include <runtime/environment/environment_state.h>
#include <runtime/environment/water_frame.h>
#include <formats/env/env.h>
#include <formats/env/env_weather.h>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

bool near(float actual, float expected, float epsilon = 1.0e-4f) {
	return std::fabs(actual - expected) <= epsilon;
}

using namespace opennova::env;

void test_main_scene_passes() {
	// Above the plane under an active gate: the beauty strip, the redraw and
	// the mirror.
	WaterFramePasses above = water_frame_passes(12.0f, 7.0f, true);
	expect(above.sides.above && !above.sides.underwater, "above: the view-0 side");
	expect(above.beauty && above.night_vision && above.refresh_mirror, "above: every pass runs");
	expect(above.any(), "above: a water surface call runs");

	// Underwater: the beauty strip's underwater side, no redraw (the bloom's
	// call is the view-0 call alone).
	WaterFramePasses below = water_frame_passes(1.0f, 7.0f, true);
	expect(below.sides.underwater && !below.sides.above, "below: the underwater side");
	expect(below.beauty && !below.night_vision, "below: no nightvision redraw");
	expect(below.refresh_mirror, "below: the prerender is gated on g_WaterActive alone");

	// Exactly on the plane: neither side draws, the prerender still runs.
	WaterFramePasses on = water_frame_passes(7.0f, 7.0f, true);
	expect(!on.beauty && !on.night_vision && !on.any(), "on the plane: no surface call");
	expect(on.refresh_mirror, "on the plane: the prerender is not side-gated");

	// The gate closed: no beauty strip and no prerender, the redraw still
	// marches above the plane.
	WaterFramePasses closed = water_frame_passes(12.0f, 7.0f, false);
	expect(!closed.beauty && !closed.refresh_mirror, "closed gate: no strip, no mirror");
	expect(closed.night_vision && closed.any(), "closed gate: the redraw is ungated");
	WaterFramePasses closed_below = water_frame_passes(1.0f, 7.0f, false);
	expect(!closed_below.any() && !closed_below.refresh_mirror, "closed gate underwater: nothing");
}

void test_inset_passes() {
	// The Inset's strip runs past its own water letter; its mirror on its
	// gate alone; it has no redraw.
	WaterFramePasses lettered = inset_water_frame_passes(12.0f, 7.0f, true, true);
	expect(lettered.beauty && lettered.refresh_mirror && !lettered.night_vision,
			"inset: strip and mirror, no redraw");
	WaterFramePasses skipped = inset_water_frame_passes(12.0f, 7.0f, true, false);
	expect(!skipped.beauty && skipped.refresh_mirror, "inset: a skipping letter keeps the mirror");
	WaterFramePasses on = inset_water_frame_passes(7.0f, 7.0f, true, true);
	expect(!on.beauty && on.refresh_mirror, "inset: on the plane no strip");
	WaterFramePasses closed = inset_water_frame_passes(1.0f, 7.0f, false, true);
	expect(!closed.beauty && !closed.refresh_mirror, "inset: a closed gate draws nothing");
}

void test_pass_fog_end() {
	// No environment: both sides march the dry end at the stand-in distance.
	const WaterFrameInputs bare = build_water_frame_inputs(nullptr, 0.6f);
	expect(near(water_pass_fog_end(bare, false), 1000.0f), "bare: the dry end");
	expect(near(water_pass_fog_end(bare, true), 1000.0f), "bare: underwater takes the dry end");

	// A loaded environment: above, the smoothed distance under the overcast
	// blend; underwater, the murk visibility curve of its water_murk.
	Config cfg = make_default_config();
	cfg.fog_level = 640.0f;
	cfg.water_murk = 0.3f;
	EnvironmentState env;
	env.set_config(&cfg, true);
	const WaterFrameInputs inputs = build_water_frame_inputs(&env, 0.6f);
	expect(inputs.env_loaded, "loaded: the inputs read the environment");
	expect(near(water_pass_fog_end(inputs, false),
				   fog_end_above_water(env.fog_level(), env.overcast_blend())),
			"loaded: the above end is the overcast-attenuated distance");
	expect(near(water_pass_fog_end(inputs, true), fog_end_underwater(0.3f)),
			"loaded: the underwater end is the murk curve");
	expect(!near(water_pass_fog_end(inputs, true), water_pass_fog_end(inputs, false), 1.0f),
			"loaded: the two sides differ");
}

void test_noise_clock() {
	// Unfed, the clock counts rendered water frames; each new count
	// regenerates once.
	WaterNoiseClock clock;
	expect(clock.take_regeneration(), "the first call generates at 0");
	expect(!clock.take_regeneration(), "a second call at the same count reads the pair");
	clock.advance_rendered_frame();
	clock.advance_rendered_frame();
	expect(clock.counter() == 2, "an unfed clock counts its render frames");
	expect(clock.take_regeneration(), "a new count regenerates");

	// Fed, it holds the world's entity-update count across render frames.
	clock.feed(40);
	clock.advance_rendered_frame();
	clock.advance_rendered_frame();
	expect(clock.counter() == 40, "a held entity update keeps the count");
	expect(clock.take_regeneration(), "the fed count regenerates once");
	expect(!clock.take_regeneration(), "the Inset's call reads the same pair");
	clock.feed(41);
	clock.advance_rendered_frame();
	expect(clock.counter() == 41, "the next entity update advances the pair");
	expect(clock.take_regeneration(), "the new count regenerates");

	// Tables made again: the same count regenerates.
	clock.invalidate();
	expect(clock.take_regeneration(), "an invalidated pair regenerates at the same count");
}

} // namespace

int main() {
	test_main_scene_passes();
	test_inset_passes();
	test_pass_fog_end();
	test_noise_clock();
	if (failures != 0) {
		std::fprintf(stderr, "water_frame: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("water_frame: ok\n");
	return 0;
}
