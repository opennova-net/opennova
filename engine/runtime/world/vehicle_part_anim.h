#pragma once

// VEHICLE PART ANIMATION — the accumulators that drive a model's PANM tracks
// (HELO_ROTOR, HELO_TAILROTOR, VEHICLE_WHEELS).
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
// The ground-motor speed and steer registers (+0x29C / +0x2B4) are the
// motor's own (world/vehicle_motor.cpp) and publish through
// PF_VEHICLE_STEERING / PF_VEHICLE_SPEED; this TU owns what those do not
// carry: the rotor spin machine and the wheel phase, published through
// PF_VEHICLE_ROTOR / PF_VEHICLE_TAIL_ROTOR / PF_VEHICLE_WHEELS.
//
// NAMED RESIDUAL (presentation, no D-row): the HELO machine's engine-start
// sound and the rotor-downwash leg that follow its spin arithmetic inside
// Entity_UpdateHeloRotorSpin @0x48FA70 — device work for the shell's audio/particle presenters.

#include <cstdint>

#include "world/entity.h"

namespace opennova::world {

class World;
struct VehicleTraits;

// ---------------------------------------------------------------------------
// THE TWO ROTOR MACHINES, split by the AI PROFILE TYPE (.aip +16) read
// through the brain the mover owns [orig: the class gate
//  `brain+4 -> profile+16 == 2` @0x4928C9..0x4928D1 selects the GROUND
//  machine Entity_UpdatePartSpinAccumulator @0x4928B0 — called from every
//  vehicle mover (@0x46F99E, @0x4700F5, @0x4869EA, @0x4889F5, @0x48AE3D,
//  @0x48D42B) and decaying 186413/tick; the HELO twin @0x48FA70 gates
//  `== 1`, decays 46603/tick and is called from the aircraft mover @0x4905A6].
// ---------------------------------------------------------------------------

using RotorState = Entity::VehicleMotorState::PartSpin;

enum class RotorMachine : uint8_t { None, Ground, Helo };

// The spin-up rates. A PLAYER-CONTROL item (ItemDef attrib 0x40) always seeds
// the full rate; every other item seeds one of three by a PRNG roll
// [orig: @0x4928E5..0x492935 — attrib & 0x40 with an occupant -> 186413
//  @0x4928F3; no 0x40 -> PRNG_Next16() % 100: > 66 -> 139809, > 33 -> 163110,
//  else 186413 @0x49290E..0x492935]. Spin-DOWN always uses the machine's
// decay [orig: ground @0x49294F = 186413; helo = 46603 inside @0x48FA70], so
// a player-control ground rotor takes as long to stop as to start while a
// rolled one may stop faster than it started, and a helicopter's rotor winds
// down four times slower than it spun up.
inline constexpr int32_t kRotorRateFull = 186413; // 0x2D82D
inline constexpr int32_t kRotorRateMid = 163110;  // 0x27D26
inline constexpr int32_t kRotorRateLow = 139809;  // 0x22221
inline constexpr int32_t kRotorSpeedMax = 0x0CCCCCC0; // 214748352
inline constexpr int32_t kRotorDecayGround = kRotorRateFull;
inline constexpr int32_t kRotorDecayHelo = 46603; // 0xB60B
// kItemAttribPlayerControl (items.def PlayerControl, 0x40) is declared once
// for the whole kItemAttrib* family in world/entity.h, included above.

// The profile type selects the machine: 2 (GROUND) / 1 (HELO); any other
// type (3 organic, 0 unresolved) runs neither.
inline RotorMachine rotor_machine_for_profile(int32_t profile_type) {
	if (profile_type == 2) return RotorMachine::Ground;
	if (profile_type == 1) return RotorMachine::Helo;
	return RotorMachine::None;
}

inline int32_t rotor_decay_for(RotorMachine machine) {
	return machine == RotorMachine::Helo ? kRotorDecayHelo : kRotorDecayGround;
}

// Whether THIS tick's seed needs a draw from the shared PRNG stream: only a
// zero rate on a non-player-control item rolls [orig: the `!rate` gate
// @0x4928DF and the `!(attrib & 0x40)` arm @0x492903]. A player-control item
// never rolls; an unoccupied non-player-control item rolls EVERY tick, because
// the unoccupied branch resets the rate to zero @0x492972.
inline bool rotor_rate_needs_roll(const RotorState &s, bool player_control) {
	return s.rate == 0 && !player_control;
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
inline void rotor_seed_rate(RotorState &s, bool player_control, bool occupied,
		int32_t rolled_rate) {
	if (s.rate != 0) return;
	if (player_control) {
		if (occupied) s.rate = kRotorRateFull; // @0x4928F3
	} else {
		s.rate = rolled_rate; // @0x492915 / @0x492930
	}
}

// One tick [orig: occupied @0x492984..0x4929A7: speed += rate, cap, angle +=
//  speed; unoccupied @0x492945..0x492972: speed -= decay floored at 0, angle
//  += speed, rate = 0]. `decay` is the machine's: 186413 ground, 46603 helo.
inline void rotor_tick(RotorState &s, bool occupied, int32_t decay = kRotorDecayGround) {
	if (occupied) {
		s.speed += s.rate;
		if (s.speed > kRotorSpeedMax) s.speed = kRotorSpeedMax;
		s.angle += s.speed;
		return;
	}
	if (s.speed != 0) {
		s.speed -= decay;
		if (s.speed < 0) s.speed = 0;
	}
	s.angle += s.speed;
	s.rate = 0;
}

// A spawn flag seeds the rotor at FULL speed rather than spinning it up
// [orig: Entity_SpawnFromBMSRecord @0x40E9F0, @0x40EE70..0x40EE94 — record flag
//  0x20000 (bms EngineRunning) sets entity Flags |= 0x80, rate = 0x2D82D,
//  speed = 0x0CCCCCC0 and the ground speed register +0x29C = 0x10000] — a
// helicopter spawned in flight has its rotor already turning. The +0x29C /
// Flags side effects belong to the spawner (mission/promote.cpp), not this
// state.
inline constexpr int32_t kSpawnRotorFullBit = 0x20000;
inline void rotor_spawn_full(RotorState &s) {
	s.rate = kRotorRateFull;
	s.speed = kRotorSpeedMax;
}

// The register the PANM feeder reads: the accumulator's HIGH WORD
// [orig: Entity_CacheVehicleHUDStats @0x4929B0 — the rotor angle's +0x466
//  @0x492ACA..0x492ADE for BOTH ordinals 46 and 47 (the tail rotor reads the
//  same accumulator @0x492AD7), the wheel phase's +0x2BA @0x4929B4; the same
//  MOVZX idiom as the steer word @0x4929C0..0x4929D7].
inline uint16_t part_register(int32_t accumulator) {
	return static_cast<uint16_t>((static_cast<uint32_t>(accumulator) >> 16) &
			0xFFFFu);
}

// ---------------------------------------------------------------------------
// WHEEL PHASE (+0x2B8) [orig: Entity_UpdateVehiclePhysics — `+0x2B8 +=
//  |+0x46C| + (+0x29C << 13)` @0x48C4C5..0x48C4D0 and the twin store
//  @0x48C4E4..0x48C4F4; the watercraft mover adds its brain forward command
//  instead, Entity_UpdateWatercraftPhysics @0x48E9F0..0x48E9F9 (`+0x220 << 13`)].
// ---------------------------------------------------------------------------

// Wheels advance by the speed scaled up, plus the slip kick |+0x46C| on an
// active skid. The slip register rides the terrain-contact skid vector the
// D-NET-161 level-frame re-derive does not carry — a peer integrates with
// slip 0; the wheelspin kick is that ledger row's, not something approximated.
inline int32_t wheel_phase_step(int32_t phase, int32_t speed, int32_t slip_abs) {
	return phase + slip_abs + (speed << 13);
}

// A watercraft's phase rides the brain's forward command instead.
inline int32_t watercraft_wheel_phase_step(int32_t phase, int32_t forward) {
	return phase + (forward << 13);
}

// ---------------------------------------------------------------------------
// The per-tick entry every family mover calls at its tail. Picks the rotor
// machine by the brain's profile type (a brainless row — a lib embedder's
// loose vehicle, a unit rig — has no profile, and its family stands in: the
// Helicopter/Plane movers are where retail calls the HELO twin from), runs it
// (seeding the rate from the shared PRNG when a non-player-control item needs
// a roll — the seed path is the ONLY PRNG consumer here, and it draws exactly
// once per unoccupied tick for such an item) and advances the wheel phase
// from the motor's own speed register. A WATERCRAFT runs no rotor machine at
// all — its mover Entity_UpdateWatercraftPhysics @0x48D480 calls neither
// @0x4928B0 nor @0x48FA70 — only the wheel phase.
// `occupied` is the engine-running latch, Entity::primary_occupant (the +0x170
// occupantEntity read @0x4928E8); `player_control` is the item's attrib 0x40.
void vehicle_part_anim_tick(World &world, Entity &veh, const VehicleTraits &traits);

} // namespace opennova::world
