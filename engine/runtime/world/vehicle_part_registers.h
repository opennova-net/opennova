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
//  — `if (driver != g_local_player_entity)` @0x48B7F0, ctrl_forward (+0x220)
//  from wire +0x2C4 and the steer target (+0x210) from wire +0x2CC
//  @0x48B7F8..0x48B80A]. So a peer integrating the same wire fields IS the
// retail mechanism, not a stand-in.
//
// STAGED, NOT WIRED: the ground-motor speed and steer registers (+0x29C /
// +0x2B4) are LIVE in world/vehicle_motor.cpp (the clamp tree, the < 48 stop
// snap, the speed-dependent steer rate), and Entity_CacheVehicleHUDStats
// @0x4929B0's steering/speed publication is the present-row
// PF_VEHICLE_STEERING / PF_VEHICLE_SPEED pair. What lives here is what those
// do not carry: the rotor spin machine and the wheel phase.

// ---------------------------------------------------------------------------
// ROTOR [orig: Entity_UpdatePartSpinAccumulator @0x4928B0 — called from every
//  vehicle mover (@0x46F99E, @0x4700F5, @0x4869EA, @0x4889F5, @0x48AE3D,
//  @0x48D42B) and gated on the move-context class 2 @0x4928C9..0x4928D1].
// ---------------------------------------------------------------------------

// The three dwords the machine owns: +0x460 speed, +0x464 the angle
// accumulator the PANM register samples, +0x468 the spin-up rate.
struct RotorState {
	int32_t speed = 0; // +0x460
	int32_t angle = 0; // +0x464
	int32_t rate = 0;  // +0x468
};

// The spin-up rates. A PLAYER-CONTROL item (ItemDef attrib 0x40) always seeds
// the full rate; every other item seeds one of three by a PRNG roll
// [orig: @0x4928E5..0x492935 — attrib & 0x40 with an occupant -> 186413
//  @0x4928F3; no 0x40 -> PRNG_Next16() % 100: > 66 -> 139809, > 33 -> 163110,
//  else 186413 @0x49290E..0x492935]. Spin-DOWN always uses the full rate
// [orig: @0x49294F], so a player-control rotor takes as long to stop as to
// start while a rolled one may stop faster than it started.
inline constexpr int32_t kRotorRateFull = 186413; // 0x2D82D
inline constexpr int32_t kRotorRateMid = 163110;  // 0x27D26
inline constexpr int32_t kRotorRateLow = 139809;  // 0x22221
inline constexpr int32_t kRotorSpeedMax = 0x0CCCCCC0; // 214748352
inline constexpr int32_t kItemAttribPlayerControl = 0x40;

// Whether THIS tick's seed needs a draw from the shared PRNG stream: only a
// zero rate on a non-player-control item rolls [orig: the `!rate` gate
// @0x4928DF and the `!(attrib & 0x40)` arm @0x492903]. A player-control item
// never rolls; an unoccupied non-player-control item rolls EVERY tick, because
// the unoccupied branch resets the rate to zero @0x492972.
inline bool rotor_rate_needs_roll(const RotorState &s, int32_t attrib) {
	return s.rate == 0 && (attrib & kItemAttribPlayerControl) == 0;
}

// The rolled rate from a PRNG word [orig: @0x49290E..0x492935].
inline int32_t rotor_rate_from_roll(uint16_t prng_word) {
	const int pct = static_cast<int>(prng_word % 100u);
	if (pct > 66) return kRotorRateLow;
	if (pct > 33) return kRotorRateMid;
	return kRotorRateFull;
}

// Seed the rate when it is zero. `rolled_rate` is rotor_rate_from_roll's
// result when rotor_rate_needs_roll said so, else ignored.
inline void rotor_seed_rate(RotorState &s, int32_t attrib, bool occupied,
		int32_t rolled_rate) {
	if (s.rate != 0) return;
	if ((attrib & kItemAttribPlayerControl) != 0) {
		if (occupied) s.rate = kRotorRateFull; // @0x4928F3
	} else {
		s.rate = rolled_rate; // @0x492915 / @0x492930
	}
}

// One tick [orig: occupied @0x492984..0x4929A7: speed += rate, cap, angle +=
//  speed; unoccupied @0x492945..0x492972: speed -= 186413 floored at 0, angle
//  += speed, rate = 0].
inline void rotor_tick(RotorState &s, bool occupied) {
	if (occupied) {
		s.speed += s.rate;
		if (s.speed > kRotorSpeedMax) s.speed = kRotorSpeedMax;
		s.angle += s.speed;
		return;
	}
	if (s.speed != 0) {
		s.speed -= kRotorRateFull;
		if (s.speed < 0) s.speed = 0;
	}
	s.angle += s.speed;
	s.rate = 0;
}

// A spawn flag seeds the rotor at FULL speed rather than spinning it up
// [orig: Entity_SpawnFromBMSRecord @0x40E9F0, @0x40EE70..0x40EE94 — record flag
//  0x20000 sets entity Flags |= 0x80, rate = 0x2D82D, speed = 0x0CCCCCC0 and
//  the ground speed register +0x29C = 0x10000] — a helicopter spawned in
// flight has its rotor already turning. The +0x29C / Flags side effects
// belong to the spawner, not this state.
inline constexpr int32_t kSpawnRotorFullBit = 0x20000;
inline void rotor_spawn_full(RotorState &s) {
	s.rate = kRotorRateFull;
	s.speed = kRotorSpeedMax;
}

// The register the PANM feeder reads: the accumulator's HIGH WORD
// [orig: Entity_CacheVehicleHUDStats @0x4929B0 — the rotor angle's +0x466
//  @0x492ACA..0x492ADE, the wheel phase's +0x2BA @0x4929B4].
inline uint16_t part_register(int32_t accumulator) {
	return static_cast<uint16_t>((static_cast<uint32_t>(accumulator) >> 16) &
			0xFFFFu);
}

// ---------------------------------------------------------------------------
// WHEEL PHASE (+0x2B8) [orig: Entity_UpdateVehiclePhysics @0x48C4C0..0x48C4D0
//  (|slip| + (speed << 13)) and the +0x46C form @0x48C4D8..0x48C4F4; the
//  watercraft mover adds its brain forward command instead,
//  Entity_UpdateWatercraftPhysics @0x48E9F0..0x48E9F9 (`+0x220 << 13`)].
// ---------------------------------------------------------------------------

// Wheels advance by the speed scaled up, plus a slip kick on an active skid.
// The slip term rides the terrain-contact skid vector, which no wire field
// carries — a peer integrates with slip 0, and the wheelspin kick is a
// RECORDED GAP rather than something approximated.
inline int32_t wheel_phase_step(int32_t phase, int32_t speed, int32_t slip_abs) {
	return phase + slip_abs + (speed << 13);
}

// A watercraft's phase rides the brain's forward command instead.
inline int32_t watercraft_wheel_phase_step(int32_t phase, int32_t forward) {
	return phase + (forward << 13);
}

} // namespace opennova::world
