#pragma once

#include <cstdint>

namespace opennova::world {

// VEHICLE PART ANIMATION REGISTERS — the accumulators that drive a model's
// PANM tracks (HELO_ROTOR, HELO_TAILROTOR, VEHICLE_WHEELS).
//
// The model reads each track's register as the HIGH WORD of a dword
// accumulator, so what these integrate is the full 32-bit value and the
// animation samples the top half. Integrating at word width instead would
// quantise the motion visibly.
//
// The client runs this for EVERY vehicle it is not driving, seeding the drive
// command straight from the wire [orig: Entity_UpdateVehiclePhysics @0x48AF00
//  — the ctrl_forward and steer-target copies @0x48B7F0..0x48B80A]. So a peer
// integrating the same wire fields IS the retail mechanism, not a stand-in.

// ---------------------------------------------------------------------------
// ROTOR [orig: RotorSpin_Update @0x4928B0]
// ---------------------------------------------------------------------------

// Player-control items (ItemDef attrib 0x40) use a FIXED acceleration and the
// same magnitude for spin-down, so a rotor takes as long to stop as to start.
inline constexpr int32_t kRotorAccel = 0x2D82D;
inline constexpr int32_t kRotorSpeedMax = 0x0CCCCCC0;
// A spawn flag seeds the rotor at FULL speed rather than spinning it up
// [orig: Entity_InitFromBmsOrNetSpawn @0x40EE70..0x40EE9A] — a helicopter
// spawned in flight has its rotor already turning.
inline constexpr int32_t kSpawnRotorFullBit = 0x20000;
// The ItemDef attribute that marks a player-controllable vehicle.
inline constexpr int32_t kItemAttribPlayerControl = 0x40;

// One tick of rotor speed. Occupied spins up to the cap; unoccupied decays by
// the SAME step down to zero.
inline int32_t rotor_step(int32_t speed, bool occupied) {
	if (occupied) {
		speed += kRotorAccel;
		if (speed > kRotorSpeedMax) speed = kRotorSpeedMax;
	} else {
		speed -= kRotorAccel;
		if (speed < 0) speed = 0;
	}
	return speed;
}

// The register the PANM feeder reads: the accumulator's HIGH WORD
// [orig: the +0x466 read @0x492ACA, +0x2BA @0x4929B4].
inline uint16_t part_register(int32_t accumulator) {
	return static_cast<uint16_t>((static_cast<uint32_t>(accumulator) >> 16) &
			0xFFFFu);
}

// ---------------------------------------------------------------------------
// GROUND MOTOR SPEED [orig: @0x48C1C6..0x48C32A]
// ---------------------------------------------------------------------------

// Below this magnitude a coasting vehicle SNAPS to a stop rather than creeping
// [orig: @0x48C314]. Without it a vehicle drifts forever at sub-pixel speed.
inline constexpr int32_t kGroundSpeedSnap = 48;

// The commanded target, reduced by the hull's pitch: a vehicle facing up a
// slope cannot command its full speed [orig: (cmd * cos^2(pitch)Q22) >> 22
//  @0x48C1C6..0x48C1F6].
inline int32_t ground_speed_target(int32_t cmd, int32_t cos2_pitch_q22) {
	return static_cast<int32_t>(
			(static_cast<int64_t>(cmd) * cos2_pitch_q22) >> 22);
}

// The raw acceleration toward the target, rounded [orig: (target - speed + 16)
//  >> 5 @0x48C1FE..0x48C20B].
inline int32_t ground_speed_accel(int32_t target, int32_t speed) {
	return (target - speed + 16) >> 5;
}

// THE CLAMP DEPENDS ON WHAT THE DRIVER IS DOING, and the three cases are not
// interchangeable:
//   * a REVERSAL — target and speed on opposite sides of zero — integrates
//     UNCLAMPED, so throwing a vehicle into reverse bites immediately
//     [orig: @0x48C3D2..0x48C3EC -> @0x48C2FC];
//   * a same-sign drive clamps to the def's acceleration [orig: @0x48C3F9];
//   * a ZERO target clamps to the def's DECELERATION, which is a different
//     field [orig: @0x48C442].
// Collapsing these into one clamp makes reverse feel mushy and braking wrong.
inline int32_t ground_speed_clamp(int32_t accel, int32_t target, int32_t speed,
		int32_t accel_limit, int32_t decel_limit) {
	const bool reversal = (target > 0 && speed < 0) || (target < 0 && speed > 0);
	if (reversal) return accel; // unclamped
	const int32_t limit = target == 0 ? decel_limit : accel_limit;
	if (accel > limit) return limit;
	if (accel < -limit) return -limit;
	return accel;
}

// One tick of motor speed.
inline int32_t ground_speed_step(int32_t speed, int32_t target,
		int32_t accel_limit, int32_t decel_limit) {
	int32_t accel = ground_speed_accel(target, speed);
	accel = ground_speed_clamp(accel, target, speed, accel_limit, decel_limit);
	// A zero acceleration SNAPS to the target rather than stalling short of it
	// [orig: @0x48C2FC..0x48C32A].
	if (accel == 0) return target;
	speed += accel;
	// The creep snap applies only when coasting to a stop.
	if (target == 0) {
		const int32_t mag = speed < 0 ? -speed : speed;
		if (mag < kGroundSpeedSnap) speed = 0;
	}
	return speed;
}

// ---------------------------------------------------------------------------
// STEER [orig: @0x48C0C5..0x48C14E]
// ---------------------------------------------------------------------------

// The effective turn rate falls off with speed: a vehicle at its top speed
// turns at `min_rate`, one at rest at the full `turn_rate`.
//
// `min_rate` is turn_rate2 when the def gives one, else a QUARTER of turn_rate.
inline int32_t steer_min_rate(int32_t turn_rate, int32_t turn_rate2) {
	return turn_rate2 != 0 ? turn_rate2 : (turn_rate >> 2);
}

// The speed falloff factor, Q16: 0x10000 at rest, 0 at player_speed.
inline int32_t steer_speed_factor(int32_t speed, int32_t player_speed) {
	if (player_speed == 0) return 0x10000;
	const int32_t f = 0x10000 - static_cast<int32_t>(
			(static_cast<int64_t>(speed) << 16) / player_speed);
	return f < 0 ? 0 : f;
}

// The effective rate, interpolated between min and full by that factor.
inline int32_t steer_effective_rate(int32_t turn_rate, int32_t min_rate,
		int32_t factor_q16) {
	const int64_t span = static_cast<int64_t>(turn_rate - min_rate) * factor_q16;
	return static_cast<int32_t>((span + 0x8000) >> 16) + min_rate;
}

// The steer register is the HIGH WORD of a SIGNED dword and is left UNCLAMPED
// [orig: @0x4929C0 — the 0x10000 minimum there is dead code]. Clamping it
// would flatten the extremes of a hard turn.

// ---------------------------------------------------------------------------
// WHEEL PHASE [orig: @0x48C4D0 / @0x48C4F4; the air form @0x48E9F0]
// ---------------------------------------------------------------------------

// Wheels advance by the speed scaled up, plus a slip kick on an active skid.
// The slip term rides the terrain-contact skid vector, which no wire field
// carries — a peer integrates with slip 0, and the wheelspin kick is a
// RECORDED GAP rather than something approximated.
inline int32_t wheel_phase_step(int32_t phase, int32_t speed, int32_t slip_abs) {
	return phase + slip_abs + (speed << 13);
}

// An aircraft's wheels ride the brain's forward command instead
// [orig: Entity_UpdateAircraftPhysics @0x48E9F0..0x48E9F9].
inline int32_t air_wheel_phase_step(int32_t phase, int32_t forward) {
	return phase + (forward << 13);
}

} // namespace opennova::world
