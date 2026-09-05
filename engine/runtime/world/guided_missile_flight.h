#pragma once

#include <cstdint>
#include <base/io/tick_rate.h>

namespace opennova::world {

// The guided-missile (Stinger `stng`) flight integrator — the per-tick pursuit
// step of the stng move_function, shared by every role. A NON-AUTHORITY client
// runs it to fly a missile it did not launch, steering purely from the
// server-fed steer point (folded off the S2C 0x44 guidance channel; net-re
// §5.15); the AUTHORITY (SP / the game host) additionally runs the termination
// tests and broadcasts their outcome as group 1. Host-neutral pure math.
//
// [orig: Entity_UpdateGuidedMissile_0 @0x446060 (the stng move_function, class
//  table @0x82AD24); Entity_ComputeGuidedPursuitError @0x546B30 -> compute_relative_position_metrics
//  @0x545710 pursuit error (elevation/heading BAM + horizontal/total distance);
//  Entity_UpdateTurretAim @0x445CC0 turn-clamp + velocity build; position
//  advance = the generic entity-move step (velocity +0x98..0xA0 added to
//  Position +0x04..0x0C, witnessed in every sibling motor, e.g.
//  Entity_UpdateProjectilePhysics @0x444aed)]
//
// The witnessed model: a proportional-pursuit chase with a HARD per-axis
// turn-rate cap toward the steer point, an 8-tick boost ramp (ages 31..38),
// NO gravity and NO drag (the stng motor omits both — unlike hlfr/jvln).
// Angles are 32-bit BAM; positions/velocity 16.16 mission units (x, y, z-up);
// tickrate 62.
//
// Role split [orig: the `!is_in_session || is_authority` gate @0x4463cb]: the
// overshoot mark, the steer-distance guard, and the proximity AI-notify are
// AUTHORITY-ONLY. A non-authority client computes the error and advances but
// never self-terminates — the wire's group 1 (detonate bits entity+696|=1,
// +276|=0x1000) is what ends its flight.
//
// Known approximation: the original measures the pursuit error in the
// missile's LOCAL frame (fixed-point matrix transform + BAM atan2 tables);
// this port derives the same two angles from world-frame deltas against the
// current heading — identical at roll 0 to first order, diverging only for
// simultaneously large elevation+heading errors (beyond the clamp's reach in
// practice). The AUTHORITY seeker branch (target acquisition cadence, flare
// preference, tracking-range break -> group 2, the 0x44 write side) is NOT
// modelled here — it remains the D-NET-64 residual.
struct GuidedFlightState {
	int32_t pos[3] = { 0, 0, 0 };  // 16.16 mission x, y, z-up
	int32_t yaw_bam = 0;           // 32-bit BAM heading
	int32_t pitch_bam = 0;         // 32-bit BAM elevation
	int32_t age = 0;               // 62 Hz ticks since launch
};

enum class GuidedStepResult : uint8_t {
	kNone = 0,
	// Numeric guard only (steer point coincides with the missile position);
	// not a witnessed original branch — the authority guard at 0.5 u subsumes
	// it in retail geometry.
	kCoincident,
	// AUTHORITY verdicts [orig: @0x446484 / @0x4464c2 set the detonate bits
	// entity+696|=1, +276|=0x1000, then broadcast group 1]:
	kOvershoot,       // |pursuit error| > ~90 deg — marked, tick STILL advances
	kGuard,           // total steer distance < 0.5 u — returns BEFORE the advance
	// AUTHORITY notify [orig: @0x446587 -> AIEvent_QueueEntry(12)]: steer point
	// within 400 u horizontal. NOT a detonation — the evade/flare AI trigger;
	// the missile keeps flying.
	kProximityNotify,
};

class GuidedFlight {
public:
	// [orig: cmp 31 @0x446538/@0x44663d] — no advance until age >= 31
	// (ignition hold); the error/termination leg is NOT age-gated.
	static constexpr int32_t kAgeGate = 31;
	// [orig: 39-age divisor @0x446543/@0x446644, clamped >= 1 @0x446548].
	static constexpr int32_t kBoostFullAge = 39;
	// [orig: the /0x3E in the speed build @0x446572/@0x446677].
	static constexpr int32_t kTickDiv = io::kTicksPerSecondInt;
	// Turn clamp default (~0.56 deg/tick) [orig: Entity_UpdateTurretAim
	//  @0x445ccd/@0x445cd1 — the def-field<=0 fallback].
	static constexpr int32_t kTurnDefault = 6734910;
	// [orig: @0x446484] — |error| > 0x3FFFFFC0 (a hair under 90 deg) marks the
	// detonate bits; the tick still advances (the overshoot block falls
	// through to the advance).
	static constexpr int32_t kOvershoot = 0x3FFFFFC0;
	// [orig: @0x4464b7] — TOTAL steer distance < 0.5 (16.16) -> detonate,
	// before the advance.
	static constexpr int32_t kSteerGuard = 0x8000;
	// [orig: @0x446587] — HORIZONTAL steer distance < 400.0 (16.16) -> queue
	// AIEvent 12 (evade notify), keep flying.
	static constexpr int32_t kProximity = 0x1900000;

	// One 62 Hz tick. `steer` is the current steer point (16.16 mission);
	// `velocity` the ammo.def velocity (units/s int); the turn clamps the
	// ammo.def turnrate_maxpit/maxyaw in BAM/tick (<=0 -> kTurnDefault).
	// `authority` selects the witnessed role split above: only the authority
	// evaluates overshoot/guard/proximity; a non-authority client only flies.
	// Advances `st` in place; returns the step verdict.
	static GuidedStepResult step(GuidedFlightState &st, const int32_t steer[3],
	                             int32_t velocity, int32_t turn_max_pit,
	                             int32_t turn_max_yaw, bool authority);

	// Seed the orientation aimed at the first steer point (the fork-witnessed
	// launch pose stand-in: the integrator starts pointed at the initial lock;
	// the exact retail launch transform rides the spawn/presentation residual).
	static void aim_at(GuidedFlightState &st, const int32_t steer[3]);

	// Signed 32-bit BAM short-way wrap.
	static int32_t wrap_bam(int64_t v);
};

} // namespace opennova::world
