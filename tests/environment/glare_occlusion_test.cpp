// The sun-glare occlusion ray sequence (runtime/environment/glare_occlusion.h)
// [orig: Render_SkyboxSunGlow @ 0x5acd9e..0x5acf7f]: the coarse gate ray
// lifted by glare_coarse_start_lift, the two jittered fine rays cast only
// behind a clear gate, and the window tick they feed.

#include <runtime/environment/glare_occlusion.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
namespace env = opennova::env;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

struct Segment {
	env::Vec3 from;
	env::Vec3 to;
};

bool close_to(const env::Vec3 &a, const env::Vec3 &b) {
	return std::fabs(a.x - b.x) < 1.0e-3f && std::fabs(a.y - b.y) < 1.0e-3f &&
			std::fabs(a.z - b.z) < 1.0e-3f;
}

// Records every segment the sequence asks about and answers from a script
// (true = clear), one answer per call in order.
struct ScriptedSight {
	std::vector<Segment> calls;
	std::vector<bool> answers;
	bool operator()(const env::Vec3 &from, const env::Vec3 &to) {
		const size_t index = calls.size();
		calls.push_back({from, to});
		return index < answers.size() ? answers[index] : true;
	}
};

const env::Vec3 kCam{100.0f, 200.0f, 50.0f};
const env::Vec3 kSun{0.6f, 0.0f, 0.8f};

env::Vec3 ray_end(const env::Vec3 &from, float jitter_y, float jitter_z) {
	return env::Vec3{from.x + kSun.x * 1024.0f, from.y + kSun.y * 1024.0f + jitter_y,
			from.z + kSun.z * 1024.0f + jitter_z};
}

void test_clear_gate_casts_the_coarse_ray_then_both_jittered_rays() {
	env::GlareOcclusionState state;
	ScriptedSight sight;
	env::advance_glare_occlusion(state, kCam, kSun, 1000.0f, sight);
	CHECK(sight.calls.size() == 3);
	if (sight.calls.size() != 3)
		return;
	// The coarse gate starts 1.0 above the camera (frame index 0) and carries
	// no jitter; the ray is 1024 world units along the sun.
	const env::Vec3 coarse_from{kCam.x, kCam.y, kCam.z + 1.0f};
	CHECK(close_to(sight.calls[0].from, coarse_from));
	CHECK(close_to(sight.calls[0].to, ray_end(coarse_from, 0.0f, 0.0f)));
	// Sample 1: north +16 - 8 = +8, height -16; sample 2: north -16 - 8 =
	// -24, height +16 (glare_ray_jitter). Both start at the exact camera.
	CHECK(close_to(sight.calls[1].from, kCam));
	CHECK(close_to(sight.calls[1].to, ray_end(kCam, 8.0f, -16.0f)));
	CHECK(close_to(sight.calls[2].from, kCam));
	CHECK(close_to(sight.calls[2].to, ray_end(kCam, -24.0f, 16.0f)));
	// Both samples entered the window visible.
	CHECK(state.window == 0xC0u);
	CHECK(state.jitter_index == 2u);
}

void test_blocked_gate_skips_the_fine_rays_and_marks_both_samples_hidden() {
	env::GlareOcclusionState state;
	state.window = 0xFFu;
	ScriptedSight sight;
	sight.answers = {false};
	env::advance_glare_occlusion(state, kCam, kSun, 1000.0f, sight);
	CHECK(sight.calls.size() == 1);
	CHECK(state.window == 0x3Fu);
	CHECK(state.jitter_index == 2u);
}

void test_the_gate_lift_follows_the_frame_index_before_the_samples() {
	env::GlareOcclusionState state;
	ScriptedSight sight;
	for (int frame = 0; frame < 4; ++frame)
		env::advance_glare_occlusion(state, kCam, kSun, 1000.0f, sight);
	CHECK(sight.calls.size() == 12);
	if (sight.calls.size() != 12)
		return;
	// Frame indices 0, 2, 4, 6 before each frame: lifts 1.0, 2.0, 1.0, 2.0.
	CHECK(std::fabs(sight.calls[0].from.z - (kCam.z + 1.0f)) < 1.0e-4f);
	CHECK(std::fabs(sight.calls[3].from.z - (kCam.z + 2.0f)) < 1.0e-4f);
	CHECK(std::fabs(sight.calls[6].from.z - (kCam.z + 1.0f)) < 1.0e-4f);
	CHECK(std::fabs(sight.calls[9].from.z - (kCam.z + 2.0f)) < 1.0e-4f);
}

void test_the_sequence_feeds_the_witnessed_window_tick() {
	// A fine ray blocked every other frame: the state must match the
	// window/brightness tick driven with the same two samples by hand.
	env::GlareOcclusionState state;
	env::GlareOcclusionState reference;
	for (int frame = 0; frame < 12; ++frame) {
		ScriptedSight sight;
		const bool block_b = (frame & 1) != 0;
		sight.answers = {true, true, !block_b};
		env::advance_glare_occlusion(state, kCam, kSun, 1500.0f, sight);
		env::glare_occlusion_tick(reference, true, !block_b, 1500.0f);
		CHECK(state.window == reference.window);
		CHECK(state.brightness == reference.brightness);
		CHECK(state.jitter_index == reference.jitter_index);
	}
	CHECK(state.brightness > 0);
}

} // namespace

int main() {
	test_clear_gate_casts_the_coarse_ray_then_both_jittered_rays();
	test_blocked_gate_skips_the_fine_rays_and_marks_both_samples_hidden();
	test_the_gate_lift_follows_the_frame_index_before_the_samples();
	test_the_sequence_feeds_the_witnessed_window_tick();
	std::printf(failures ? "GLARE OCCLUSION TEST FAILED (%d)\n" : "glare occlusion test passed\n",
	            failures);
	return failures ? 1 : 0;
}
