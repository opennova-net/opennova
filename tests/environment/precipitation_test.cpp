// The precipitation drop pool [orig: Precipitation_SeedPool @ 0x5debb0;
// Precipitation_Reset @ 0x5df3a0; Precipitation_FallTick @ 0x5de8f0;
// WeatherParticle_UpdatePositions @ 0x5dec40].

#include <runtime/environment/precipitation.h>

#include <cstdio>

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

// The PRNG_B stream [orig: PRNG_Next16_B @ 0x6130f0] from the retail
// mission-start seed [orig: 0x5ADEADA5 @ 0x52460b].
struct PrngB {
	uint32_t state = 0x5ADEADA5u;
	static uint16_t next(void *ctx) {
		PrngB *p = static_cast<PrngB *>(ctx);
		const uint32_t rol11 = (p->state << 11) | (p->state >> 21);
		uint32_t n = p->state + rol11;
		n = ((n << 4) | (n >> 28)) ^ 1u;
		p->state = n;
		return static_cast<uint16_t>(n);
	}
};

void test_seed_draws_three_words_per_slot_in_the_witnessed_scales() {
	env::PrecipitationField field;
	PrngB stream;
	field.fall_accum_z = 77;
	field.reset(&PrngB::next, &stream);
	CHECK(field.fall_accum_z == 0);
	PrngB expected;
	for (int i = 0; i < 4; ++i) {
		const uint32_t x = expected.next(&expected);
		const uint32_t y = expected.next(&expected);
		const uint32_t z = expected.next(&expected);
		// (r << 21 + 0x8000) >> 16 == r << 5 (0..32 m); z (r << 20 + 0x8000) >> 16 == r << 4
		CHECK(field.slots[static_cast<size_t>(i)].x == static_cast<int32_t>(x << 5));
		CHECK(field.slots[static_cast<size_t>(i)].y == static_cast<int32_t>(y << 5));
		CHECK(field.slots[static_cast<size_t>(i)].z == static_cast<int32_t>(z << 4));
		CHECK(field.slots[static_cast<size_t>(i)].floor_z == 0);
	}
	// Every slot lies inside the 32 x 32 x 16 m seed volume.
	for (const env::PrecipitationSlot &slot : field.slots) {
		CHECK(slot.x >= 0 && slot.x < (32 << 16));
		CHECK(slot.y >= 0 && slot.y < (32 << 16));
		CHECK(slot.z >= 0 && slot.z < (16 << 16));
	}
}

void test_active_count_rounds_and_caps() {
	CHECK(env::PrecipitationField::active_count(0) == 0);
	CHECK(env::PrecipitationField::active_count(20) == 0);   // (3072*20+0x8000)>>16 = 1 -> not > 1
	CHECK(env::PrecipitationField::active_count(48) == 2);
	CHECK(env::PrecipitationField::active_count(0x8000) == 1536);
	CHECK(env::PrecipitationField::active_count(0x10000) == 3072);
	CHECK(env::PrecipitationField::active_count(0x20000) == 3072);
}

void test_fall_tick_gates_on_48_and_uses_the_kind_rate() {
	env::PrecipitationField field;
	field.slots[7].z = 5 << 16;
	field.fall_tick(48, 0);
	CHECK(field.slots[7].z == (5 << 16) && field.fall_accum_z == 0);
	field.fall_tick(49, 0);
	CHECK(field.slots[7].z == (5 << 16) - 12288 && field.fall_accum_z == -12288);
	field.fall_tick(0x10000, 1);
	CHECK(field.slots[7].z == (5 << 16) - 12288 - 2048 && field.fall_accum_z == -12288 - 2048);
	// Every slot fell, not only the active ones.
	CHECK(field.slots[3071].z == -12288 - 2048);
}

struct FloorProbe {
	int32_t terrain = 3 << 16;
	bool hit = false;
	int32_t hit_z = 0;
	static int32_t terrain_height(void *ctx, int32_t, int32_t) {
		FloorProbe *p = static_cast<FloorProbe *>(ctx);
		return p->terrain;
	}
	static bool entity_hit(void *ctx, int32_t, int32_t, int32_t z_top, int32_t z_bottom, int32_t &hit_z) {
		FloorProbe *p = static_cast<FloorProbe *>(ctx);
		// The ray runs from floor + 200 m down to the floor.
		if (z_top - z_bottom != 0xC80000) return false;
		hit_z = p->hit_z;
		return p->hit;
	}
};

void test_update_wraps_into_the_camera_volume_and_refloors() {
	env::PrecipitationField field;
	// Camera at (100, 100, 20) m: x/y bounds [96, 128), z [12, 28).
	const int32_t cam_x = 100 << 16, cam_y = 100 << 16, cam_z = 20 << 16;
	// Slot 0 inside the volume: untouched, never re-floored.
	field.slots[0] = {110 << 16, 110 << 16, 15 << 16, 0};
	// Slot 1 one metre below the x bound: masks under the bound's high bits
	// then lifts by the span -> 95 -> 127 m.
	field.slots[1] = {95 << 16, 110 << 16, 15 << 16, 0};
	// Slot 2 fell below the z bound: 11 -> 27 m; the water plane (5 m) beats
	// the terrain (3 m) and the entity hit (6 m) beats both.
	field.slots[2] = {110 << 16, 110 << 16, 11 << 16, 0};
	FloorProbe probe;
	probe.hit = true;
	probe.hit_z = 6 << 16;
	env::PrecipitationFloorSampler sampler;
	sampler.terrain_height = &FloorProbe::terrain_height;
	sampler.entity_hit = &FloorProbe::entity_hit;
	sampler.ctx = &probe;
	field.update(cam_x, cam_y, cam_z, 0x10000, 5 << 16, sampler);
	CHECK(field.slots[0].x == (110 << 16) && field.slots[0].floor_z == 0);
	CHECK(field.slots[1].x == (127 << 16));
	CHECK(field.slots[1].floor_z == (6 << 16));
	CHECK(field.slots[2].z == (27 << 16));
	CHECK(field.slots[2].floor_z == (6 << 16));
	// Without the entity hit the water plane is the floor; without water the terrain.
	probe.hit = false;
	field.slots[3] = {95 << 16, 110 << 16, 15 << 16, 0};
	field.update(cam_x, cam_y, cam_z, 0x10000, 5 << 16, sampler);
	CHECK(field.slots[3].floor_z == (5 << 16));
	field.slots[4] = {95 << 16, 110 << 16, 15 << 16, 0};
	field.update(cam_x, cam_y, cam_z, 0x10000, 0, sampler);
	CHECK(field.slots[4].floor_z == (3 << 16));
	// Only the active prefix walks: rain 48 -> 2 slots.
	field.slots[5] = {95 << 16, 110 << 16, 15 << 16, 0};
	field.update(cam_x, cam_y, cam_z, 48, 0, sampler);
	CHECK(field.slots[5].x == (95 << 16));
	// A slot exactly 32 m past the bound wraps back onto the bound.
	field.slots[0] = {128 << 16, 110 << 16, 15 << 16, 0};
	field.update(cam_x, cam_y, cam_z, 0x10000, 0, sampler);
	CHECK(field.slots[0].x == (96 << 16));
}

} // namespace

int main() {
	test_seed_draws_three_words_per_slot_in_the_witnessed_scales();
	test_active_count_rounds_and_caps();
	test_fall_tick_gates_on_48_and_uses_the_kind_rate();
	test_update_wraps_into_the_camera_volume_and_refloors();
	std::printf(failures ? "PRECIPITATION TEST FAILED (%d)\n" : "precipitation test passed\n",
	            failures);
	return failures ? 1 : 0;
}
