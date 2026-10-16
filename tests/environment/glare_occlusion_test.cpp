// The sun-glare occlusion ray sequence (runtime/environment/glare_occlusion.h)
// [orig: Render_SkyboxSunGlow @ 0x5acd9e..0x5acf7f]: the coarse TERRAIN gate
// ray lifted by glare_coarse_start_lift to the camera's unlifted ray end, the
// two fine ENTITY rays cast only behind a clear gate, their jitters
// accumulating on the one end, and the window tick they feed. The water
// glint's leg (runtime/environment/celestial_frame.h advance_water_glint)
// [orig: Environment_UpdateSunGlare @ 0x5ad130]: the frame index stepped
// first, the terrain rays from the unjittered point, then the +-2 jitter and
// the entity ray from the camera. And the entities' line of sight both legs
// ask (world::CollisionWorld::sun_ray_clear_of_entities) [orig:
// Physics_RaycastAgainstPoolByIndex @ 0x539530, pools 2, 1, 0, the local
// player excluded].

#include <runtime/environment/celestial_frame.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/glare_occlusion.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
namespace env = opennova::env;
namespace world = opennova::world;

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

// Records every segment one line of sight is asked about and answers from a
// script (true = clear), one answer per call in order.
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

// The one ray end every glare ray of a frame starts from: camera + sun * 1024
// at the camera's own height [orig: @ 0x5acdb7..0x5acde8].
env::Vec3 ray_end(float jitter_y, float jitter_z) {
	return env::Vec3{kCam.x + kSun.x * 1024.0f, kCam.y + kSun.y * 1024.0f + jitter_y,
			kCam.z + kSun.z * 1024.0f + jitter_z};
}

void test_clear_gate_casts_the_terrain_gate_then_two_entity_rays() {
	env::GlareOcclusionState state;
	ScriptedSight terrain;
	ScriptedSight entities;
	env::advance_glare_occlusion(state, kCam, kSun, 1000.0f, terrain, entities);
	CHECK(terrain.calls.size() == 1);
	CHECK(entities.calls.size() == 2);
	if (terrain.calls.size() != 1 || entities.calls.size() != 2)
		return;
	// The coarse gate starts 1.0 above the camera (frame index 0); its end is
	// the camera's, unlifted and unjittered [orig: start.z @ 0x5acde4, end
	// @ 0x5acdcf].
	CHECK(close_to(terrain.calls[0].from, env::Vec3{kCam.x, kCam.y, kCam.z + 1.0f}));
	CHECK(close_to(terrain.calls[0].to, ray_end(0.0f, 0.0f)));
	// Sample 1 adds north +16 - 8 = +8, height -16; sample 2 adds north
	// -16 - 8 = -24, height +16 to the SAME end, so it carries both: north
	// -16, height 0 [orig: @ 0x5ace3d..0x5ace61]. Both start at the exact
	// camera [orig: start.z @ 0x5ace37].
	CHECK(close_to(entities.calls[0].from, kCam));
	CHECK(close_to(entities.calls[0].to, ray_end(8.0f, -16.0f)));
	CHECK(close_to(entities.calls[1].from, kCam));
	CHECK(close_to(entities.calls[1].to, ray_end(-16.0f, 0.0f)));
	// Both samples entered the window visible.
	CHECK(state.window == 0xC0u);
	CHECK(state.jitter_index == 2u);
}

void test_blocked_gate_skips_the_entity_rays_and_marks_both_samples_hidden() {
	env::GlareOcclusionState state;
	state.window = 0xFFu;
	ScriptedSight terrain;
	terrain.answers = {false};
	ScriptedSight entities;
	env::advance_glare_occlusion(state, kCam, kSun, 1000.0f, terrain, entities);
	CHECK(terrain.calls.size() == 1);
	CHECK(entities.calls.empty());
	CHECK(state.window == 0x3Fu);
	CHECK(state.jitter_index == 2u);
}

void test_the_fine_rays_test_entities_not_terrain() {
	// A terrain that blocks everything but the gate (its later answers false)
	// never hides the sun: the fine samples ask the entities alone.
	env::GlareOcclusionState state;
	ScriptedSight terrain;
	terrain.answers = {true, false, false};
	ScriptedSight entities;
	entities.answers = {false, true};
	env::advance_glare_occlusion(state, kCam, kSun, 1000.0f, terrain, entities);
	CHECK(terrain.calls.size() == 1);
	CHECK(state.window == 0x80u); // sample 1 hidden by an entity, sample 2 visible
}

void test_the_gate_lift_follows_the_frame_index_before_the_samples() {
	env::GlareOcclusionState state;
	ScriptedSight terrain;
	ScriptedSight entities;
	for (int frame = 0; frame < 4; ++frame)
		env::advance_glare_occlusion(state, kCam, kSun, 1000.0f, terrain, entities);
	CHECK(terrain.calls.size() == 4);
	CHECK(entities.calls.size() == 8);
	if (terrain.calls.size() != 4)
		return;
	// Frame indices 0, 2, 4, 6 before each frame: lifts 1.0, 2.0, 1.0, 2.0.
	CHECK(std::fabs(terrain.calls[0].from.z - (kCam.z + 1.0f)) < 1.0e-4f);
	CHECK(std::fabs(terrain.calls[1].from.z - (kCam.z + 2.0f)) < 1.0e-4f);
	CHECK(std::fabs(terrain.calls[2].from.z - (kCam.z + 1.0f)) < 1.0e-4f);
	CHECK(std::fabs(terrain.calls[3].from.z - (kCam.z + 2.0f)) < 1.0e-4f);
	// Each frame's jitters start again from the unjittered end: frame 1's
	// first sample (index 3: +16 + -8 north, +16 height) is base + J(3) alone.
	if (entities.calls.size() == 8)
		CHECK(close_to(entities.calls[2].to, ray_end(8.0f, 16.0f)));
}

void test_the_sequence_feeds_the_witnessed_window_tick() {
	// An entity ray blocked every other frame: the state must match the
	// window/brightness tick driven with the same two samples by hand.
	env::GlareOcclusionState state;
	env::GlareOcclusionState reference;
	for (int frame = 0; frame < 12; ++frame) {
		ScriptedSight terrain;
		ScriptedSight entities;
		const bool block_b = (frame & 1) != 0;
		entities.answers = {true, !block_b};
		env::advance_glare_occlusion(state, kCam, kSun, 1500.0f, terrain, entities);
		env::glare_occlusion_tick(reference, true, !block_b, 1500.0f);
		CHECK(state.window == reference.window);
		CHECK(state.brightness == reference.brightness);
		CHECK(state.jitter_index == reference.jitter_index);
	}
	CHECK(state.brightness > 0);
}

// --- the water glint ---------------------------------------------------------

constexpr float kWater = 10.0f;
const env::Vec3 kGlintCam{100.0f, 200.0f, 40.0f};
const env::Vec3 kGlintSun{0.0f, 0.6f, 0.8f};

void test_the_glint_steps_its_frame_index_then_rays_terrain_before_the_jitter() {
	env::Config cfg{};
	cfg.water_height_set = true;
	cfg.water_height = kWater;
	env::EnvironmentState state;
	state.set_config(&cfg, true);
	env::WaterGlintState glint; // frame_index 0: this call's index is 1
	ScriptedSight terrain;
	ScriptedSight entities;
	const env::WaterGlintFrame frame = env::advance_water_glint(state, kGlintCam, kGlintSun,
			env::Vec3{0.0f, 1.0f, 0.0f}, glint, terrain, entities);
	// Index 1: the reflected height lifted 0.25 * (1 & 3)
	// [orig: Environment_UpdateSunGlare @ 0x5ad1f2..0x5ad21a].
	env::Vec3 point;
	CHECK(env::water_glint_point(kGlintCam, kGlintSun, kWater, 0.25f, point));
	CHECK(terrain.calls.size() == 2);
	CHECK(entities.calls.size() == 1);
	if (terrain.calls.size() != 2 || entities.calls.size() != 1)
		return;
	// Both terrain rays leave the UNJITTERED point: to camera + sun * 2048,
	// then to the camera [orig: Environment_UpdateSunGlare, the calls @ 0x5ad234 / @ 0x5ad24e].
	const env::Vec3 sun_far{kGlintCam.x + kGlintSun.x * 2048.0f,
			kGlintCam.y + kGlintSun.y * 2048.0f, kGlintCam.z + kGlintSun.z * 2048.0f};
	CHECK(close_to(terrain.calls[0].from, point));
	CHECK(close_to(terrain.calls[0].to, sun_far));
	CHECK(close_to(terrain.calls[1].from, point));
	CHECK(close_to(terrain.calls[1].to, kGlintCam));
	// Then the point moves +2 in x (bit 0 set) and -2 in z (bit 1 clear), and
	// the entity ray runs from the camera to it [orig: @ 0x5ad26a..0x5ad296].
	const env::Vec3 jittered{point.x + 2.0f, point.y, point.z - 2.0f};
	CHECK(close_to(entities.calls[0].from, kGlintCam));
	CHECK(close_to(entities.calls[0].to, jittered));
	CHECK(close_to(frame.point_m, jittered));
	CHECK(frame.visible && glint.frame_index == 1u && glint.window == 0x8u);
}

void test_the_glint_skips_the_entity_ray_behind_terrain_and_hides_behind_an_entity() {
	env::Config cfg{};
	cfg.water_height_set = true;
	cfg.water_height = kWater;
	env::EnvironmentState state;
	state.set_config(&cfg, true);
	env::WaterGlintState glint;
	ScriptedSight terrain;
	terrain.answers = {true, false}; // the point does not see the camera
	ScriptedSight entities;
	env::WaterGlintFrame frame = env::advance_water_glint(state, kGlintCam, kGlintSun,
			env::Vec3{0.0f, 1.0f, 0.0f}, glint, terrain, entities);
	CHECK(!frame.visible && entities.calls.empty() && glint.window == 0u);
	ScriptedSight open_terrain;
	ScriptedSight blocking;
	blocking.answers = {false};
	frame = env::advance_water_glint(state, kGlintCam, kGlintSun, env::Vec3{0.0f, 1.0f, 0.0f},
			glint, open_terrain, blocking);
	CHECK(!frame.visible && blocking.calls.size() == 1 && glint.frame_index == 2u);
}

// --- the entities' line of sight ---------------------------------------------

world::Entity person_at(float x, int team) {
	world::Entity person;
	person.kind = world::EntityKind::Organic;
	person.item_id = 1;
	person.item_type = 3;
	person.has_item_def = true;
	person.bound_radius = 1;
	person.position = {x, 0, 5};
	person.team = static_cast<uint8_t>(team);
	return person;
}

void test_the_entity_walk_skips_the_local_player_and_its_near_teammates() {
	auto heap = std::make_unique<world::World>();
	world::World &w = *heap;
	for (int pool = 0; pool < 3; ++pool)
		w.registry.configure_pool(pool, 4);
	world::CollisionWorld collision;
	const world::FixedVec3 from{0, 0, 5 * 65536};
	const world::FixedVec3 to{20 * 65536, 0, 5 * 65536};
	CHECK(collision.sun_ray_clear_of_entities(w, from, to));
	// The local player on the ray never blocks its own sun [orig: ctx[17] =
	// g_LocalPlayerEntity @ 0x5acea7; Physics_RaycastAgainstEntityPool @0x538720 (the skip @0x538836)].
	w.cached.local_player = w.registry.spawn(0, person_at(10, 1));
	CHECK(collision.sun_ray_clear_of_entities(w, from, to));
	// A teammate within 3 u of the eye is passed; one past it blocks
	// [orig: Physics_RaycastAgainstEntityPool @ 0x5389B1..0x5389FB].
	const world::EntityHandle mate = w.registry.spawn(0, person_at(2, 1));
	CHECK(collision.sun_ray_clear_of_entities(w, from, to));
	w.registry.get(mate)->position.x = 5;
	CHECK(!collision.sun_ray_clear_of_entities(w, from, to));
	w.registry.get(mate)->position.x = 2;
	// Another team's person blocks at any distance; a NoTarget row too (the
	// walker's include arg 1, `push 1` @ 0x5ace9b).
	const world::EntityHandle enemy = w.registry.spawn(0, person_at(2, 2));
	CHECK(!collision.sun_ray_clear_of_entities(w, from, to));
	w.registry.get(enemy)->engine_flags |= world::kEntityFlagNoTarget;
	CHECK(!collision.sun_ray_clear_of_entities(w, from, to));
	// Off the ray it does not.
	w.registry.get(enemy)->position.y = 30;
	CHECK(collision.sun_ray_clear_of_entities(w, from, to));
	// A segment under 16 raw is clear whatever stands on it [orig:
	// Physics_RaycastIntContext @ 0x5385E0 returns 1].
	w.registry.get(enemy)->position = {0, 0, 5};
	CHECK(collision.sun_ray_clear_of_entities(w, from, world::FixedVec3{8, 0, 5 * 65536}));
}

} // namespace

int main() {
	test_clear_gate_casts_the_terrain_gate_then_two_entity_rays();
	test_blocked_gate_skips_the_entity_rays_and_marks_both_samples_hidden();
	test_the_fine_rays_test_entities_not_terrain();
	test_the_gate_lift_follows_the_frame_index_before_the_samples();
	test_the_sequence_feeds_the_witnessed_window_tick();
	test_the_glint_steps_its_frame_index_then_rays_terrain_before_the_jitter();
	test_the_glint_skips_the_entity_ray_behind_terrain_and_hides_behind_an_entity();
	test_the_entity_walk_skips_the_local_player_and_its_near_teammates();
	std::printf(failures ? "GLARE OCCLUSION TEST FAILED (%d)\n" : "glare occlusion test passed\n",
	            failures);
	return failures ? 1 : 0;
}
