#pragma once

// THE GROUND-VEHICLE SUSPENSION SPRING LEG — the per-wheel spring state the
// contact solves consume: the parked latch with its role-picked disable rate,
// the compressing / oscillating step over each wheel's probe depth, and the
// per-pad compression the probe points ride.
//
// Sibling TU of vehicle_contact_solve.cpp (the oversize-TU ratchet); the
// oscillator KERNEL is world/ground_conform.h (Suspension_CompressWheelQuadratic
// / Suspension_OscillateWheelFast), this file is what calls it and owns the
// latch + state bytes on Entity::VehicleMotorState.
// [orig: Entity_ProcessWheeledVehicleSuspension @0x46B140 — the latch pick
//  @0x46B1A6..0x46B213; Entity_ProcessTrackedVehiclePhysics @0x47C1C0 — the
//  spring dt pick @0x47C218..0x47C22B, the spring calls @0x47E2C1/@0x47E2D3
//  /@0x47EB68/@0x47EB7C/@0x47EBBB; vehicle-client-movers-re.md §7.3]

#include <cstdint>

#include "world/entity.h"

namespace opennova::world {

class World;
struct VehicleTraits;

// The parked latch and its one-shot disable-rate pick. When the mover's
// disable request (+0x2ED) and the latch (+0x2EC) are both clear, the solve
// picks the wheel-solver disable-rate multiplier BY ROLE — 1.25 on the
// authority, 1.75 off it — into the entity, zeroes +0x2EF, and sets the latch;
// the authority also raises entity Flags 0x10 while a client merely READS it
// (the latch is replicated through that bit), then +0x2EE clears and the
// suspension state is reset [orig: gate @0x46B1A6..0x46B1B9, the pick
//  @0x46B1C5..0x46B1DB (flt_7C6F14 = 1.75 / flt_7C6F18 = 1.25 by
//  g_napi_np_ctx.is_authority), +0x2EF = 0, Flags |= 0x10 @0x46B1ED on the
//  authority vs the `test Flags, 0x10` @0x46B1F3 on a client, +0x2EC = 1
//  @0x46B1F9, +0x2EE = 0, Entity_ClearSuspensionState @0x4592B0].
// Returns true when the latch set this tick.
//
// WITNESS PENDING — the leg is wired but ARMED OFF: read literally, the gate
// `+0x2ED == 0 && +0x2EC == 0` fires on a fresh row's first tick, which would
// park every vehicle at spawn and drop a landing bike by one parked step —
// contradicting the witnessed mover. The +0x2ED disable-request producer
// @0x48168D, the +0x2EC clears @0x480831/@0x481544/@0x48177C and any third
// gate term settle it. Until they land, `suspension_leg_armed` stays false:
// the latch never sets and the step runs with the unparked dt over
// zero-state sinks exactly as before (the pad offsets stay 0 because the
// compress arm is what grows them and it is part of the same armed leg).
inline constexpr bool kSuspensionLegArmed = false;
bool vehicle_suspension_latch(World &world, Entity &veh);

// The spring dt the step integrates with: 0.75 while the latch is clear, 3.0
// once parked — a parked vehicle settles its springs four times faster
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C218..0x47C22B — flt_7C3DC8
//  @0x47C222 / flt_7C6F80 @0x47C21A].
float vehicle_suspension_dt(const Entity::VehicleMotorState &m);

// One tick of the four wheel springs over the solve's per-pad probe depths:
// a wheel whose pad penetrates (d > 0) COMPRESSES by the dt step; a wheel in
// the air OSCILLATES (free decay). Each wheel's compression is what the next
// tick's pad probe point rides (+0x2D4 + 4k added to the pad body-frame Z).
// Returns nothing; the state lives on `m`.
// [orig: the per-wheel spring loop — compressing arm
//  Suspension_CompressWheelQuadratic @0x47E2C1 / @0x47EB68 / @0x47EB7C,
//  releasing arm Suspension_OscillateWheelFast @0x47E2D3 / @0x47EBBB]
void vehicle_suspension_step(World &world, Entity &veh, const VehicleTraits &traits,
                             const int32_t pad_depths[4]);

// Reset every per-wheel spring field [orig: Entity_ClearSuspensionState
//  @0x4592B0 — called at the latch edge @0x46B20E]. WITNESS PENDING: the
// exact field set it zeroes; until then every field this TU owns resets.
void vehicle_suspension_clear(Entity::VehicleMotorState &m);

} // namespace opennova::world
