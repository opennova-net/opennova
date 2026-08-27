// The mode-4 death camera (world/death_camera.h): the FROM/TO computation of
// Camera_ComputeThirdPersonPositions @0x438b80 (the too-close hold, the
// first full-reach trial winning, the 5.0 u push-out), the probe's count-0
// path, and the 128-tick lerp of @0x4389f8..0x438b3d.
#include <cmath>
#include <cstdio>
#include <cstdint>

#include <runtime/world/death_camera.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

constexpr int32_t Q(double u) { return static_cast<int32_t>(u * 65536.0); }

int32_t clear_probe(const int32_t *, const int32_t *, int32_t, int32_t max_dist) {
	return max_dist;
}

void test_too_close_holds_the_from_pose() {
	const int32_t player[3] = {Q(100), Q(200), Q(10)};
	const int32_t anchor[3] = {Q(100.5), Q(200), Q(10)}; // 0.5 u < 1.0 u
	DeathCameraState s;
	death_camera_compute(player, anchor, clear_probe, s);
	CHECK(s.valid);
	// The direction becomes +X, the trial loop still runs, and TO == FROM.
	CHECK(s.to.pos[0] == s.from.pos[0] && s.to.pos[1] == s.from.pos[1] &&
			s.to.pos[2] == s.from.pos[2]);
	CHECK(s.to.yaw_bam == s.from.yaw_bam && s.to.pitch_bam == s.from.pitch_bam);
	CHECK(s.from.pos[2] > player[2]); // lifted 0.5 u
}

void test_clear_probe_first_trial_wins_at_full_reach() {
	// Anchor 10 u north (+Y) of the player: dir = (0, 1, 0).
	const int32_t player[3] = {Q(0), Q(0), Q(0)};
	const int32_t anchor[3] = {Q(0), Q(10), Q(0)};
	DeathCameraState s;
	death_camera_compute(player, anchor, clear_probe, s);
	CHECK(s.valid);
	// With every probe returning the full reach the FIRST trial (+0x1000000
	// about the vertical, 1.40625 deg) wins and FROM = player + best * 5.0.
	const double a = 0x1000000 / 4294967296.0 * 2.0 * 3.14159265358979323846;
	// trial = (s*ny - c*nx, -(s*nx + c*ny), -nz) with n = (0,1,0):
	//       = (sin a, -cos a, 0)
	const double ex = std::sin(a) * 5.0;
	const double ey = -std::cos(a) * 5.0;
	CHECK(std::fabs(s.from.pos[0] / 65536.0 - ex) < 0.01);
	CHECK(std::fabs(s.from.pos[1] / 65536.0 - ey) < 0.01);
	CHECK(std::fabs(s.from.pos[2] / 65536.0 - 0.5) < 0.001);
	// TO: the lifted player pushed 5.0 u back along -dir = (0,-1,0), looking
	// along +Y: atan2(1, 0) = +90 deg = 0x40000000 BAM.
	CHECK(s.to.pos[0] == 0 && std::fabs(s.to.pos[1] / 65536.0 + 5.0) < 0.001);
	CHECK(std::fabs(s.to.pos[2] / 65536.0 - 0.5) < 0.001);
	CHECK(std::abs(s.to.yaw_bam - 0x40000000) < 0x10000);
	CHECK(std::abs(s.to.pitch_bam) < 0x10000);
	// FROM looks back along -best: atan2(cos a, -sin a) ~ 90 + 1.4 deg.
	const double from_yaw = std::atan2(std::cos(a), -std::sin(a)) * 683565275.5764316;
	CHECK(std::abs(static_cast<int64_t>(s.from.yaw_bam) - static_cast<int64_t>(from_yaw)) < 0x20000);
}

void test_lerp_half_at_64_and_saturates_at_128() {
	DeathCameraState s;
	s.valid = true;
	s.start_tick = 1000;
	s.from.pos[0] = Q(0);
	s.to.pos[0] = Q(10);
	s.from.yaw_bam = 0;
	s.to.yaw_bam = 0x20000000;
	DeathCameraPose p;
	death_camera_view(s, 1000, p);
	CHECK(p.pos[0] == Q(0) && p.yaw_bam == 0);
	death_camera_view(s, 1064, p);
	CHECK(p.pos[0] == Q(5) && p.yaw_bam == 0x10000000);
	death_camera_view(s, 1128, p);
	CHECK(p.pos[0] == Q(10) && p.yaw_bam == 0x20000000);
	death_camera_view(s, 5000, p);
	CHECK(p.pos[0] == Q(10) && p.yaw_bam == 0x20000000);
}

void test_probe_terrain_leg() {
	// No bones: the untouched reach [orig: @0x4378c4].
	const int32_t origin[3] = {0, 0, Q(0.5)};
	const int32_t dir[3] = {Q(1), 0, 0};
	CHECK(death_camera_probe_terrain(nullptr, 0, origin, dir, Q(0.5), Q(5)) == Q(5));
	// One bone, ground at z = 0.25 under a 0.5 u origin with the 0.5 u lift:
	// lift + ground (0.75) exceeds the first sample's z (0.5), so the walk
	// stops at 0.25 u with accum = -dir/4 and the dot cancels the reach.
	const auto flat = [](int32_t, int32_t) { return Q(0.25); };
	const int32_t reach = death_camera_probe_terrain(flat, 1, origin, dir, Q(0.5), Q(5));
	// step 0.25 hit: reached 0.25, accum = -dir/4 -> dot = -0.25 -> 0.
	CHECK(reach == 0);
	// Ground far below: the walk exhausts at 19 x 0.25 = 4.75 u.
	const auto deep = [](int32_t, int32_t) { return Q(-100); };
	CHECK(death_camera_probe_terrain(deep, 1, origin, dir, Q(0.5), Q(5)) == Q(4.75));
}

} // namespace

int main() {
	test_too_close_holds_the_from_pose();
	test_clear_probe_first_trial_wins_at_full_reach();
	test_lerp_half_at_64_and_saturates_at_128();
	test_probe_terrain_leg();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("death_camera_test OK\n");
	return 0;
}
