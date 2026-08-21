// The third-person camera's mounted and clearance legs.
// [orig: ThirdPersonCamera_Update @0x437AF0; the clamps and slope raise
//  @0x438409..0x438619]

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
}

// A vehicle is looked DOWN on, not level with.
void test_mount_pitch_is_downward() {
	CHECK(kMountPitchDeg < 0.0f, "the mounted pitch is downward");
	CHECK(near(kMountPitchDeg, -11.25f), "and is the witnessed -11.25 degrees");
}

void test_aircraft_eye_drop() {
	CHECK(near(aircraft_eye_drop(4.0f), 2.0f), "half the carrier radius");
	CHECK(near(aircraft_eye_drop(0.0f), 0.0f), "a zero radius drops nothing");
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
}

// The slope minimum rise is the LITERAL 0.333, not one third. Writing 1/3
// gives 0.33333433 and moves where the march stops on a shallow slope.
void test_slope_min_rise_is_the_literal() {
	CHECK(near(kSlopeMinRise, 0.333f), "the literal 0.333");
	CHECK(kSlopeMinRise != 1.0f / 3.0f,
			"and NOT one third — the difference changes the march termination");
	CHECK(near(kSlopeStep, 0.5f), "the slope march steps a half unit");
}

// The two entry seeds differ: entering play is a tight over-the-shoulder view,
// following a tracked entity starts further back and pitched up.
void test_seeds_differ() {
	CHECK(kTpPlayDistance < kTpTrackedDistance,
			"entering play starts closer than tracking");
	CHECK(near(kTpPlayOrbitPitchDeg, 0.0f), "play zeroes the orbit");
	CHECK(kTpTrackedOrbitPitchDeg > 0.0f, "tracking starts pitched up");
}

} // namespace

int main() {
	test_mount_lift_floor();
	test_mount_look_yaw_damping();
	test_mount_pitch_is_downward();
	test_aircraft_eye_drop();
	test_clearances_are_a_floor();
	test_slope_min_rise_is_the_literal();
	test_seeds_differ();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("tp_camera_mount_test OK\n");
	return 0;
}
