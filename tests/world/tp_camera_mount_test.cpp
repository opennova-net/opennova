// The third-person camera's mounted and clearance legs.
// [orig: ThirdPersonCamera_Update @0x437AF0; Camera_ComputeThirdPersonView
//  @0x437D10 — the clearances @0x438409..0x438456, the slope march
//  @0x43846E..0x438619]

#include <world/tp_camera_mount.h>

#include <cmath>
#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

// The anchor lift has a FLOOR; the distance does not. That asymmetry is the
// point — a small hull must not put the camera inside itself, but it may sit
// close behind.
void test_mount_lift_floor() {
	// A large hull uses the scaled value.
	CHECK(near(mount_anchor_lift(8.0f), 3.0f), "0.375 of a big radius");
	// A small hull clamps up to the floor rather than scaling down.
	CHECK(near(mount_anchor_lift(1.0f), kMountAnchorMinLift),
			"a small hull clamps to the 1.0 floor");
	CHECK(near(mount_anchor_lift(0.0f), kMountAnchorMinLift),
			"a zero radius still lifts");
	// The crossover is where 0.375r == 1.0, i.e. r == 2.667.
	CHECK(mount_anchor_lift(3.0f) > kMountAnchorMinLift,
			"above the crossover the scale wins");

	// The distance has NO floor beyond its base term.
	CHECK(near(mount_distance(0.0f), kMountDistanceBase),
			"a zero radius gives the base distance");
	CHECK(near(mount_distance(2.0f), 4.0f), "1.0 + 1.5 * 2.0");
	CHECK(mount_distance(8.0f) > mount_distance(2.0f),
			"a bigger hull is framed from further back");
}

// The mounted anchor eases a sixteenth horizontally and a thirty-second
// vertically, with the half-step rounding — slower than the on-foot quarter.
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
	// No difference: the camera sits on the vehicle yaw.
	CHECK(near(mount_look_yaw(90.0f, 90.0f), 90.0f),
			"looking straight ahead leaves the camera on the hull yaw");
	// A 40-degree look offset moves the camera only 10.
	CHECK(near(mount_look_yaw(0.0f, 40.0f), 10.0f),
			"the camera takes a QUARTER of the look offset");
	// It never reaches the look yaw in one step.
	CHECK(mount_look_yaw(0.0f, 100.0f) < 100.0f,
			"the camera never snaps to the look direction");
	// Negative offsets damp the same way.
	CHECK(near(mount_look_yaw(0.0f, -40.0f), -10.0f), "and symmetrically");
	// The BAM form is an arithmetic shift: -3 >> 2 is -1, not 0.
	CHECK(mount_look_yaw_bam(0, -3) == -1, "the BAM quarter is an arithmetic shift");
	CHECK(mount_look_yaw_bam(1000, 1400) == 1100, "BAM: a quarter of the offset");
}

// A vehicle is looked DOWN on, not level with.
void test_mount_pitch_is_downward() {
	CHECK(kMountPitchDeg < 0.0f, "the mounted pitch is downward");
	CHECK(near(kMountPitchDeg, -11.25f), "and is the witnessed -11.25 degrees");
}

void test_aircraft_eye_drop() {
	CHECK(near(aircraft_eye_drop(4.0f), 2.0f), "half the carrier radius");
	CHECK(near(aircraft_eye_drop(0.0f), 0.0f), "a zero radius drops nothing");
	// The gate is the item def's class byte: 3 and 4 only.
	CHECK(vehicle_class_byte_is_aircraft(3) && vehicle_class_byte_is_aircraft(4),
			"classes 3 and 4 are aircraft");
	CHECK(!vehicle_class_byte_is_aircraft(2) && !vehicle_class_byte_is_aircraft(5),
			"2 and 5 are not — the test is a two-value window");
}

// The clearances raise the eye only when it is BELOW the surface — they are a
// floor, not an offset applied unconditionally.
void test_clearances_are_a_floor() {
	// Below the water: raised to water + clearance.
	CHECK(near(raise_above(1.0f, 2.0f, kWaterClearance), 2.25f),
			"an eye under water is raised to clear it");
	// Already above: untouched, NOT pushed further up.
	CHECK(near(raise_above(10.0f, 2.0f, kWaterClearance), 10.0f),
			"an eye already clear is left alone");
	// Exactly at the clearance line: untouched.
	CHECK(near(raise_above(2.25f, 2.0f, kWaterClearance), 2.25f),
			"exactly at the line is already clear");
	// Terrain behaves the same way.
	CHECK(near(raise_above(0.0f, 5.0f, kTerrainClearance), 5.25f),
			"terrain clearance works the same");

	// The water floor only applies while the ENTITY is above the water line;
	// a submerged entity keeps its underwater eye.
	CHECK(near(raise_above_water(1.0f, 2.0f, 5.0f), 2.25f),
			"an entity above water gets the water floor");
	CHECK(near(raise_above_water(1.0f, 2.0f, 1.5f), 1.0f),
			"a submerged entity keeps the eye where it is");
	CHECK(near(raise_above_water(1.0f, 2.0f, 2.25f), 1.0f),
			"exactly at water + clearance is NOT above it");

	// The terrain floor is skipped for the 0x800000 flag: the floor is 0.
	CHECK(near(raise_above_terrain(0.0f, 5.0f, 0u), 5.25f), "terrain + 0.25 normally");
	CHECK(near(raise_above_terrain(-1.0f, 5.0f, kEntityFlagCameraSkipsTerrainFloor), 0.0f),
			"with the flag the floor is the height-0 plane");
	CHECK(near(raise_above_terrain(3.0f, 5.0f, kEntityFlagCameraSkipsTerrainFloor), 3.0f),
			"and an eye above zero is left alone");
}

// The slope raise is a FLOOR from the anchor: the steepest sampled slope times
// the horizontal distance, plus 0.333 per unit — the literal 0.333, not one
// third.
void test_slope_raise_floor() {
	CHECK(near(kSlopeRaiseMargin, 0.333f), "the literal 0.333");
	CHECK(kSlopeRaiseMargin != 1.0f / 3.0f,
			"and NOT one third — the difference moves the floor");
	CHECK(near(kSlopeStep, 0.5f), "the slope march steps a half unit");
	// Flat ground: the margin alone lifts the floor.
	CHECK(near(slope_raise_floor(10.0f, 4.0f, 0.0f), 10.0f + 4.0f * 0.333f),
			"flat ground still adds the margin");
	// A 1:1 slope adds the full run on top of the margin.
	CHECK(near(slope_raise_floor(10.0f, 4.0f, 1.0f), 10.0f + 4.0f + 4.0f * 0.333f),
			"the steepest slope times the run, plus the margin");
	CHECK(near(slope_raise_floor(10.0f, 0.0f, 5.0f), 10.0f),
			"no horizontal distance, no raise");
}

// The entry seeds differ: entering play is a tight over-the-shoulder view,
// following a tracked entity starts further back and pitched up, and a dead
// target starts further back still.
void test_seeds_differ() {
	CHECK(kTpPlayDistance < kTpTrackedDistance,
			"entering play starts closer than tracking");
	CHECK(near(kTpPlayOrbitPitchDeg, 0.0f), "play zeroes the orbit");
	CHECK(kTpTrackedOrbitPitchDeg > 0.0f, "tracking starts pitched up");
	CHECK(kTpDeadTargetDistance > kTpTrackedDistance,
			"a dead target is framed from 10.0 before the reel-in");
}

} // namespace

int main() {
	test_mount_lift_floor();
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
