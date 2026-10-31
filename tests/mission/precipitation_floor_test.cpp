// The precipitation pool's terrain floor through the mission kernel (D-NET-507): a wrapped
// slot's floor is the bilinear sampler over the slot's own X / Y words, the height word it
// returns stored as is [orig: WeatherParticle_UpdatePositions @0x5DEC40 (the
// Terrain_SampleHeightBilinear call @0x5DED75, the store @0x5DED85)]. The kernel's callback
// went through the float ground_height before, whose float image of a word past 24
// significant bits (and whose float weights) are not the sampler's.
#include <runtime/mission/mission_kernel.h>
#include <runtime/terrain_query/height_field.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include "../common/title_fixture.h"

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

int main() {
	// The kernel on the heap: sizeof(MissionKernel) overflows MSVC's 1 MB stack in main.
	auto kernel = std::make_unique<mission::MissionKernel>();
	test_titles::jo_world(kernel->world);

	// A full 1024 atlas of varied heights over a 16 x 16 grid of quadrant-1 sectors.
	constexpr int kDim = 1024;
	std::vector<uint16_t> heights(static_cast<size_t>(kDim) * kDim);
	uint32_t h = 0x12345678u;
	for (uint16_t &raw : heights) {
		h = h * 1664525u + 1013904223u;
		raw = static_cast<uint16_t>(0x0400u + ((h >> 8) & 0x3FFFu));
	}
	const std::vector<int> grid(256, 1);
	kernel->terrain_store.build(heights.data(), heights.size(), grid.data(), 0, 0, 16, 16, false,
			false, terrain::CoordsQuadrantLocks{});
	CHECK(kernel->has_terrain());

	// Rain at full strength walks all 3072 slots; with no water plane and no collision
	// world the floor is the terrain sample alone.
	world::World &w = kernel->world;
	w.weather.core.scalar_channels.rain_pct_fp = 0x10000;
	w.env.water_z = 0;
	auto &slots = w.weather.precipitation.slots;
	for (size_t i = 0; i < slots.size(); ++i) {
		slots[i].x = static_cast<int32_t>(static_cast<uint32_t>(i) * 0x9E3779B1u);
		slots[i].y = static_cast<int32_t>(static_cast<uint32_t>(i) * 0x85EBCA77u);
		slots[i].z = 0;
		slots[i].floor_z = 0;
	}
	// The camera 300 u east, 280 u north and 50 u up: every slot wraps into a volume whose
	// X / Y words lie past 2^24.
	const int32_t cam_x = 300 << 16, cam_y = 280 << 16, cam_z = 50 << 16;
	kernel->update_precipitation(cam_x, cam_y, cam_z);

	const terrain::TerrainHeightField &field = kernel->terrain_store.height_field();
	int mismatched = 0, raised = 0;
	for (const env::PrecipitationSlot &slot : slots) {
		const int32_t want = terrain::height_field_height_bilinear_fixed(field, slot.x, slot.y);
		if (slot.floor_z != want) ++mismatched;
		if (slot.floor_z != 0) ++raised;
	}
	CHECK(mismatched == 0);
	CHECK(raised > 0);
	// Every slot sits in the camera volume after the walk.
	bool inside = true;
	for (const env::PrecipitationSlot &slot : slots) {
		inside = inside &&
		         static_cast<uint32_t>(slot.x) -
		                         static_cast<uint32_t>(cam_x - env::PrecipitationField::kBackXY) <
		                 static_cast<uint32_t>(env::PrecipitationField::kSpanXY);
	}
	CHECK(inside);

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("precipitation_floor_test OK\n");
	return 0;
}
