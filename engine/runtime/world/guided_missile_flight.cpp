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

GuidedStepResult GuidedFlight::step(GuidedFlightState &st, const int32_t steer[3],
                                    int32_t velocity, int32_t turn_max_pit,
                                    int32_t turn_max_yaw, bool authority) {
	st.age += 1;

	// (a) pursuit error to the steer point — elevation/heading angles plus the
	// horizontal and total distances. Runs every tick, NOT age-gated
	// [orig: sub_546B30 @0x4463b5 -> compute_relative_position_metrics
	//  @0x545710: out = elevation BAM, heading BAM, horizontal dist, total
	//  dist. Local-frame in retail; world-frame equivalent here — see the
	//  header's approximation note].
	const double dx = static_cast<double>(steer[0] - st.pos[0]) / kFixedOne;
	const double dy = static_cast<double>(steer[1] - st.pos[1]) / kFixedOne;
	const double dz = static_cast<double>(steer[2] - st.pos[2]) / kFixedOne;
	const double horiz = std::sqrt(dx * dx + dy * dy);
	const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (dist <= 0.0001) return GuidedStepResult::kCoincident;
	const int32_t want_yaw =
			wrap_bam(static_cast<int64_t>(std::llround(std::atan2(dy, dx) / kBamToRad)));
	const double sp = std::max(-1.0, std::min(1.0, dz / dist));
	const int32_t want_pitch =
			wrap_bam(static_cast<int64_t>(std::llround(std::asin(sp) / kBamToRad)));
	const int32_t err_yaw = wrap_bam(static_cast<int64_t>(want_yaw) - st.yaw_bam);
	const int32_t err_pitch = wrap_bam(static_cast<int64_t>(want_pitch) - st.pitch_bam);
	const int64_t total_q16 = static_cast<int64_t>(dist * kFixedOne);
	const int64_t horiz_q16 = static_cast<int64_t>(horiz * kFixedOne);

	// (b) the AUTHORITY-ONLY termination leg [orig: the `!is_in_session ||
	// is_authority` gate @0x4463cb] — a non-authority client skips all three
	// and flies until the wire's group 1 ends it.
	bool overshoot = false;
	bool proximity = false;
	if (authority) {
		// Overshoot marks the detonate bits and FALLS THROUGH — the tick still
		// advances; the next tick's entry gate returns on the dead bit
		// [orig: @0x446484 sets +696|=1/+276|=0x1000, no early return;
		//  dead-bit early-out @0x44607a].
		if (std::abs(static_cast<int64_t>(err_pitch)) > kOvershoot ||
		    std::abs(static_cast<int64_t>(err_yaw)) > kOvershoot)
			overshoot = true;
		// The steer guard returns BEFORE the advance [orig: total distance
		//  < 0x8000 @0x4464b7 -> detonate bits, return].
		if (total_q16 < kSteerGuard) return GuidedStepResult::kGuard;
		// Proximity is an AI notify, not a detonation [orig: horizontal
		//  distance < 0x1900000 @0x446587 -> AIEvent_QueueEntry(12)].
		if (horiz_q16 < kProximity) proximity = true;
	}

	// (c) the advance is age-gated: no motion until age >= 31 (ignition hold)
	// [orig: cmp 31 @0x446538/@0x44663d].
	if (st.age < kAgeGate)
		return overshoot ? GuidedStepResult::kOvershoot
		                 : (proximity ? GuidedStepResult::kProximityNotify
		                              : GuidedStepResult::kNone);

	// (d) boost ramp: speed = ((velocity / max(1, 39-age)) << 16) / 62 with
	// the original's integer truncation before the shift
	// [orig: @0x446543..0x446572 / @0x446644..0x446677].
	const int32_t divisor = std::max(1, kBoostFullAge - st.age);
	const int32_t speed = static_cast<int32_t>(
			(static_cast<int64_t>(velocity / divisor) << 16) / kTickDiv);

	// (e) turn-limited heading: per-axis clamp (def +0x50 pitch / +0x54 yaw,
	// <=0 -> 6734910), new angle = clamped error + current angle
	// [orig: Entity_UpdateTurretAim @0x445CC0].
	const int64_t max_p = turn_max_pit > 0 ? turn_max_pit : kTurnDefault;
	const int64_t max_y = turn_max_yaw > 0 ? turn_max_yaw : kTurnDefault;
	st.pitch_bam = wrap_bam(static_cast<int64_t>(st.pitch_bam) +
	                        clamp_i32(err_pitch, -max_p, max_p));
	st.yaw_bam = wrap_bam(static_cast<int64_t>(st.yaw_bam) +
	                      clamp_i32(err_yaw, -max_y, max_y));

	// (f) velocity = forward(orientation) * speed; position += velocity
	// [orig: the rotated [speed,0,0] @0x445d70..0x445d8c; the generic
	//  entity-move velocity add].
	const double yr = static_cast<double>(st.yaw_bam) * kBamToRad;
	const double pr = static_cast<double>(st.pitch_bam) * kBamToRad;
	st.pos[0] += static_cast<int32_t>(std::cos(yr) * std::cos(pr) * speed);
	st.pos[1] += static_cast<int32_t>(std::sin(yr) * std::cos(pr) * speed);
	st.pos[2] += static_cast<int32_t>(std::sin(pr) * speed);

	return overshoot ? GuidedStepResult::kOvershoot
	                 : (proximity ? GuidedStepResult::kProximityNotify
	                              : GuidedStepResult::kNone);
}

} // namespace opennova::world
