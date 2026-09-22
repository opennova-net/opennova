#pragma once

#include <cstdint>

namespace opennova::world {

// THE GROUND-VEHICLE SUSPENSION SPRING LEG — the per-wheel compression step
// and free-decay oscillator the client-side conform runs for every items.def
// physics-1 vehicle, every tick, remote or local
// [orig: Entity_UpdateVehiclePhysics @0x48AF00 -> (unconditional @0x48D0B1)
//  Entity_ProcessTrackedVehiclePhysics @0x47C1C0, whose spring calls are
//  Suspension_CompressWheelQuadratic @0x47E2C1/@0x47EB68/@0x47EB7C and
//  Suspension_OscillateWheelFast @0x47E2D3/@0x47EBBB; the same pair serves
//  Entity_ProcessLightVehiclePhysics @0x479600 and
//  Entity_ProcessAircraftContactPhysics @0x47EF10].
//
// This header is the oscillator KERNEL; world/vehicle_suspension.cpp is the
// leg that calls it from the tracked (§7), wheeled (§8) and light (§9) contact
// solves of vehicle-client-movers-re.md over each tick's pad depths, owning the
// parked latch and the per-wheel state on Entity::VehicleMotorState. The
// wheeled (tank) family takes the linear compress and the slow oscillator
// [orig: Suspension_CompressWheelLinear @0x45CEB0; Suspension_OscillateWheel
// @0x45D240]. The probe radii those solves derive (`r = beam >> 2`, the spine
// floor 0x2000 @0x47C9E8) live THERE, not here.
//
// OWNERSHIP, witnessed at both conform exits: Pitch and Roll are written
// UNCONDITIONALLY; Yaw only when the parked latch (+0x2EC) is set
// [orig: @0x47E61F..0x47E632 / @0x47EC75..0x47EC8F]. A client holding the five
// state bytes at zero therefore takes Yaw from the wire and owns Pitch/Roll
// locally — the split matters, because owning Yaw too would fight the wire.
//
// Fixed point throughout, as retail: positions and heights 16.16, slope
// thresholds Q22, suspension travel 16.16 with full extension 0xFFFF.

// Suspension extension range [orig: dword_815180 = 0xFFFF — a .data constant
// with no writer in the image] and the per-step delta it is divided into
// [orig: >> 4 @0x47CBE5; the oscillator re-derives it as
//  dword_815180 >> 4 @0x45D203..0x45D20B].
inline constexpr int32_t kSuspFull = 0xFFFF;
inline constexpr int32_t kSuspStep = 0xFFF;

// The disable-rate pick (1.75 / 1.25 by role) and the tracked solve's
// spring dt (0.75 / 3.0 crashed) are the LEG's constants — they live with
// the latch machine in vehicle_suspension.h, not with the kernel.

// The wheel oscillator's constants [orig: Suspension_OscillateWheelFast
//  @0x45D110 (ex `Entity_ApplyDamageOscillationFast` — no health is touched;
//  the slow twin Suspension_OscillateWheel @0x45D240 steps 0.0872 rad/tick)
//  and its compressing-tick sibling Suspension_CompressWheelQuadratic
//  @0x45CFB0 (ex `sub_45CFB0`; the linear form is
//  Suspension_CompressWheelLinear @0x45CEB0). flt_7C6A10 @0x45D11F,
//  flt_7C6A04 @0x45D1C2, flt_7C6A00 @0x45D1CA, flt_7C69FC @0x45CFC8].
inline constexpr float kOscPhaseStep = 0.2616667f; // flt_7C6A10
inline constexpr float kOscDecayA = 0.09090909f;   // flt_7C6A04, = 1/11
inline constexpr float kOscDecayB = 0.99f;         // flt_7C6A00
// The phase preset a compressing wheel is forced to [orig: flt_7C69FC = 1.57
// stored @0x45CFC8..0x45CFD4].
inline constexpr float kOscPhasePreset = 1.57f;

// The entity+0xA0 threshold below which a landing is damped four times harder
// [orig: `cmp [esi+0A0h], -2000; jge skip; sar [edi], 2` @0x45D21F..0x45D22D].
inline constexpr int32_t kOscHardLandingBelow = -2000;

// Suspension travel from the def's spring_comp PERCENTAGE
// [orig: dword_815184 = ftol((100 - spring_comp) * 0xFFFF * flt_7C56A8 (0.01))
//  @0x47C51F..0x47C544; the bound the compress step tests is
//  dword_815180 - dword_815184 @0x45CFC2..0x45CFCE].
// A spring_comp of 100 gives full travel; 0 gives none.
inline int32_t conform_travel_from_def(int spring_comp) {
	if (spring_comp < 0) spring_comp = 0;
	if (spring_comp > 100) spring_comp = 100;
	const float t = static_cast<float>(kSuspFull) *
			static_cast<float>(100 - spring_comp) * 0.01f;
	return kSuspFull - static_cast<int32_t>(t);
}

// A slope threshold from a def BAM field, as Q22
// [orig: @0x47C54C..0x47C587 — cos(field * dbl_7C3608 (2pi / 2^32)) *
//  dbl_7C3600 (2^22); the slip threshold comes from slip_slope (+0x8F8) and
//  the max from max_slope (+0x8F4)]. Implemented in the caller's math so the
// cos is shared; this header states the scaling the result must carry.
inline constexpr float kConformThresholdScale = 4194304.0f; // 2^22

// One wheel's oscillator state — the 4-dword block Suspension_CompressWheelQuadratic and the
// oscillator share: +0 amplitude, +4 extension, +8 energy, +0x14 phase.
struct ConformOscillator {
	int32_t amplitude = 0; // o[0]
	int32_t extension = 0; // o[1]
	int32_t energy = 0;    // o[2]
	float phase = 0.0f;    // o[5]
};

// COMPRESSION step [orig: Suspension_CompressWheelQuadratic @0x45CFB0]. Returns the step actually
// applied — zero once the wheel is already past its travel limit, which is how
// a bottomed suspension stops absorbing [orig: the > travel early-out @0x45CFE8
// returning 0 @0x45D0A0..0x45D0A5].
//
// `impact` (entity+0x300) is decremented alongside, but only while POSITIVE:
// the energy sink is one-directional [orig: the > 0 gate @0x45D03A..0x45D042].
int32_t conform_spring_compress(ConformOscillator &osc, int32_t &compression,
		int32_t &impact, int32_t step, int32_t travel, int32_t spring);

// The tank's LINEAR compression step: the energy and impact sink drain by half
// the low-dword spring x step product instead of the quadratic term.
// [orig: Suspension_CompressWheelLinear @0x45CEB0]
int32_t conform_spring_compress_linear(ConformOscillator &osc, int32_t &compression,
		int32_t &impact, int32_t step, int32_t travel, int32_t spring);

// OSCILLATION step, run once the wheel is releasing. Returns the change in
// compression, which the caller consumes [orig: the subtraction @0x47EBC3].
//
// TWO DECAYS, and conflating them is the easy mistake: the amplitude decays by
// 0.99 EVERY tick [orig: @0x45D1C8..0x45D1D5], but the (11 - shock)/11 damping
// applies ONLY on the tick the wheel lands at zero compression
// [orig: the compression == 0 arm @0x45D1D7..0x45D1EE, multiplying the
//  ALREADY-decayed amplitude]. Applying the damp every tick kills the bounce
// far too quickly. `entity_a0` is the entity+0xA0 field the hard-landing test
// reads; `shock` is the def's field (+0x904), clamped to [0, 10] IN PLACE here
// exactly as retail writes the clamp back into the item def
// @0x45D18F..0x45D1A2 — pass the def's field, never a copy. The tank passes
// the slow phase step [orig: Suspension_OscillateWheel @0x45D240, flt_7C6A14].
int32_t conform_spring_oscillate(ConformOscillator &osc, int32_t &compression, int32_t &impact,
		int32_t &shock, int32_t spring, int32_t entity_a0, float phase_step = kOscPhaseStep);

} // namespace opennova::world
