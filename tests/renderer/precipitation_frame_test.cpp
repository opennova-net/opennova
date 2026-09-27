// Pins the witnessed precipitation streak compile
// [orig: Render_WeatherTrailParticles @ 0x5dee10].

#include <runtime/renderer/precipitation_frame.h>

#include <cmath>
#include <cstdio>

namespace {
namespace r = opennova::renderer;
namespace env = opennova::env;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

bool near(float actual, float expected, float epsilon = 0.0005f) {
	return std::fabs(actual - expected) <= epsilon;
}

env::PrecipitationField two_drop_field() {
	env::PrecipitationField field;
	// Slot 0: a drop 10 m east of the camera at 2 m up, floor at 0.
	field.slots[0] = {10 << 16, 0, 2 << 16, 0};
	// Slot 1: a drop that fell through its floor — never drawn.
	field.slots[1] = {0, 0, -1 << 16, 0};
	return field;
}

void test_rain_gate_and_first_frame() {
	env::PrecipitationField field = two_drop_field();
	r::PrecipitationDrawState state;
	r::PrecipitationDrawFrame frame;
	r::PrecipitationCamera camera;
	// Rain 48 (the gate) compiles nothing; 100 % walks every active slot.
	r::compile_precipitation_frame(field, 48, 0, 0x102030u, camera, state, frame);
	CHECK(frame.drops == 0 && frame.vertices.empty());
	CHECK(frame.color_argb == 0xFF102030u);
	r::compile_precipitation_frame(field, 0x10000, 0, 0x102030u, camera, state, frame);
	// 3072 active, only slot 0 above its floor (the rest sit at z 0 == floor 0
	// and so DO draw: z >= floor). Slot 1 (z < floor) is skipped.
	CHECK(frame.drops == 3071);
	CHECK(!frame.snow);
	// Slot 0 in the render frame: (10, 2, 0); dist 10.198; width 2.0198,
	// length 1.5099; trail = (0, 0.1, 0) (no camera motion, no fall) -> head
	// at pos + (0, 0.15099, 0); right = (0.01, 0, 0) x width.
	const float *v = frame.vertices.data();
	CHECK(near(v[0], 10.0f) && near(v[1], 2.0f + 0.1f * 1.50990f, 0.001f) && near(v[2], 0.0f));
	CHECK(near(v[3], 0.5f) && near(v[4], 0.0f));
	CHECK(near(v[5], 10.0f - 0.01f * 2.01980f, 0.001f) && near(v[6], 2.0f) && near(v[8], 0.0f) && near(v[9], 1.0f));
	CHECK(near(v[10], 10.0f + 0.01f * 2.01980f, 0.001f) && near(v[13], 1.0f) && near(v[14], 1.0f));
}

void test_rain_velocity_and_fall_terms() {
	env::PrecipitationField field = two_drop_field();
	r::PrecipitationDrawState state;
	r::PrecipitationDrawFrame frame;
	r::PrecipitationCamera camera;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, camera, state, frame);
	// Move the camera 1 m along mission +y (render -z): the velocity clamps
	// to 0.2; the fall accumulator (2 x 12288 = 0.375 m) clamps to 0.1.
	camera.position_q16[1] = 1 << 16;
	field.fall_accum_z = -2 * env::PrecipitationField::kRainFallPerTick;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, camera, state, frame);
	CHECK(field.fall_accum_z == 0);
	const float *v = frame.vertices.data();
	// slot 0 pos (10, 2, 0); length = 1 + dist(10, 2, 1 -> (10,2,0)-(0,0,-1))
	const float dist = std::sqrt(10.0f * 10.0f + 2.0f * 2.0f + 1.0f * 1.0f);
	const float length = 1.0f + dist * 0.05f;
	// trail = velocity (0, 0, -0.2) - fall (0, -0.1, 0) + (0, 0.1, 0)
	CHECK(near(v[0], 10.0f));
	CHECK(near(v[1], 2.0f + (0.1f + 0.1f) * length, 0.001f));
	CHECK(near(v[2], 0.0f - 0.2f * length, 0.001f));
}

void test_snow_uses_camera_axes_without_distance_scaling() {
	env::PrecipitationField field = two_drop_field();
	r::PrecipitationDrawState state;
	r::PrecipitationDrawFrame frame;
	r::PrecipitationCamera camera;
	camera.right[0] = 0.0f;
	camera.right[2] = 1.0f;
	camera.up[0] = 0.0f;
	camera.up[1] = 1.0f;
	field.fall_accum_z = -4096;
	r::compile_precipitation_frame(field, 0x8000, 1, 0xFFFFFFu, camera, state, frame);
	CHECK(frame.snow);
	CHECK(frame.drops == env::PrecipitationField::active_count(0x8000) - 1);
	CHECK(field.fall_accum_z == 0);
	const float *v = frame.vertices.data();
	// head = pos + up x 0.05, sides = pos -/+ right x 0.025 — unit scales.
	CHECK(near(v[0], 10.0f) && near(v[1], 2.05f) && near(v[2], 0.0f));
	CHECK(near(v[5], 10.0f) && near(v[6], 2.0f) && near(v[7], -0.025f));
	CHECK(near(v[10], 10.0f) && near(v[11], 2.0f) && near(v[12], 0.025f));
}

// The weapon Inset pass calls the drawer again at its own camera, after the
// main scene's call of the same frame, over the one memory
// (runtime/renderer/scene_overlay.h kInsetOverlayOrder): the Inset's
// velocity is its eye's offset from the main call's, the main call already
// zeroed the fall, and the next main call's velocity starts from the Inset
// eye [orig: Render_WeatherTrailParticles @ 0x5dee74..0x5deed8 (the camera
// memory), @ 0x5deede..0x5deef4 (the fall read and zeroing)].
void test_the_inset_pass_call_shares_the_drawer_memory() {
	env::PrecipitationField field = two_drop_field();
	r::PrecipitationDrawState state;
	r::PrecipitationDrawFrame frame;
	r::PrecipitationCamera main_camera;
	r::PrecipitationCamera inset_camera;
	// The Inset eye 1/16 m east of the main eye (mission +x = render +x).
	inset_camera.position_q16[0] = 0x1000;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, main_camera, state, frame);
	// The frame: the main call consumes the fall ...
	field.fall_accum_z = -2 * env::PrecipitationField::kRainFallPerTick;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, main_camera, state, frame);
	CHECK(field.fall_accum_z == 0);
	// ... then the Inset's: velocity (1/16, 0, 0) from the main eye, no fall.
	r::compile_precipitation_frame(field, 0x10000, 0, 0, inset_camera, state, frame);
	const float inset_dist = std::sqrt((10.0f - 0.0625f) * (10.0f - 0.0625f) + 2.0f * 2.0f);
	const float inset_length = 1.0f + inset_dist * 0.05f;
	const float *v = frame.vertices.data();
	CHECK(near(v[0], 10.0f + 0.0625f * inset_length, 0.001f));
	CHECK(near(v[1], 2.0f + 0.1f * inset_length, 0.001f));
	CHECK(near(v[2], 0.0f));
	CHECK(state.last_camera_q16[0] == 0x1000);
	// The next main call reads the Inset's eye: velocity (-1/16, 0, 0).
	r::compile_precipitation_frame(field, 0x10000, 0, 0, main_camera, state, frame);
	const float main_length = 1.0f + std::sqrt(10.0f * 10.0f + 2.0f * 2.0f) * 0.05f;
	v = frame.vertices.data();
	CHECK(near(v[0], 10.0f - 0.0625f * main_length, 0.001f));
	CHECK(near(v[1], 2.0f + 0.1f * main_length, 0.001f));
}

// Below the rain gate the drawer returns before its memory: the next rainy
// call measures from the last rainy call's camera, and the fall it did not
// draw stays accumulated [orig: the `jle` @ 0x5dee48 ahead of
// @ 0x5dee74..0x5deef4].
void test_the_rain_gate_leaves_the_drawer_memory() {
	env::PrecipitationField field = two_drop_field();
	r::PrecipitationDrawState state;
	r::PrecipitationDrawFrame frame;
	r::PrecipitationCamera camera;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, camera, state, frame);
	// A dry call at a moved camera: nothing drawn, nothing remembered.
	camera.position_q16[0] = 0x1000;
	field.fall_accum_z = -env::PrecipitationField::kRainFallPerTick;
	camera.mode = 1;
	r::compile_precipitation_frame(field, 48, 0, 0, camera, state, frame);
	CHECK(frame.drops == 0);
	CHECK(state.last_camera_q16[0] == 0 && state.last_mode == 0);
	CHECK(field.fall_accum_z == -env::PrecipitationField::kRainFallPerTick);
	// The rain returns at mode 0: the velocity spans both moves (1/16 m east)
	// and the kept fall (12288 / 65536 m, clamped to 0.1) draws now.
	camera.mode = 0;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, camera, state, frame);
	CHECK(field.fall_accum_z == 0);
	const float length =
			1.0f + std::sqrt((10.0f - 0.0625f) * (10.0f - 0.0625f) + 2.0f * 2.0f) * 0.05f;
	const float *v = frame.vertices.data();
	CHECK(near(v[0], 10.0f + 0.0625f * length, 0.001f));
	CHECK(near(v[1], 2.0f + (0.1f + 0.1f) * length, 0.001f));
}

// The velocity is measured only while the camera mode is the last call's; a
// mode change draws no camera motion but still records the new mode and
// camera [orig: the mode test @ 0x5dee74..0x5dee7a, the records
// @ 0x5deeb4..0x5deed8].
void test_a_camera_mode_change_drops_the_velocity() {
	env::PrecipitationField field = two_drop_field();
	r::PrecipitationDrawState state;
	r::PrecipitationDrawFrame frame;
	r::PrecipitationCamera camera;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, camera, state, frame);
	// The chase camera (mode 1) 1/16 m east: no velocity this call.
	camera.mode = 1;
	camera.position_q16[0] = 0x1000;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, camera, state, frame);
	CHECK(state.last_mode == 1 && state.last_camera_q16[0] == 0x1000);
	float length =
			1.0f + std::sqrt((10.0f - 0.0625f) * (10.0f - 0.0625f) + 2.0f * 2.0f) * 0.05f;
	const float *v = frame.vertices.data();
	CHECK(near(v[0], 10.0f, 0.001f));
	CHECK(near(v[1], 2.0f + 0.1f * length, 0.001f));
	// The same mode again, another 1/16 m east: the velocity resumes.
	camera.position_q16[0] = 0x2000;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, camera, state, frame);
	length = 1.0f + std::sqrt((10.0f - 0.125f) * (10.0f - 0.125f) + 2.0f * 2.0f) * 0.05f;
	v = frame.vertices.data();
	CHECK(near(v[0], 10.0f + 0.0625f * length, 0.001f));
	// The retail memory starts zeroed: the first call at mode 0 measures from
	// the origin (clamped to the 0.2 velocity).
	r::PrecipitationDrawState fresh;
	r::PrecipitationCamera far;
	far.position_q16[1] = 4 << 16;
	r::compile_precipitation_frame(field, 0x10000, 0, 0, far, fresh, frame);
	const float far_length =
			1.0f + std::sqrt(10.0f * 10.0f + 2.0f * 2.0f + 4.0f * 4.0f) * 0.05f;
	v = frame.vertices.data();
	CHECK(near(v[2], -0.2f * far_length, 0.001f));
}

} // namespace

int main() {
	test_rain_gate_and_first_frame();
	test_rain_velocity_and_fall_terms();
	test_snow_uses_camera_axes_without_distance_scaling();
	test_the_inset_pass_call_shares_the_drawer_memory();
	test_the_rain_gate_leaves_the_drawer_memory();
	test_a_camera_mode_change_drops_the_velocity();
	std::printf(failures ? "PRECIPITATION FRAME TEST FAILED (%d)\n"
	                     : "precipitation frame test passed\n",
	            failures);
	return failures ? 1 : 0;
}
