#pragma once

#include <cstdint>

namespace opennova::world {

// The client-side guided-missile (Stinger `stng`) flight integrator — the
// per-tick pursuit a NON-AUTHORITY client runs to fly a guided missile it did
// not launch, steering purely from the server-fed steer point (folded off the
// S2C 0x44 guidance channel; net-re §5.15). Host-neutral pure math.
//
// [orig: Entity_UpdateGuidedMissile_0 @0x446060 (the stng move_function, class
//  table @0x82AD24) — non-authority branch; sub_546B30 @0x546B30 pursuit
//  error; Entity_UpdateTurretAim @0x445CC0 turn-clamp + velocity build;
//  position advance = the generic entity-move step (velocity +0x98..0xA0 added
//  to Position +0x04..0x0C, witnessed in every sibling motor, e.g.
//  Entity_UpdateProjectilePhysics @0x444aed)]
//
// The witnessed model: a proportional-pursuit chase with a HARD per-axis
// turn-rate cap toward the steer point, an 8-tick boost ramp, NO gravity and
// NO drag (the stng motor omits both — unlike hlfr/jvln). Angles are 32-bit
// BAM; positions/velocity 16.16 mission units (x, y, z-up); tickrate 62.
//
// The AUTHORITY branch (the launching host's seeker: target acquisition,
// flare preference, the 0x44 write side) is NOT modelled here — it remains
// the round_sim guided-callback gap (D-NET-64 residual).
struct GuidedFlightState {
	int32_t pos[3] = { 0, 0, 0 };  // 16.16 mission x, y, z-up
	int32_t yaw_bam = 0;           // 32-bit BAM
	int32_t pitch_bam = 0;
	int32_t age = 0;               // 62 Hz ticks since launch
};

enum class GuidedDetonate : uint8_t {
	kNone = 0,
	kCoincident,  // steer point coincides with the missile position
	kOvershoot,   // |pursuit error| > 90 deg [orig: @0x44646c]
	kGuard,       // steer distance < 0.5 units [orig: @0x4464b7]
	kProximity,   // within 400.0 units of the steer point [orig: @0x44657f]
};

class GuidedFlight {
public:
	// [orig: cmp 0x1f @0x446535] — no motion until age >= 31 (ignition hold).
	static constexpr int32_t kAgeGate = 31;
	// [orig: @0x446641] — boost divisor = 39 - age, clamped >= 1.
	static constexpr int32_t kBoostFullAge = 39;
	// [orig: @0x44655c] — the /62 tickrate magic.
	static constexpr int32_t kTickDiv = 62;
	// Turn clamp default (~0.56 deg/tick) [orig: the field<=0 fallback].
	static constexpr int32_t kTurnDefault = 6734910;
	// [orig: @0x44646c] — |error| > 90 deg -> detonate.
	static constexpr int32_t kOvershoot = 0x3FFFFFC0;
	// [orig: @0x4464b7] — steer distance < 0.5 (16.16) -> detonate.
	static constexpr int32_t kSteerGuard = 0x8000;
	// [orig: @0x44657f] — aim distance < 400.0 (16.16) -> detonate.
	static constexpr int32_t kProximity = 0x1900000;

	// One 62 Hz tick. `steer` is the current steer point (16.16 mission);
	// `velocity` the ammo.def velocity (units/s int); the turn clamps the
	// ammo.def turnrate_maxpit/maxyaw in BAM/tick (<=0 -> kTurnDefault).
	// Advances `st` in place; returns the detonation verdict. Proximity
	// detonates AFTER the tick's position advance; overshoot/guard BEFORE it
	// [orig: the test order inside Entity_UpdateGuidedMissile_0 @0x446060].
	static GuidedDetonate step(GuidedFlightState &st, const int32_t steer[3],
	                           int32_t velocity, int32_t turn_max_pit,
	                           int32_t turn_max_yaw);

	// Seed the orientation aimed at the first steer point (the fork-witnessed
	// launch pose: the integrator starts pointed at the initial lock).
	static void aim_at(GuidedFlightState &st, const int32_t steer[3]);

	// Signed 32-bit BAM short-way wrap.
	static int32_t wrap_bam(int64_t v);
};

} // namespace opennova::world
