#pragma once

#include <cstdint>

namespace opennova::world {

// THE GROUND-VEHICLE TERRAIN CONFORM — the client-side solve retail runs for
// every items.def physics-1 vehicle, every tick, remote or local
// [orig: Entity_UpdateVehiclePhysics @0x48AF00 -> (unconditional @0x48D0B1)
//  Entity_ProcessTrackedVehiclePhysics @0x47C1C0].
//
// This is what keeps a jeep's wheels on the ground instead of the hull sliding
// through a hillside at a fixed attitude. It runs on EVERY machine for EVERY
// such vehicle, which is why a client that skips it looks wrong even when the
// wire data is perfect.
//
// OWNERSHIP, witnessed at both conform exits: Pitch and Roll are written
// UNCONDITIONALLY; Yaw only when the parked latch (+0x2EC) is set
// [orig: @0x47E61F..0x47E632 / @0x47EC75..0x47EC8F]. A client holding the five
// state bytes at zero therefore takes Yaw from the wire and owns Pitch/Roll
// locally — the split matters, because owning Yaw too would fight the wire.
//
// Fixed point throughout, as retail: positions and heights 16.16, slope
// thresholds Q22, suspension travel 16.16 with full extension 0xFFFF.

// Suspension extension range [orig: dword_815180 — a .data constant with no
// writer in the image] and the per-step delta it is divided into
// [orig: >> 4 @0x47CBE5].
inline constexpr int32_t kSuspFull = 0xFFFF;
inline constexpr int32_t kSuspStep = 0xFFF;

// The NON-AUTHORITY spring scale [orig: flt_7C6F14 @0x46B1C5..0x46B1DB]. A
// client runs its springs 1.75x the authority's; this is a witnessed
// difference between the two roles, not a tuning value to normalise away.
inline constexpr float kClientSpringScale = 1.75f;
// The unparked time scale [orig: flt_7C3DC8 @0x47C222].
inline constexpr float kTimeScaleUnparked = 0.75f;

// The wheel oscillator's constants [orig: sub_45D110 — note Hex-Rays names it
// Entity_ApplyDamageOscillationFast, which is wrong; it is the suspension
// oscillator].
inline constexpr float kOscPhaseStep = 0.2616667f; // flt_7C6A10
inline constexpr float kOscDecayA = 0.09090909f;   // flt_7C6A04, = 1/11
inline constexpr float kOscDecayB = 0.99f;         // flt_7C6A00
// The phase preset a compressing wheel is forced to [orig: @0x45CFC8].
inline constexpr float kOscPhasePreset = 1.57f;

// Suspension travel from the def's spring_comp PERCENTAGE
// [orig: the dword_815184 derivation @0x47C51F..0x47C544 —
//  travel = 0xFFFF - 0xFFFF * (100 - spring_comp) * 0.01].
// A spring_comp of 100 gives full travel; 0 gives none.
inline int32_t conform_travel_from_def(int spring_comp) {
	if (spring_comp < 0) spring_comp = 0;
	if (spring_comp > 100) spring_comp = 100;
	const float t = static_cast<float>(kSuspFull) *
			static_cast<float>(100 - spring_comp) * 0.01f;
	return kSuspFull - static_cast<int32_t>(t);
}

// A slope threshold from a def BAM field, as Q22
// [orig: @0x47C54C..0x47C587 — cos(field * 2pi / 2^32) * 2^22; the slip
//  threshold comes from slip_slope (+0x8F8) and the max from max_slope
//  (+0x8F4)]. Implemented in the caller's math so the cos is shared; this
// header states the scaling the result must carry.
inline constexpr float kConformThresholdScale = 4194304.0f; // 2^22

// The wheel radius the contact points use: a quarter of the bounds band's
// width [orig: w4 = (B15 - B14) >> 2 @0x47C804].
inline int32_t conform_wheel_radius(int32_t b14, int32_t b15) {
	return (b15 - b14) >> 2;
}

// The SPINE points' radius, which is clamped rather than derived outright
// [orig: v42 = clamp(min(((B11-B10)>>1) - 0x4000, ((B15-B14)>>1) - 0x1000),
//  >= 0x2000) @0x47C998..0x47C9EA]. The floor is what stops a low or narrow
// hull from collapsing its spine probes to nothing.
inline constexpr int32_t kConformSpineRadiusFloor = 0x2000;
inline int32_t conform_spine_radius(int32_t b10, int32_t b11, int32_t b14,
		int32_t b15) {
	const int32_t a = ((b11 - b10) >> 1) - 0x4000;
	const int32_t b = ((b15 - b14) >> 1) - 0x1000;
	int32_t r = a < b ? a : b;
	if (r < kConformSpineRadiusFloor) r = kConformSpineRadiusFloor;
	return r;
}

// One wheel's oscillator state.
struct ConformOscillator {
	int32_t amplitude = 0; // o[0]
	int32_t extension = 0; // o[1]
	int32_t energy = 0;    // o[2]
	float phase = 0.0f;    // o[3]
};

// COMPRESSION step. Returns the step actually applied — zero once the wheel is
// already past its travel limit, which is how a bottomed suspension stops
// absorbing [orig: the > travel early-out @0x45CFE8 returning 0 @0x45D0A5].
//
// `impact` is decremented alongside, but only while POSITIVE: the energy sink
// is one-directional [orig: the > 0 gate @0x45D03A].
int32_t conform_spring_compress(ConformOscillator &osc, int32_t &compression,
		int32_t &impact, int32_t step, int32_t travel, int32_t spring);

// OSCILLATION step, run once the wheel is releasing. Returns the change in
// compression, which the caller consumes [orig: the subtraction @0x47EBC3].
//
// TWO DECAYS, and conflating them is the easy mistake: the amplitude decays by
// 0.99 EVERY tick [orig: @0x45D1D5], but the (11 - shock)/11 damping applies
// ONLY on the tick the wheel lands at zero compression
// [orig: the comp == 0 arm @0x45D1E5..]. Applying the damp every tick kills
// the bounce far too quickly.
int32_t conform_spring_oscillate(ConformOscillator &osc, int32_t &compression,
		int32_t &impact, int32_t shock, int32_t spring, bool a0_negative);

} // namespace opennova::world
