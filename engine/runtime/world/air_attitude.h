#pragma once

#include <cstdint>

namespace opennova::world {

// THE AIR-VEHICLE PITCH/ROLL INTEGRATOR.
//
// Retail never puts a live vehicle's pitch or roll on the wire — only its
// heading. Every client RE-DERIVES them locally, every tick, from what it does
// receive: the drive-command pair, the hull heading and the hull's own motion
// [orig: Entity_UpdateAircraftPhysics @0x490310, the CHel/cpln row of the
//  move_function class table @0x82ABC8].
//
// So a peer that skips this shows helicopters flying dead level. Ground
// vehicles use a different mechanism entirely (the seven-point terrain conform)
// and must not be fed through here.
//
// Angles and rates are BAM32 (2^32 per turn) per tick.

// The global rate ceiling: 15 degrees per tick
// [orig: @0x4925A8..0x492601; the negative bound is 0xF5555560].
inline constexpr int32_t kAttitudeRateClamp = 178956960;
// The def multiplier for the optional per-vehicle ceiling
// [orig: imul 0x2EFAA @0x492256].
inline constexpr int32_t kAttitudeDefRateUnit = 192426;
// entity+0x24 bits [orig: @0x491C95 / @0x491E35].
inline constexpr uint32_t kAttitudeFlagPowered = 0x80u;
inline constexpr uint32_t kAttitudeFlagAirborne = 0x2000u;

// ---------------------------------------------------------------------------
// SIDESLIP FEEDBACK, airborne only [orig: @0x491E42..0x491FA5]
// ---------------------------------------------------------------------------
//
// The hull's ACTUAL motion is compared against what it was commanded to do,
// and the difference is fed back as rate. This is what makes a helicopter bank
// into a slide rather than tracking its stick input exactly.

// Lateral feedback: only applies when the measured slip EXCEEDS the command,
// and the gain is 8 [orig: @0x491F12..0x491F2A].
inline int32_t sideslip_roll_delta(int32_t measured_lat, int32_t commanded_lat) {
	const int32_t am = measured_lat < 0 ? -measured_lat : measured_lat;
	const int32_t ac = commanded_lat < 0 ? -commanded_lat : commanded_lat;
	if (am <= ac) return 0;
	return 8 * commanded_lat - 8 * measured_lat;
}

// Forward feedback picks between TWO GAIN PAIRS by asking whether the larger
// correction would move the pitch CLOSER TO ZERO
// [orig: @0x491F50..0x491FA5]:
//   * if |pitch + (d << 5)| < |pitch| — the big step levels the hull — take
//     the strong pair: rate += d << 4, pitch += d << 5;
//   * otherwise take the weak pair: rate += d << 2, pitch += d << 2.
//
// The test is a levelling check, not a magnitude limit. Using one gain drops
// the nose-down authority a diving aircraft needs.
struct SideslipPitch {
	int32_t rate_delta = 0;
	int32_t angle_delta = 0;
};

inline SideslipPitch sideslip_pitch_delta(int32_t measured_fwd,
		int32_t commanded_fwd, int32_t pitch) {
	const int32_t am = measured_fwd < 0 ? -measured_fwd : measured_fwd;
	const int32_t ac = commanded_fwd < 0 ? -commanded_fwd : commanded_fwd;
	if (am <= ac) return {};
	const int32_t d = measured_fwd - commanded_fwd;
	const int32_t strong = pitch + (d << 5);
	const int32_t a_strong = strong < 0 ? -strong : strong;
	const int32_t a_pitch = pitch < 0 ? -pitch : pitch;
	if (a_strong < a_pitch) return {d << 4, d << 5};
	return {d << 2, d << 2};
}

// ---------------------------------------------------------------------------
// SELF-LEVEL, airborne only [orig: @0x4921CD..0x492210]
// ---------------------------------------------------------------------------
//
// A 1/512 pull toward level, applied to BOTH the rate and the angle. The +0x100
// is a round-half term before the shift, so the pull is symmetric about zero
// rather than biased toward negative angles.
inline int32_t self_level_delta(int32_t angle) {
	return (angle + 0x100) >> 9;
}

// ---------------------------------------------------------------------------
// RATE DAMP [orig: airborne @0x492213..0x492246; grounded @0x492323..0x492365]
// ---------------------------------------------------------------------------
//
// Airborne damps a SIXTEENTH per tick, grounded an EIGHTH — a grounded hull
// settles twice as fast.
//
// THE DAMP IS NOT SYMMETRIC, and the asymmetry is the interesting part. The
// shift truncates toward zero for a positive rate, so a rate SMALLER THAN THE
// DIVISOR produces a zero step and never decays at all: an airborne pitch rate
// of +7 stays +7 forever. The `+ (v >> 31)` term adds -1 only for negatives,
// which is exactly what lets a small NEGATIVE rate reach zero.
//
// So a hull left alone settles from below and creeps from above. That is what
// the image does; "fixing" it into a symmetric damp changes the resting
// attitude of every idle aircraft.
inline int32_t rate_damp(int32_t rate, bool airborne) {
	const int32_t step = airborne ? ((rate + 8) >> 4) : ((rate + 4) >> 3);
	return rate - (step + (rate >> 31));
}

// ---------------------------------------------------------------------------
// CEILINGS
// ---------------------------------------------------------------------------

// The optional per-vehicle ceiling, airborne only [orig: @0x49224C..0x4922BC].
// A zero def key leaves it inactive.
//
// Measured on the shipped items.def: NO helicopter and NO ground vehicle sets
// either key — only the two RHIBs and the PWC — so for every aircraft this is
// inactive and the global clamp is what actually binds.
inline int32_t apply_def_rate_ceiling(int32_t rate, int32_t def_key) {
	if (def_key == 0) return rate;
	const int64_t limit = static_cast<int64_t>(kAttitudeDefRateUnit) * def_key;
	if (rate > limit) return static_cast<int32_t>(limit);
	if (rate < -limit) return static_cast<int32_t>(-limit);
	return rate;
}

inline int32_t apply_global_rate_clamp(int32_t rate) {
	if (rate > kAttitudeRateClamp) return kAttitudeRateClamp;
	if (rate < -kAttitudeRateClamp) return -kAttitudeRateClamp;
	return rate;
}

} // namespace opennova::world
