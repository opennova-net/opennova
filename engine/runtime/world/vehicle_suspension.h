#pragma once

// THE GROUND-VEHICLE SUSPENSION SPRING LEG — the per-wheel free-fall sinks,
// the spring-energy loop that compresses and releases each wheel, the
// per-tick crash request the family physics raises, and the crash latch it
// arms. Sibling TU of vehicle_contact_solve.cpp (the oversize-TU ratchet); the
// oscillator KERNEL is world/ground_conform.h, this file is everything around
// it and owns the state bytes on Entity::VehicleMotorState.
//
// THE STATE, by retail offset [orig: Entity_RespawnVehicle @0x45FF40 writes the
//  family @0x45ffeb..0x46001e; vehicle-client-movers-re.md §7.3]:
//   +0x2C4..+0x2D0  sink_k      the per-pad free-fall accumulators (grow while a
//                               pad is off the ground; VehicleMotorState::plat_acc —
//                               the boat platform solve's corner drops are the SAME
//                               four dwords)
//   +0x2D4..+0x2E0  comp_k      the wheel compression the pad probe rides
//   +0x304+0x18k    osc_k       amplitude / extension / energy(force) / phase
//   +0x300          spring_energy  the impact sink (+1.25·F on an impulse, drained
//                               by every compress step)
//   +0x2EC  crashed             the CRASHED / TIPPED state — the bike version ejects
//                               its rider, the tank/tracked versions play the crash
//                               sound at their own `+0x2EC = 1` sites
//                               [orig: @0x478998 / @0x47e47b / @0x468b3b]
//   +0x2ED  crash_request       a PER-TICK bool the family physics raises mid-tick and
//                               every family tail clears unconditionally
//                               [orig: tank @0x4795da, bike @0x47c0b6, tracked @0x47eeee]
//   +0x2EE  landing_2ee         the hard-landing marker the catch-up sets
//   +0x2EF  byte_2ef            zeroed at arming, set with the crash sound
//   +0x2F0  settle_2f0          the wreck/settle latch (gates the airborne spring loop)
//   +0x2F1  fresh_2f1           1 after Entity_RespawnVehicle, 0 after a BMS spawn (memset),
//                               cleared by the crash tests, re-raised by the client window
//   +0x2F2  settled_2f2         the sleep path's "settled upright" byte
//   +0x2F8  airborne_stamp_2f8  the client crash window's airborne tick stamp
//   +0x2FC  wreck_2fc           wreck-settled / bike fall-over latch; the light
//                               chassis helper uses +0x460 as its angular rate
//   +0x3DE  wheelie_active      the bike's retained launch mode
//
// THE TICK, in the witnessed order inside each family contact solve:
//   1. crash tests   — raise crash_request under the family's conditions
//   2. sink growth   — `!contact_k && !crash_request && !crashed` → sink_k += growth
//   3. spring loop   — grounded: the impulse / settle / compress / oscillate loop
//                      whose resolved penetration lifts the corner quad; airborne:
//                      the energy-driven compress + free decay, gated on !settle_2f0
//   4. arming        — `crash_request && !crashed` → the role pick (1.25 authority /
//                      1.75 client), Flags 0x10 (authority sets, a client arms only
//                      if the bit arrived), crashed = 1
//   5. post-contact  — all pads back in contact → sinks reset
//   6. tail          — crash_request = 0; the amplitude/energy tail
//
// Crash impulses, chassis reset, recovery, the tank spring pair and bike
// wheelie/ejection gates are implemented in vehicle_chassis.cpp,
// vehicle_suspension.cpp and vehicle_contact_solve.cpp.
// Witness sites: [orig: @0x45FF40, @0x45ffeb, @0x46001e, @0x478998, @0x47e47b, @0x468b3b,
// @0x4795da, @0x47c0b6, @0x47eeee, @0x46b22e, @0x46b30b, @0x463a3b, @0x463a64, @0x4592B0,
// @0x47606e, @0x479437, @0x47ee3b, @0x47ee49, @0x47ed67, @0x47ed73, @0x47ed82, @0x47ee2f,
// @0x47eea0, @0x47eebc, @0x47c3a0, @0x47c3c6, @0x45CEB0, @0x45D240, @0x47b14c, @0x47b6a7,
// @0x47b6d1, @0x47b6fb, @0x47b375, @0x47b431, @0x47bad0, @0x4859e0, @0x47b63f]

#include <cstdint>

#include <runtime/world/entity.h>

namespace opennova::world {

class World;
struct VehicleTraits;

// --- constants -----------------------------------------------------------

// The wheel-solver DISABLE-rate multiplier picked ONCE at arming by role
// [orig: flt_7C6F18 = 1.25 on the authority @0x46b1cd, flt_7C6F14 = 1.75 off
//  it @0x46b1d5]. Stored for the (residual) impulse dump.
inline constexpr float kSuspensionDisableRateAuthority = 1.25f;
inline constexpr float kSuspensionDisableRateNonAuthority = 1.75f;

// The tracked solve's spring dt: 0.75 normal, 3.0 CRASHED [orig: flt_7C3DC8
//  @0x47c222 / flt_7C6F80 @0x47c21a, selected on +0x2EC @0x47c1de..0x47c218].
// It scales the sink growth (ftol(dt·250)); the tank and the bike have no
// select (their growth is the literal 250 / 100).
inline constexpr float kSuspensionDtNormal = 0.75f;
inline constexpr float kSuspensionDtCrashed = 3.0f;
inline constexpr int32_t kSinkGrowthPerTick = 250; // flt_7C6F7C
inline constexpr int32_t kSinkGrowthTank = 250;    // [orig: @0x478510..0x47852b]
inline constexpr int32_t kSinkGrowthBike = 100;    // [orig: @0x47ab36..0x47abdc]

// The replicated crash/park bit: the authority raises it at arming, a client
// arms only when it arrived [orig: Flags |= 0x10 @0x46b1ed / test @0x46b1f3].
inline constexpr uint32_t kEntityFlagSuspensionCrashed = 0x10u;

// Crash-test thresholds [orig: tracked @0x47d745..0x47d7a8 / tank
//  @0x477760..0x4777bf]: the flip angle is the def's `flip` percent of 1.0 in
//  Q16 (flt_7C56A8 = 0.01 × flt_7C32BC = 65536.0); the authority's hard-fall
//  test reads |slide_z| against 0x7000.
inline constexpr int32_t kCrashFallVzAbove = 0x7000;
inline constexpr uint32_t kClientCrashWindowTicks = 10; // [orig: @0x478bc0 / @0x47e7d8]

// The two families that run the crash tests differ in two terms: the TRACKED
// tip test (a) also fires on the replicated bit [orig: `test al, 10h`
//  @0x47d749 — absent from the tank's @0x477760..0x477776], and the TANK
// client window (c) also requires settle_2f0 == 0 [orig: `cmp [esi+2F0h], 0`
//  @0x478b8a — absent from the tracked @0x47e793..0x47e7a8].
enum class SuspensionFamily : uint8_t { Tracked, Tank };

// The spring loop's literals [orig: the grounded loop @0x47E960..0x47EC1F]:
// the compress step cap (dword_815180 >> 4), the impulse threshold select on
// spring_comp, the hard-landing catch-up bound, and the oscillate ceiling on
// the sinks.
inline constexpr int32_t kSpringStepCap = 4095;
inline constexpr int32_t kImpulseThresholdSoft = 1000; // spring_comp <= 10
inline constexpr int32_t kImpulseThresholdHard = 5000;
inline constexpr int32_t kHardLandingCatchupBelow = -5000; // 0xFFFFEC78
inline constexpr int32_t kOscillateSinkCeiling = 2000;     // 0x7D0

// --- state ---------------------------------------------------------------

// Entity_RespawnVehicle's write set [orig: @0x45ffeb..0x46001e]. A BMS spawn
// writes NONE of these (the record memset @0x40ea27 zeroes them, fresh_2f1 = 0).
void vehicle_suspension_respawn(Entity::VehicleMotorState &m);

// The flip angle as a Q16 |up.z| bound: ftol(flip × 0.01 × 65536.0).
int32_t vehicle_flip_threshold_q16(const VehicleTraits &traits);

// --- the tick ------------------------------------------------------------


// 1b. The bike's live crash test [orig: @0x47b32d..0x47b375]: both wheels off
//  the ground, the bike has been driven, and any of the three spine probes
//  touches → request.
void vehicle_suspension_bike_crash_test(Entity &veh, bool front_contact,
                                        bool rear_contact, bool any_spine_contact);

// 2. Sink growth [orig: tracked extend loop @0x47db70..0x47dbd1 with the
//  pre-gate @0x47db76..0x47dba8 (skipped when !settled_2f2 && all sinks == 0
//  && up.z < 0); tank @0x478510..0x47852b; bike @0x47ab36..0x47abdc with NO
//  latch terms and its own +0x2F2 shape]. `latch_gated` selects the
//  `!crash_request && !crashed` terms (tracked/tank) vs the bike's none.
void vehicle_suspension_grow_sinks(Entity &veh, const bool contact[4], int wheels,
                                   int32_t growth, bool latch_gated, bool pre_gate_skip);




// 5. After the conform [orig: tracked @0x47ece9..0x47ed60]: each pad in
//  contact zeroes its own sink @0x47eced..0x47ed17; then a DIAGONAL PAIR in
//  contact ((0 && 2) || (1 && 3) @0x47ed1d..0x47ed2b), the hull upright
//  (up.z > 0 @0x47ed2d) and not crashed @0x47ed31 → landing_2ee = 0, all
//  four sinks = 0, byte_2ef = 0 (@0x47ed3a..0x47ed59; the `crashed = 0`
//  @0x47ed60 is redundant under the gate). A two-wheel row pairs 0 && 1.
void vehicle_suspension_post_contact(Entity &veh, const bool contact[4], int wheels,
                                     int32_t up_z16);

// 6. The family tail [orig: crash_request = 0 unconditionally @0x47eeee /
//  @0x4795da / @0x47c0b6; the amplitude tail @0x48178a..0x4817c4: all four
//  amplitudes under ftol(0.01 · ftol(travel_locked)) with a nonzero impact
//  sink → impact sink = 0].
void vehicle_suspension_tick_tail(Entity &veh, const VehicleTraits &traits);

} // namespace opennova::world
