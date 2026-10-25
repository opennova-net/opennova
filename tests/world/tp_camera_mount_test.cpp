// The third-person camera's mounted and clearance legs, in words.
// [orig: ThirdPersonCamera_Update @0x437AF0; Camera_ComputeThirdPersonView
//  @0x437D10 — the clearances @0x438409..0x438456, the slope march
//  @0x43846E..0x438619]

#include <runtime/world/tp_camera_mount.h>
#include <runtime/world/player_view.h>

#include <cstdint>
#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// The anchor lift has a FLOOR; the distance does not. That asymmetry is the
// point: a small hull must not put the camera inside itself, but it may sit
// close behind.
void test_mount_lift_and_distance() {
	CHECK(mount_anchor_lift_q16(8 * 0x10000) == 3 * 0x10000, "0.375 of 8.0 is 3.0");
	CHECK(mount_anchor_lift_q16(0x10000) == 0x10000, "a small hull floors at 1.0");
	CHECK(mount_anchor_lift_q16(0) == 0x10000, "a zero radius still lifts");
	CHECK(mount_anchor_lift_q16(3 * 0x10000) > 0x10000, "above the crossover the scale wins");

	// 0x10000 less ftol(r * -1.5) [orig: @0x438121..0x438136].
	CHECK(mount_distance_q16(0) == 0x10000, "a zero radius gives the base distance");
	CHECK(mount_distance_q16(2 * 0x10000) == 4 * 0x10000, "1.0 + 1.5 * 2.0");
	CHECK(mount_distance_q16(4 * 0x10000) == 7 * 0x10000, "1.0 + 1.5 * 4.0");
	// The ftol truncates toward zero: r = 3 words gives -4.5 -> -4.
	CHECK(mount_distance_q16(3) == 0x10000 + 4, "the half word truncates toward zero");
	CHECK(mount_distance_q16(8 * 0x10000) > mount_distance_q16(2 * 0x10000),
			"a bigger hull is framed from further back");
}

// The mounted anchor eases a sixteenth horizontally and a thirty-second
// vertically, with the half-step rounding: slower than the on-foot quarter.
void test_mount_anchor_ease() {
	CHECK(mount_anchor_ease_q16(0, 0x10000, kMountAnchorEaseShiftXY) == 0x1000,
			"a sixteenth of the gap horizontally");
	CHECK(mount_anchor_ease_q16(0, 0x10000, kMountAnchorEaseShiftZ) == 0x800,
			"a thirty-second vertically");
	// The rounding term: 8 >> 4 rounds a gap of 8 up to 1, 7 stays 0.
	CHECK(mount_anchor_ease_q16(0, 8, kMountAnchorEaseShiftXY) == 1, "+8 >> 4 rounds up");
	CHECK(mount_anchor_ease_q16(0, 7, kMountAnchorEaseShiftXY) == 0, "below half stays");
	CHECK(mount_anchor_ease_q16(100, 100, kMountAnchorEaseShiftZ) == 100, "no gap, no move");
}

// LOOK YAW IS DAMPED TO A QUARTER. Following it outright would swing the
// camera around the hull instead of letting the vehicle lead.
void test_mount_look_yaw_damping() {
	CHECK(mount_look_yaw_bam(0x10000000, 0x10000000) == 0x10000000,
			"looking straight ahead leaves the camera on the hull yaw");
	CHECK(mount_look_yaw_bam(1000, 1400) == 1100, "a quarter of the offset");
	// The BAM form is an arithmetic shift: -3 >> 2 is -1, not 0.
	CHECK(mount_look_yaw_bam(0, -3) == -1, "the BAM quarter is an arithmetic shift");
	// The difference wraps across the seam: from just below +180 to just past
	// it is a short positive offset, not a near-full negative turn.
	CHECK(mount_look_yaw_bam(0x7FFFFF00, static_cast<int32_t>(0x80000100u)) ==
			static_cast<int32_t>(0x7FFFFF00 + 0x80),
			"the offset is the wrapped shortest arc");
}

// A vehicle is looked DOWN on: -0x08000000 BAM, -11.25 degrees.
void test_mount_pitch_is_downward() {
	CHECK(kMountPitchBam == -0x08000000, "the witnessed 0xF8000000");
}

// An AIRCRAFT drops the eye by half its radius; unit_type 3/4 are the air
// classes (Entity_GetVehicleClass @0x4f9e27: 5..8 are the boats).
void test_aircraft_eye_drop() {
	CHECK(aircraft_eye_drop_q16(4 * 0x10000) == 2 * 0x10000, "half the carrier radius");
	CHECK(aircraft_eye_drop_q16(3) == 1, "sar 1");
	CHECK(aircraft_eye_drop_q16(0) == 0, "a zero radius drops nothing");
	CHECK(vehicle_unit_type_is_aircraft(3) && vehicle_unit_type_is_aircraft(4),
			"unit types 3 and 4 are aircraft");
	CHECK(!vehicle_unit_type_is_aircraft(2) && !vehicle_unit_type_is_aircraft(5),
			"2 (land) and 5 (a boat) are not: the test is a two-value window");
}

// The clearances raise the eye only when it is BELOW the surface: they are a
// floor, not an offset applied unconditionally.
void test_clearances_are_a_floor() {
	constexpr int32_t one = 0x10000;
	// The water floor only applies while the ENTITY is above the water line;
	// a submerged entity keeps its underwater eye.
	CHECK(raise_above_water_q16(1 * one, 2 * one, 5 * one) == 2 * one + 0x4000,
			"an entity above water gets the water floor");
	CHECK(raise_above_water_q16(10 * one, 2 * one, 5 * one) == 10 * one,
			"an eye already clear is left alone");
	CHECK(raise_above_water_q16(2 * one + 0x4000, 2 * one, 5 * one) == 2 * one + 0x4000,
			"exactly at the line is already clear");
	CHECK(raise_above_water_q16(1 * one, 2 * one, one + one / 2) == 1 * one,
			"a submerged entity keeps the eye where it is");
	CHECK(raise_above_water_q16(1 * one, 2 * one, 2 * one + 0x4000) == 1 * one,
			"exactly at water + clearance is NOT above it");

	// The terrain floor is skipped INDOORS (Flags & 0x800000): the floor is 0.
	CHECK(raise_above_terrain_q16(0, 5 * one, false) == 5 * one + 0x4000,
			"terrain + 0.25 normally");
	CHECK(raise_above_terrain_q16(9 * one, 5 * one, false) == 9 * one,
			"an eye above the floor is left alone");
	CHECK(raise_above_terrain_q16(-one, 5 * one, true) == 0,
			"indoors the floor is the height-0 plane");
	CHECK(raise_above_terrain_q16(3 * one, 5 * one, true) == 3 * one,
			"and an eye above zero is left alone");
}

// THE SLOPE RAISE in words [orig: @0x43846E..0x438619]: a half-unit march from
// the anchor toward the eye, the steepest (h - anchor z) / run seeded at 0.0,
// and the floor anchor z + ftol(max * length + ftol(L) * 0.333f).
void test_slope_raise_floor() {
	CHECK(kSlopeRaiseMargin == 0.333f, "the literal 0.333f");
	CHECK(kSlopeRaiseMargin != 1.0f / 3.0f, "and NOT one third: the difference moves the floor");
	CHECK(kSlopeStepQ16 == 0x8000, "the march steps a half unit");
	const int32_t anchor[3] = {0, 0, 0};
	// The eye 1.25 u out along +X: L = 81920, the unit ftol(81920 * 65536 /
	// 81920) = 65536, the step 0x8000, the march length 81920. The run is
	// tested BEFORE each step (0, then 0x8000, then 0x10000, all below 81920),
	// so the third sample lands at x = 0x18000: half a unit PAST the eye.
	int samples = 0;
	int32_t last_x = 0;
	const auto spike_past_eye = [&](int32_t x, int32_t y) {
		++samples;
		last_x = x;
		return x == 0x18000 && y == 0 ? 0x18000 : 0;
	};
	const int32_t floor_z = mount_slope_raise_floor_q16(anchor, 0x14000, 0, spike_past_eye);
	CHECK(samples == 3 && last_x == 0x18000, "three samples, the last past the eye");
	// The spike's slope 0x18000 / 0x18000 = 1.0 is the max: ftol(1.0 * 81920 +
	// 81920 * 0.333f) = ftol(109199.36...) = 109199.
	CHECK(floor_z == 109199, "the past-the-eye sample sets the floor");

	// Flat or downhill ground: the max stays at its 0.0 seed, so the margin
	// alone lifts the floor: ftol(81920 * 0.333f) = ftol(27279.36...) = 27279.
	const auto downhill = [](int32_t, int32_t) { return -0x40000; };
	CHECK(mount_slope_raise_floor_q16(anchor, 0x14000, 0, downhill) == 27279,
			"a downhill run never lowers the floor");

	// No horizontal distance: no unit, no march, no raise.
	const auto never = [&](int32_t, int32_t) { ++samples; return 0x7FFF0000; };
	samples = 0;
	const int32_t above[3] = {5, 6, 7};
	CHECK(mount_slope_raise_floor_q16(above, 5, 6, never) == 7 && samples == 0,
			"the eye over the anchor takes no sample");
}

// The entry seeds differ: entering play is a tight over-the-shoulder view,
// following a tracked entity starts further back and pitched up, and a dead
// target starts further back still.
void test_seeds_differ() {
	CHECK(kTpDistanceQ16 < kTpTrackedDistanceQ16,
			"entering play starts closer than tracking");
	CHECK(PlayerViewState{}.chase_orbit_pitch == 0, "play zeroes the orbit");
	CHECK(kTpTrackedOrbitPitchBam > 0, "tracking starts pitched up");
	CHECK(kTpDeadTargetDistanceQ16 > kTpTrackedDistanceQ16,
			"a dead target is framed from 10.0 before the reel-in");
}

} // namespace

int main() {
	test_mount_lift_and_distance();
	test_mount_anchor_ease();
	test_mount_look_yaw_damping();
	test_mount_pitch_is_downward();
	test_aircraft_eye_drop();
	test_clearances_are_a_floor();
	test_slope_raise_floor();
	test_seeds_differ();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("tp_camera_mount_test OK\n");
	return 0;
}
