// The two force passes' terrain broad phase (D-NET-515, the resting aircraft hulls): a probe
// whose four bilinear samples all lie more than its radius below it is skipped. Retail's
// CheckCollisionState (the tracked, wheeled and light solves) never skips its first four
// probes, the wheels; ComputeCollisionForces (the aircraft, platform and physics-0 suspension
// solves) skips any probe, since every one of its calls passes no per-wheel array and the skip
// tests that array alone. A wheel probe that is not skipped takes the grazing correction, and
// that correction's wrapped product can still push a probe that is clear of the slope.
// [orig: Entity_CheckCollisionState @0x462A30 - the `wheelIdx < 4` test @0x462C13..0x462C45,
//  the grazing `imul eax, ebx; cdq; idiv` @0x462C5F; Entity_ComputeCollisionForces @0x462150 -
//  the skip `!per_wheel_distances && ...` @0x4622D2; the null per_wheel_distances pushes of
//  Entity_ProcessAircraftContactPhysics @0x47F855 / @0x47FE13]
//
// The 45-degree ramp of vehicle_motor's grazing test (h = X), a probe at X = 10 u, Y = -5 u,
// r = 2 u, now at Z = 15 u: its samples 8 / 12 / 10 / 10 u all lie more than 2 u below it.
// Hand-run, the pass that evaluates it:
//   rel = 983040 - 655360 = 327680; gx = 262144, gz = -262144, norm = int(370727.6) = 370727
//   fx = 34359738368 / 370727 = 92682; pen = 327680 - 92682 = 234998
//   pen * gz = -61603315712 -> low dword -1473773568 -> / gx = -5622
//   |-5622| < |92682| -> fx = 87060, hit, pen = 0
//   mag = int(339048.12) = 339048; cosr = (327680 << 22) / 339048 = 4053672 < soft 0x400000
//   -> severity 3: out.fx = -87060, out.fy = 0, out.fz = 0.
// Each truncated root sits clear of its next integer at 24, 53 and 64 bits (Q3-11).

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

struct Ramp {
	std::vector<uint16_t> heightmap = std::vector<uint16_t>(64 * 64);
	std::vector<int> sector_grid = std::vector<int>(256, 1);
	opennova::terrain::TerrainHeightField field{};
	Ramp() {
		for (int z = 0; z < 64; ++z)
			for (int x = 0; x < 64; ++x)
				heightmap[z * 64 + x] = uint16_t(x * 256);
		field.heightmap = heightmap.data();
		field.dim = 64;
		field.layout.sector_grid = sector_grid.data();
	}
};

// Five probes at one point: four wheels and a spine probe.
int32_t run_pass(detail::ProbeForcePass pass, detail::PlatProbeForce (&forces)[5]) {
	Ramp ramp;
	auto world = std::make_unique<World>();
	world->tables.terrain = &ramp.field;
	Entity hull;
	int32_t probes[5][3];
	int32_t radii[5];
	for (int i = 0; i < 5; ++i) {
		probes[i][0] = 10 << 16;
		probes[i][1] = -(5 << 16);
		probes[i][2] = 15 << 16;
		radii[i] = 2 << 16;
	}
	EntityHandle hit;
	return detail::vehicle_probe_pass(*world, hull, pass, probes, radii, 5, 0x400000, 0x400000,
			forces, 10 << 16, -(5 << 16), 13 << 16, &hit);
}

void test_the_ground_pass_evaluates_its_wheels() {
	detail::PlatProbeForce forces[5];
	const int32_t severity = run_pass(detail::ProbeForcePass::CheckCollisionState, forces);
	CHECK(severity == 3, "the wheel probe's grazing push is severity 3");
	for (int i = 0; i < 4; ++i) {
		CHECK(forces[i].fx == -87060, "each wheel probe takes the wrapped grazing push");
		CHECK(forces[i].fy == 0 && forces[i].fz == 0, "and no lateral or vertical force");
	}
	CHECK(forces[4].fx == 0 && forces[4].fy == 0 && forces[4].fz == 0,
			"the fifth probe, clear by more than its radius, is skipped");
}

void test_the_aircraft_pass_skips_every_clear_probe() {
	detail::PlatProbeForce forces[5];
	const int32_t severity = run_pass(detail::ProbeForcePass::ComputeCollisionForces, forces);
	CHECK(severity == 0, "no probe contacts");
	for (int i = 0; i < 5; ++i)
		CHECK(forces[i].fx == 0 && forces[i].fy == 0 && forces[i].fz == 0,
				"every probe clear by more than its radius is skipped, the pads included");
}

} // namespace

int main() {
	test_the_ground_pass_evaluates_its_wheels();
	test_the_aircraft_pass_skips_every_clear_probe();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("probe_force_pass: all passed\n");
	return 0;
}
