#include "world/guided_missile_flight.h"

#include <algorithm>
#include <cmath>

namespace opennova::world {

namespace {
constexpr double kBamToRad = 6.283185307179586 / 4294967296.0;
constexpr double kFixedOne = 65536.0;

int32_t clamp_i32(int64_t v, int64_t lo, int64_t hi) {
	return static_cast<int32_t>(std::max(lo, std::min(hi, v)));
}
} // namespace

int32_t GuidedFlight::wrap_bam(int64_t v) {
	// ((v + 0x80000000) & 0xFFFFFFFF) - 0x80000000 — the signed short-way wrap.
	return static_cast<int32_t>(((v + 0x80000000LL) & 0xFFFFFFFFLL) - 0x80000000LL);
}

void GuidedFlight::aim_at(GuidedFlightState &st, const int32_t steer[3]) {
	const double dx = static_cast<double>(steer[0] - st.pos[0]) / kFixedOne;
	const double dy = static_cast<double>(steer[1] - st.pos[1]) / kFixedOne;
	const double dz = static_cast<double>(steer[2] - st.pos[2]) / kFixedOne;
	const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (dist <= 0.0001) return;
	st.yaw_bam = wrap_bam(static_cast<int64_t>(std::llround(std::atan2(dy, dx) / kBamToRad)));
	const double s = std::max(-1.0, std::min(1.0, dz / dist));
	st.pitch_bam = wrap_bam(static_cast<int64_t>(std::llround(std::asin(s) / kBamToRad)));
}

GuidedDetonate GuidedFlight::step(GuidedFlightState &st, const int32_t steer[3],
                                  int32_t velocity, int32_t turn_max_pit,
                                  int32_t turn_max_yaw) {
	st.age += 1;
	// Ignition hold — no guidance yet [orig: cmp 0x1f @0x446535].
	if (st.age < kAgeGate) return GuidedDetonate::kNone;

	// (a) pursuit error to the steer point, in the missile's LOCAL frame
	// [orig: sub_546B30 -> compute_relative_position_metrics @0x545710].
	const double dx = static_cast<double>(steer[0] - st.pos[0]) / kFixedOne;
	const double dy = static_cast<double>(steer[1] - st.pos[1]) / kFixedOne;
	const double dz = static_cast<double>(steer[2] - st.pos[2]) / kFixedOne;
	const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (dist <= 0.0001) return GuidedDetonate::kCoincident;
	const int32_t want_yaw =
			wrap_bam(static_cast<int64_t>(std::llround(std::atan2(dy, dx) / kBamToRad)));
	const double sp = std::max(-1.0, std::min(1.0, dz / dist));
	const int32_t want_pitch =
			wrap_bam(static_cast<int64_t>(std::llround(std::asin(sp) / kBamToRad)));
	const int32_t err_yaw = wrap_bam(static_cast<int64_t>(want_yaw) - st.yaw_bam);
	const int32_t err_pitch = wrap_bam(static_cast<int64_t>(want_pitch) - st.pitch_bam);
	const int64_t aim_z = static_cast<int64_t>(dist * kFixedOne);

	// Termination tests [orig: @0x44646c overshoot, @0x4464b7 steer guard] —
	// these fire BEFORE the position advance.
	if (std::abs(static_cast<int64_t>(err_yaw)) > kOvershoot ||
	    std::abs(static_cast<int64_t>(err_pitch)) > kOvershoot)
		return GuidedDetonate::kOvershoot;
	if (aim_z < kSteerGuard) return GuidedDetonate::kGuard;

	// (b) boost ramp: speed = (velocity / max(1, 39-age)) * 65536 / 62
	// [orig: @0x44654f-0x446572].
	const int32_t divisor = std::max(1, kBoostFullAge - st.age);
	const int32_t speed = static_cast<int32_t>(
			static_cast<double>(velocity) / static_cast<double>(divisor) * kFixedOne /
			static_cast<double>(kTickDiv));

	// (c) turn-limited heading [orig: Entity_UpdateTurretAim @0x445CC0].
	const int64_t max_p = turn_max_pit > 0 ? turn_max_pit : kTurnDefault;
	const int64_t max_y = turn_max_yaw > 0 ? turn_max_yaw : kTurnDefault;
	st.pitch_bam = wrap_bam(static_cast<int64_t>(st.pitch_bam) +
	                        clamp_i32(err_pitch, -max_p, max_p));
	st.yaw_bam = wrap_bam(static_cast<int64_t>(st.yaw_bam) +
	                      clamp_i32(err_yaw, -max_y, max_y));

	// (d) velocity = forward(orientation) * speed; position += velocity.
	const double yr = static_cast<double>(st.yaw_bam) * kBamToRad;
	const double pr = static_cast<double>(st.pitch_bam) * kBamToRad;
	st.pos[0] += static_cast<int32_t>(std::cos(yr) * std::cos(pr) * speed);
	st.pos[1] += static_cast<int32_t>(std::sin(yr) * std::cos(pr) * speed);
	st.pos[2] += static_cast<int32_t>(std::sin(pr) * speed);

	// Proximity detonates AFTER the advance [orig: @0x44657f].
	return aim_z < kProximity ? GuidedDetonate::kProximity : GuidedDetonate::kNone;
}

} // namespace opennova::world
