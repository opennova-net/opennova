#include <runtime/world/vehicle_motor.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/io/bam.h>

#include "vehicle_motor_detail.h"

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/geom.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/vehicle_sound.h>
#include <runtime/world/world.h>

namespace opennova::world {

using namespace detail; // the shared platform-solve helpers, unqualified as before

namespace {

// Key-steer ramp: +2.0 deg/tick, capped at 50 deg [orig: @0x48b4e0 — `[137] +=
// 0x16C16C0` while `< 0x238E38C0`]. Constants verbatim.
constexpr int32_t kSteerRampStep = 0x16C16C0;
constexpr int32_t kSteerRampCap = 596523200; // 0x238E38C0

// Gravity on the vertical velocity, 16.16 u/tick per tick — the GROUND
// family constant [orig: @0x48d009 `slideDecay -= 324`; an earlier comment
// cited @0x48d69b, which is watercraft smoke-FX code — corrected by the
// platform-solve review]. The cbik mover uses 250 [orig: @0x4865a6]; the
// watercraft mover uses 167 in its not-afloat drag branch [orig: @0x48EBD9].
constexpr int32_t kGravityStep = 324;
constexpr int32_t kGravityStepBike = 250;

// Analog steer scale: BAM/tick per axis unit [orig: @0x48b783 `(192426 * analogZ) >> 1`;
// the same 2^32/360/62 deg/s->BAM/tick constant the turn_rate parse uses].
constexpr int32_t kAnalogSteerScale = 192426;

// Full-precision quantized trig at 2^22 — the original samples the 1024-entry
// fixed-point cos table [orig: off_849934, idx = (bam + 0x200000) >> 22]; the
// infantry motor established the same computed equivalent (D-INF-4).
int32_t cos22_of_bam(int32_t bam) {
    const double a = static_cast<double>(bam) * (3.14159265358979323846 / 2147483648.0);
    return static_cast<int32_t>(std::cos(a) * 4194304.0);
}
int32_t sin22_of_bam(int32_t bam) {
    const double a = static_cast<double>(bam) * (3.14159265358979323846 / 2147483648.0);
    return static_cast<int32_t>(std::sin(a) * 4194304.0);
}

// The x87 trig pair (cos22/sin22_of_bam_x87) lives in vehicle_motor_detail.h —
// shared with the contact solves in vehicle_contact_solve.cpp.

// x86 SHL used by the vehicle angle/rate integrators: keep only the low
// 32 bits at every step, exactly like the retail register operation.
int32_t bam_shl_wrap(int32_t value, unsigned shift) {
    while (shift-- != 0) value = io::bam_dbl(value);
    return value;
}

// x86 IMUL low-dword result. The shared BAM helpers cover add/sub/shift/abs;
// this is the remaining multiply primitive needed by the aircraft rate caps.
int32_t bam_mul_wrap(int32_t lhs, int32_t rhs) {
    return static_cast<int32_t>(static_cast<uint32_t>(lhs) *
                                static_cast<uint32_t>(rhs));
}

} // namespace

VehicleCtrlRegisters vehicle_ctrl_registers(
        const Entity::VehicleMotorState &state) {
    VehicleCtrlRegisters out;

    // entity+0x2B6 is the high word of the vehicle wheel/steer dword. MOVZX
    // makes a negative wheel deflection a 0..0xFFFF cyclic phase; the following
    // retail 0x10000 cap is unreachable for a zero-extended word.
    // [orig: Entity_CacheVehicleHUDStats @0x4929B0;
    //  MOVZX/store @0x4929C0..0x4929D7]
    out.steering = static_cast<int32_t>(
            static_cast<uint32_t>(state.steer_state) >> 16);

    // Reproduce CDQ/XOR/SUB as unsigned two's-complement arithmetic before the
    // unsigned 0x10000 cap. In particular INT_MIN becomes 0x80000000 (it does
    // not invoke C++ signed-abs UB) and therefore publishes 0x10000.
    // [orig: Entity_CacheVehicleHUDStats @0x4929DC..0x4929F1]
    const uint32_t speed_bits = static_cast<uint32_t>(state.speed);
    const uint32_t sign_mask = 0u - (speed_bits >> 31);
    const uint32_t magnitude =
            (speed_bits ^ sign_mask) - sign_mask;
    out.speed = static_cast<int32_t>(
            std::min(magnitude, uint32_t{0x10000}));

    // The part-animation words [orig: Entity_CacheVehicleHUDStats @0x4929B0 —
    // the rotor angle's +0x466 @0x492ACA..0x492ADE, the wheel phase's +0x2BA
    // @0x4929B4]: the accumulators' high words, zero-extended like the steer
    // word. The tail rotor publishes the same word: ordinal 47 reads the one
    // +0x464 accumulator [orig: @0x492AD7].
    out.rotor = static_cast<int32_t>(
            static_cast<uint32_t>(state.part_spin.angle) >> 16);
    out.tail_rotor = out.rotor;
    out.wheels = static_cast<int32_t>(
            static_cast<uint32_t>(state.wheel_phase) >> 16);
    return out;
}

// The controlling occupant: the Controller/Driver seat's occupant, stale-validated
// against the occupant's own mount fields (the original walks its mountHandles and
// clears mismatches every tick) [orig: @0x48b8a1-0x48b944 — occupant must satisfy
// `parentEntity == vehicle && parentSlot in {2,5} && attachBoneId == controlBone`;
// our seat model keys the same relation through Seat::occupant + Entity::mount_*,
// the wire-bone divergence is tracked in D-NET-157].
Entity *resolve_vehicle_controller(World &world, Entity &veh) {
    Entity *controller = nullptr;
    for (Seat &s : veh.seats) {
        if (!is_vehicle_control_seat(s.type)) continue;
        if (!s.occupant.valid()) continue;
        Entity *occ = world.registry.get(s.occupant);
        if (occ == nullptr || !occ->mounted || occ->mount_target != veh.handle ||
            !is_vehicle_control_seat(occ->mount_type)) {
            s.occupant = EntityHandle{}; // [orig: stale mountHandles slot -> 0xFFFF]
            continue;
        }
        if (controller == nullptr) controller = occ;
    }
    // Stale claimant (for example, a scripted/despawned occupant that bypassed detach):
    // validate the +368 mirror each tick alongside the seat sweep. The stop edge
    // fires for THE claimant only, matching the detach leg; a surviving second control
    // occupant does not inherit the claim (it re-arms only on a fresh attach).
    // [orig: Entity_DetachFromVehicle @0x4356e9..0x43577c]
    if (veh.primary_occupant.valid()) {
        Entity *po = world.registry.get(veh.primary_occupant);
        if (po == nullptr || !po->mounted || po->mount_target != veh.handle) {
            stop_ground_vehicle_sound(world, veh);
            veh.primary_occupant = EntityHandle{};
            emit_vehicle_control_stopped(world, veh);
        }
    }
    return controller;
}

// Entity_UpdateVehiclePhysics's shared player-input block. The authority stages
// a remote driver's replicated fields through it; the controlling client stages
// its current local fields instead of replaying delayed compact drive registers.
static void stage_player_vehicle_input(Entity &veh, Entity &occ,
                                       const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    // The above-water gate at the player leg head remains with D-NET-161.
    const uint32_t move_order = static_cast<uint32_t>(occ.net_move_input) |
                                (static_cast<uint32_t>(occ.net_stance_bits) << 8);
    int dir = static_cast<int>(move_order & 7u);
    bool moving = ((move_order >> 3) & 1u) != 0;
    const int32_t analog_sum = static_cast<int32_t>(occ.net_analog_x) +
                               static_cast<int32_t>(occ.net_analog_y) +
                               static_cast<int32_t>(occ.net_analog_z);
    const int32_t driver_yaw_bam =
            bam_heading_from_mission_yaw_deg(static_cast<double>(occ.yaw));

    // Family split at the command source: the boat player leg reads itemDef
    // waterSpeed (+0x8EC) at EVERY command site — retail cbot defs author no
    // player_speed, so the ground field would command 0 — and consumes MoveOrder
    // bits 6/7 as motion overrides instead of the ground lean flags: bit6 forces
    // dir=1 AND the move bit; bit7 forces cmd=waterSpeed, dir=7, skipping the
    // move/analog split. [orig: bit6 @0x48E005..0x48E00E (edx=edi=1), bit7
    // @0x48E010..0x48E028, cmd reads +0x8EC @0x48E017/@0x48E034/@0x48E088]
    const bool boat = traits.family == VehicleFamily::Watercraft;
    const int32_t cmd_base = boat ? traits.water_speed : traits.player_speed;
    bool boat_hard_turn = false;
    if (boat) {
        if ((move_order & 0x40u) != 0) {
            dir = 1;
            moving = true;
        }
        if ((move_order & 0x80u) != 0) {
            dir = 7;
            boat_hard_turn = true;
        }
    }

    if (boat_hard_turn || moving) {
        m.cmd_speed = cmd_base;
    } else {
        int32_t steer_delta =
                (kAnalogSteerScale * static_cast<int32_t>(occ.net_analog_z)) >> 1;
        const int32_t alt =
                (kAnalogSteerScale * static_cast<int32_t>(occ.net_analog_y)) >> 1;
        if (std::abs(alt) > std::abs(steer_delta)) steer_delta = alt;
        m.steer_target_bam -= steer_delta;
        m.cmd_speed =
                -(cmd_base * static_cast<int32_t>(occ.net_analog_x)) >> 7;
        // The driver's own analog-yaw write remains D-NET-161; the host cannot
        // mutate a remote peer's wire-owned yaw, and local look owns the client row.
    }

    if ((move_order & Entity::kMoveOrderCrouch) != 0) m.cmd_speed >>= 1;
    if ((move_order & Entity::kMoveOrderProne) != 0) m.cmd_speed >>= 2;
    if ((move_order & 0x20u) != 0) veh.flags |= 0x80u;
    else veh.flags &= ~0x80u;
    if (!boat) {
        // Ground/bike lean flags; a boat's bits 6/7 are the motion overrides above
        // and its leg writes no flags from this range [orig: @0x48DFE5..0x48E0C0].
        if ((move_order & 0x40u) != 0) veh.flags |= 0x20u;
        else veh.flags &= ~0x20u;
        if ((move_order & 0x80u) != 0) veh.flags |= 0x8u;
        else veh.flags &= ~0x8u;
    }

    if (analog_sum == 0) {
        m.steer_target_bam =
                (move_order & Entity::kMoveOrderFreeLook) != 0
                        ? m.yaw_bam
                        : driver_yaw_bam;
    }

    // The key-steer ramp cap is a family delta: the boat's own input block
    // caps [137] at 0x1FFFFFE0 (45deg) [orig: @0x48E13A]; the ground/bike
    // template caps at 0x238E38C0 (50deg) [orig: the shared seat-direction
    // template]. The step is 0x16C16C0 everywhere.
    const int32_t ramp_cap = traits.family == VehicleFamily::Watercraft
            ? 0x1FFFFFE0 : kSteerRampCap;
    if (dir != 0) {
        if (m.steer_ramp_bam < ramp_cap)
            m.steer_ramp_bam += kSteerRampStep;
    } else {
        m.steer_ramp_bam = 0;
    }
    // Reverse-gear command: the bike commands -[136] >> 3 (1/8 speed)
    // [orig: @0x484d0f, 0x484d2b, 0x484d50] where the ground core commands
    // -[136] >> 1 (1/2) [orig: @0x48bb89].
    const int reverse_shift = traits.family == VehicleFamily::Bike ? 3 : 1;
    switch (dir) {
        case 1:
            m.steer_target_bam = io::bam_add(m.yaw_bam, m.steer_ramp_bam);
            break;
        case 2:
            m.steer_target_bam = io::bam_add(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = 0;
            break;
        case 3:
            m.steer_target_bam = io::bam_add(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = io::bam_sar(io::bam_sub(0, m.cmd_speed), reverse_shift);
            break;
        case 4:
            m.steer_target_bam = m.yaw_bam;
            m.cmd_speed = io::bam_sar(io::bam_sub(0, m.cmd_speed), reverse_shift);
            break;
        case 5:
            m.steer_target_bam = io::bam_sub(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = io::bam_sar(io::bam_sub(0, m.cmd_speed), reverse_shift);
            break;
        case 6:
            m.steer_target_bam = io::bam_sub(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = 0;
            break;
        case 7:
            m.steer_target_bam = io::bam_sub(m.yaw_bam, m.steer_ramp_bam);
            break;
        default:
            break;
    }
}

// The occupant whose input this machine should consume. Retail's gate is
// `(occ->Flags & 0x100) && (occ == g_local_player_entity || is_authority)` — the
// AUTHORITY runs the input block for ANY player occupant, not only its own local
// player. That distinction is invisible on a listen host flying its own
// aircraft, and decisive when a JOINER is the pilot: the host owns the mover, so
// insisting on the host's local player left a remote pilot commanding nothing.
// [orig: Entity_UpdateAircraftPhysics @0x490310 input gate; the ground twin is
//  Entity_UpdateVehiclePhysics @0x48b0ff]
static Entity *resolve_piloting_player(World &world, Entity &veh,
                                       const VehicleTraits &traits) {
    if (!traits.player_control) return nullptr;
    Entity *occ = resolve_vehicle_controller(world, veh);
    if (occ == nullptr || occ->handle.pool() != 0 || occ->player_class == 0 ||
        !occ->alive || occ->health <= 0)
        return nullptr;
    const bool is_authority = world.ai != nullptr && world.ai->is_authority;
    if (!is_authority && occ->handle != world.cached.local_player) return nullptr;
    return occ;
}

static Entity *resolve_local_vehicle_controller(World &world, Entity &veh,
                                                const VehicleTraits &traits) {
    if (!traits.player_control || !world.cached.local_player.valid()) return nullptr;
    Entity *occ = resolve_vehicle_controller(world, veh);
    if (occ == nullptr || occ->handle != world.cached.local_player ||
        occ->handle.pool() != 0 || occ->player_class == 0 ||
        !occ->alive || occ->health <= 0)
        return nullptr;
    return occ;
}

// The ground/tracked contact + suspension solve — the client-executed subset
// of Entity_ProcessTrackedVehiclePhysics [orig: @0x47C1C0] — is declared in
// vehicle_motor_detail.h and defined in vehicle_contact_solve.cpp.

void tick_vehicle_motor(World &world, Entity &veh, const VehicleTraits &traits,
                        const VehicleDriveCmd *ai_cmd) {
    if (traits.physics == 0) return; // no vehicle physics selected [orig: @0x48efc7]

    Entity::VehicleMotorState &m = veh.veh;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));
        // Seed the live BAM attitude mirror from the row's authored pose: one
        // entity Pitch/Roll in the original — the same storage the contact
        // solve conforms every grounded tick [orig: entity->Pitch/Roll writes
        // @0x47EC83/@0x47EC75]. (A joiner row arms through ground_client_tick,
        // whose staging already seeded these from the wire.)
        m.air_pitch_bam = static_cast<int32_t>(veh.pitch) * 11930464;
        m.air_roll_bam = static_cast<int32_t>(veh.roll) * 11930464;
        m.yaw_seeded = true;
    }

    // The per-family contact solve owns Z, attitude, the contact byte and
    // the airborne/in-water flags for rows with resolved model boxes on a
    // terrain-backed world; every other row (boxless lib-embedder rows,
    // terrain-less unit worlds) keeps the 5-tap terrain-clamp stand-in below. Retail keys the same split on
    // graphicModel presence [orig: the @0x47C49F bail]. The family routing is
    // the class table's [orig: @0x82ABC0]: cveh/ctrn/catv -> the tracked
    // solve @0x47C1C0; ctan -> the wheeled solve @0x475DE0 (call @0x48a9ef);
    // cbik -> the light solve @0x479600 (call @0x486672).
    enum class ContactSolveKind : uint8_t { None, Tracked, Wheeled, Light };
    ContactSolveKind solve_kind = ContactSolveKind::None;
    if (world.terrain != nullptr) {
        if (traits.family == VehicleFamily::Ground &&
            ground_contact_solve_active(traits))
            solve_kind = ContactSolveKind::Tracked;
        else if (traits.family == VehicleFamily::Tank &&
                 ground_contact_solve_active(traits))
            solve_kind = ContactSolveKind::Wheeled;
        else if (traits.family == VehicleFamily::Bike &&
                 light_contact_solve_active(traits))
            solve_kind = ContactSolveKind::Light;
    }
    const bool wheeled_solve = solve_kind != ContactSolveKind::None;

    // Wreck gate: a dead vehicle stops driving (the mode-21 wreck state; its settle
    // physics is deferred with the state machine) [orig: @0x48af61 `!(Flags & 2) &&
    // Health <= 0 -> mode 21`; the drive block also rejects on Flags bit 1 via the
    // 0x10000002 mask below].
    const bool wrecked = veh.health <= 0;

    // ------------------------------------------------------------------ input block
    // [orig: Entity_UpdateVehiclePhysics @0x48af00, the `attrib & 0x40` occupant block
    // @0x48b949-0x48c034. The authority always runs it; the driver's own client runs
    // it as prediction — we ARE the authority host.]
    Entity *occ = traits.player_control ? resolve_vehicle_controller(world, veh) : nullptr;
    // A DEAD controller counts as none. The infantry death edge now detaches first;
    // this remains the same-frame safety gate when motor/system ordering varies.
    // [orig: infantry death detach @0x4b9c57..0x4b9c60]
    if (occ != nullptr && (!occ->alive || occ->health <= 0)) occ = nullptr;
    // "Occupant is a player" [orig: `occupant->Flags & 0x100`] — our runtime players
    // are pool-0 organics with a resolved soldier class.
    const bool player_occupant =
            occ != nullptr && occ->handle.pool() == 0 && occ->player_class != 0;

    if (traits.player_control) {
        if (occ == nullptr || wrecked || (veh.flags & kEntityFlagDead) != 0) {
            // No controller (or dead/locked vehicle): steer holds the current heading,
            // commanded speed decays to zero through the decel clamps below.
            // [orig: @0x48c002-0x48c02d — `+528 = entity->Yaw; [136] = 0; [137] = 0;
            //  Flags &= ~0x80; state = 22`; AI_CheckVehicleStuckState unported]
            m.steer_target_bam = m.yaw_bam;
            m.cmd_speed = 0;
            m.steer_ramp_bam = 0;
            veh.flags &= ~0x80u;
        } else if (player_occupant) {
            stage_player_vehicle_input(veh, *occ, traits);
        }
        else if (ai_cmd != nullptr && ai_cmd->ai_drive) {
            // An AI controller drives: consume the brain-computed command block
            // (AiSystem::vehicle_ai_drive — the witnessed leg's steer/speed outputs).
            // [orig: the AI-driver leg @0x48bc12-0x48c034 writes aiComp[132]/[136];
            //  the ramp/dir state is the player path's only]
            m.steer_target_bam = ai_cmd->steer_target_bam;
            m.cmd_speed = ai_cmd->cmd_speed;
            m.steer_ramp_bam = 0;
        }
        // A live NON-player controller with no drive command holds the previous
        // steer/speed targets (the deferral tail of D-NET-161: boarding-wait, the
        // handbrake byte-973 latch and the aim-lock stop are unmodeled).
    }

    // ------------------------------------------------------------- steering chase
    // Turn rate blends DOWN with speed: eff = (turnRate - minRate) * clamp01(1 -
    // speed/playerSpeed) + minRate, where minRate = turn_rate2 (else turnRate/4)
    // [orig: @0x48b990-0x48b9e6].
    {
        const int32_t turn_rate = traits.turn_rate;
        int32_t min_rate = io::bam_sar(turn_rate, 2);
        if (traits.turn_rate2 != 0) min_rate = traits.turn_rate2;
        int32_t f = 0x10000;
        if (traits.player_speed != 0) {
            f = 0x10000 - static_cast<int32_t>((static_cast<int64_t>(m.speed) * 0x10000) /
                                               traits.player_speed);
            if (f < 0) f = 0;
        }
        const int32_t eff = io::bam_add(min_rate, static_cast<int32_t>(
                (static_cast<int64_t>(io::bam_sub(turn_rate, min_rate)) * f +
                        0x8000) >> 16));
        // Proportional step: 1/64 of the heading error, clamped to the effective rate
        // [orig: @0x48b9e9 `v106 = (target - Yaw + 32) >> 6` + the +-clamp].
        int32_t delta = io::bam_sar(io::bam_add(
                io::bam_sub(m.steer_target_bam, m.yaw_bam), 32), 6);
        if (delta > eff) delta = eff;
        const int32_t neg_eff = io::bam_sub(0, eff);
        if (delta < neg_eff) delta = neg_eff;
        // Smoothed wheel deflection [orig: @0x48ba17 `aiState += (4 - 32*delta -
        // aiState) >> 3` — entity->aiState is the wheel state on vehicles].
        const int32_t steer_error = io::bam_sub(
                io::bam_sub(4, bam_shl_wrap(delta, 5)), m.steer_state);
        m.steer_state = io::bam_add(
                m.steer_state, io::bam_sar(steer_error, 3));
        // Yaw rate = -speed * (wheel >> 2) >> 16, applied while grounded
        // [orig: @0x48ba33 modelPtr0 write; the aim/AI lock bytes are unmodeled].
        if ((veh.flags & kEntityFlagInAir) == 0 || m.plat_afloat) {
            const int32_t neg_speed = io::bam_sub(0, m.speed);
            m.wheel_rate_bam = static_cast<int32_t>(
                    (static_cast<int64_t>(neg_speed) *
                             io::bam_sar(m.steer_state, 2) + 0x8000) >> 16);
        }
    }

    // ------------------------------------------------------------- speed pipeline
    int32_t target_speed;
    {
        // Airborne: the command opposes the current motion (a coast brake)
        // [orig: @0x48ba64-0x48ba8a, gated `BYTE2(aiRef0) == 0 && !(Flags & 0x2000)`].
        // GROUND-FAMILY ONLY: the cbik mover holds throttle off-contact (its
        // +25/tick jump-latch ramp is input-side; the flip simply does not
        // exist there) [orig: cbik speed servo @0x4853ac..0x4853eb].
        int32_t cmd = m.cmd_speed;
if (traits.family != VehicleFamily::Bike &&
            traits.family != VehicleFamily::Tank &&
            !m.grounded && (veh.flags & kEntityFlagInAir) == 0) {
            if (m.speed < 0) {
                if (cmd < 0) cmd = -cmd;
            } else if (cmd > 0) {
                cmd = -cmd;
            }
        }
        // Slope factor: cos^2(pitch) in 22-bit fixed [orig: @0x48ba47 — off_849934
        // cos-table sample squared >> 22]. Retail reads the live entity Pitch the
        // contact solve conforms; boxless stand-in rows keep the degree-quantized
        // row pitch (their attitude never advances past the authored pose).
        const int32_t pitch_bam = wheeled_solve
                ? m.air_pitch_bam
                : static_cast<int32_t>(
                          static_cast<int64_t>(veh.pitch) * 11930464);
        const int32_t c = cos22_of_bam(pitch_bam);
        const int32_t c2 = static_cast<int32_t>((static_cast<int64_t>(c) * c) >> 22);
        target_speed = static_cast<int32_t>((static_cast<int64_t>(c2) * cmd) >> 22);

        // Chase 1/32 of the gap per tick, then family clamps [orig: @0x48baa2
        // `rawAccel = (target - speed + 16) >> 5` + the branch tree @0x48bac0-0x48bbe0].
        const int32_t raw_accel = (target_speed - m.speed + 16) >> 5;
        m.speed_accel = raw_accel;
        if (traits.family == VehicleFamily::Tank) {
            // The tank servo's clamp tree [orig: Entity_UpdateTankVehiclePhysics
            // chase @0x489d79..0x489d84, clamps @0x489d89..0x489e79]: a
            // direction REVERSAL (and the standing
            // start) clamps at ±2·deceleration — where the ground core keeps
            // the raw 1/32 chase — and same-direction drive clamps to
            // ±acceleration (target != 0) or ±deceleration (target == 0). The
            // slope anti-creep legs ride the deferred contact-direction store
            // (D-NET-161): with the store empty, retail takes exactly these
            // plain caps [orig: the |dir|==0 arm @0x489C3A..0x489C6C].
            const bool reversal = (target_speed >= 0 && m.speed < 0) ||
                                  (target_speed <= 0 && m.speed > 0);
            if (reversal) {
                const int32_t d2 = 2 * traits.deceleration;
                if (m.speed_accel > d2) m.speed_accel = d2;
                if (m.speed_accel < -d2) m.speed_accel = -d2;
            } else if (target_speed != 0) {
                if (m.speed_accel > traits.acceleration) m.speed_accel = traits.acceleration;
                if (m.speed_accel < -traits.acceleration) m.speed_accel = -traits.acceleration;
            } else {
                if (m.speed_accel > traits.deceleration) m.speed_accel = traits.deceleration;
                if (m.speed_accel < -traits.deceleration) m.speed_accel = -traits.deceleration;
            }
        } else if (!m.grounded && traits.family != VehicleFamily::Bike) {
            // Wheels off the ground: coast clamp at half deceleration
            // [orig: @0x48bacf `±deceleration >> 1`]. The cbik mover has no
            // airborne clamp — it skips speed INTEGRATION off-contact instead
            // (below) [orig: the contact gate @0x485501..0x485534].
            const int32_t d2 = traits.deceleration >> 1;
            if (m.speed_accel > d2) m.speed_accel = d2;
            if (m.speed_accel < -d2) m.speed_accel = -d2;
        } else {
            // The skid/tire-slip leg is deferred (D-NET-161); the straight-drive clamp
            // tree is ported verbatim [orig: @0x48bb46-0x48bbe0]:
            //   same-direction drive clamps to ±acceleration; a zero target clamps to
            //   ±deceleration; a direction REVERSAL keeps the raw 1/32 chase unclamped.
            const bool reversal = (target_speed > 0 && m.speed <= 0) ||
                                  (target_speed < 0 && m.speed >= 0) ||
                                  (target_speed == 0 && m.speed == 0);
            if (!reversal) {
                if (target_speed != 0) {
                    if (m.speed_accel > traits.acceleration) m.speed_accel = traits.acceleration;
                    if (m.speed_accel < -traits.acceleration) m.speed_accel = -traits.acceleration;
                } else {
                    if (m.speed_accel > traits.deceleration) m.speed_accel = traits.deceleration;
                    if (m.speed_accel < -traits.deceleration) m.speed_accel = -traits.deceleration;
                }
            }
        }
        // The cbik AND ctan movers integrate speed only in CONTACT [orig: the
        // `!crashed && !(Flags & 0x2000) && BYTE2(aiRef0)` gates
        // @0x485501..0x485534 (cbik) / @0x489f34..0x489f56 with the <48
        // stop snap (ctan)]; the
        // ground core integrates unconditionally [orig: @0x48c302..0x48c32a].
        if ((traits.family != VehicleFamily::Bike &&
             traits.family != VehicleFamily::Tank) || m.grounded) {
            m.speed += m.speed_accel; // [orig: @0x48bbe6 `currentSpeed += speedAccel`]
            if (target_speed == 0 && std::abs(m.speed) < 48) m.speed = 0; // [orig: @0x48bbf7]
            if (m.speed_accel == 0) m.speed = target_speed;               // [orig: @0x48bc0d]
        }
    }

    bool collided = false;

    // ------------------------------------------- velocity, gravity, integration
    {
        // Direction from the live heading; velocity only re-derives while grounded —
        // airborne keeps the last (ballistic) velocity [orig: the @0x48ec74 gate
        // `!(Flags & 0x2000) && (BYTE2(aiRef0) || autopilot)` around the
        // velocity-from-heading rewrite @0x48ed3b-0x48ed6b].
        if (m.grounded) {
            if (traits.family == VehicleFamily::Tank) {
                // The tank drives along its FULL basis forward row — the
                // conformed pitch tilts the velocity, and slideDecay is
                // REPLACED by the vertical component (the crashed-gated skip
                // rides the deferred wreck machine) [orig: the normalized
                // row-0 products + `slideDecay = speed*fwd.z` in the contact
                // velocity-build stores @0x48a5ac..0x48a8b4; the low-speed
                // contact-direction realign (the ±5°/tick cross-product
                // rotate toward forward, BuildYXZ ±59652323) rides the
                // deferred D-NET-161 store — with the store empty retail
                // takes exactly this velocity = speed * fwd arm].
                const VehicleEulerBasis tb = vehicle_euler_basis(
                        m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
                const int32_t fwd_q16[3] = {
                    static_cast<int32_t>(tb.fwd[0] * 65536.0),
                    static_cast<int32_t>(tb.fwd[1] * 65536.0),
                    static_cast<int32_t>(tb.fwd[2] * 65536.0)};
                m.vel_x = detail::q16_mul_rhu(m.speed, fwd_q16[0]);
                m.vel_y = detail::q16_mul_rhu(m.speed, fwd_q16[1]);
                m.slide_z = detail::q16_mul_rhu(m.speed, fwd_q16[2]);
            } else {
                // The GROUND core's own grounded build is the SAME full-basis
                // form: dir rows from the euler matrix, and slideDecay REPLACED
                // by speed*fwd.z while the crash latch is clear. The former
                // yaw-only leg with slide_z = 0 was the D-NET-161 stand-in; it
                // left LEVEL velocity at every contact loss, so a crest sent the
                // hull sailing horizontally while the road dropped -- 27% of the
                // 00TRg convoy's drive read airborne and the witnessed coast
                // brake ate the route pace (AI-PARITY-CONCEPT 6.12g/h; the
                // 13-pin's root). Retail's frozen ballistic velocity follows the
                // slope and re-contacts at once.
                // [orig: Entity_UpdateVehiclePhysics @0x48AF00 grounded build,
                //  kong 116425-116490 -- Math_BuildFixedPointMatrixFromEuler-
                //  Angles rows 0/2, velocity = speed*row products @116804-116831,
                //  slideDecay = v139 (= speed*fwd.z) @116470 gated on the
                //  aimHeading crash byte == 0]
                const VehicleEulerBasis gb = vehicle_euler_basis(
                        m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
                const int32_t gfwd[3] = {
                    static_cast<int32_t>(gb.fwd[0] * 65536.0),
                    static_cast<int32_t>(gb.fwd[1] * 65536.0),
                    static_cast<int32_t>(gb.fwd[2] * 65536.0)};
                m.vel_x = detail::q16_mul_rhu(m.speed, gfwd[0]);
                m.vel_y = detail::q16_mul_rhu(m.speed, gfwd[1]);
                if (m.crashed == 0) // [orig: the aimHeading-byte gate @116468]
                    m.slide_z = detail::q16_mul_rhu(m.speed, gfwd[2]);
            }
        }
        if (traits.family == VehicleFamily::Bike) {
            // Bike-only vertical up-cap; the airborne input latch that can lift
            // it is input-side, so the client-run form caps unconditionally
            // [orig: vZ = min(vZ, 0x4000) @0x48659b..0x48659d].
            if (m.slide_z > 0x4000) m.slide_z = 0x4000;
            m.slide_z -= kGravityStepBike; // [orig: @0x4865a6 `slideDecay -= 250`]
            // The bike's "has been driven" byte: a leaning bike (Flags 0x20)
            // above 0x1000 speed marks itself driven; the light solve's crash
            // test over the spine probes reads it (vehicle_suspension.h)
            // [orig: Entity_UpdateLightVehiclePhysics @0x48524c].
            if ((veh.flags & 0x20u) != 0 && m.speed > 0x1000) m.has_been_driven = 1;
        } else if (traits.family == VehicleFamily::Tank) {
            // The tank shares the 250 step with the bike — no up-cap
            // [orig: `slideDecay += -250` @0x48a82c in
            // Entity_UpdateTankVehiclePhysics].
            m.slide_z -= kGravityStepBike;
        } else {
            m.slide_z -= kGravityStep; // [orig: @0x48d69b `slideDecay -= 324`]
        }
        // Submerged drag: while the solve-owned in-water flag is up, planar and
        // vertical velocity shed 1/4 per tick [orig: `test Flags,0x8000` then
        // `v -= (v+2)>>2` on all three @0x48d013..0x48d052; identical in the
        // bike mover @0x4865bb..0x4865ed]. The authority drown-drain countdown
        // (word +0x11E -> overlay clear) is authority-gated — deferred with the
        // authority damage legs [orig: @0x48d05a..0x48d083].
        if ((veh.flags & 0x8000u) != 0) {
            m.vel_x -= (m.vel_x + 2) >> 2;
            m.vel_y -= (m.vel_y + 2) >> 2;
            m.slide_z -= (m.slide_z + 2) >> 2;
        }

        const int32_t prev[3] = {to_fixed(veh.position.x), to_fixed(veh.position.y),
                                 to_fixed(veh.position.z)};
        int32_t px = prev[0] + m.vel_x;
        int32_t py = prev[1] + m.vel_y;
        int32_t pz = prev[2] + m.slide_z;

        // Hull-vs-world contact [orig: Entity_CheckCollisionState @0x462a30 from the
        // vehicle physics @0x47cb8c/0x47d213]: wall-like pushes move the hull out and
        // the collision severity decays speed through the def torque shifts
        // [orig: @0x47cc13-0x47ccc1 — sev 1/3: speed -= speed >> (torque+2),
        //  sev 2: speed -= speed >> (torque+1); `sar cl` masks the count mod 32].
        // Our stand-in reports severity 0/3 only (collision.h; D-NET-161).
        if (world.ai != nullptr && world.ai->collision != nullptr) {
            const int32_t moved[3] = {px, py, pz};
            int32_t push[2];
            const int32_t sev =
                    world.ai->collision->resolve_vehicle_hull(world, veh.handle, moved,
                                                              prev, push);
            collided = sev != 0;
            if (sev == 3) {
                px += push[0];
                py += push[1];
                m.speed -= m.speed >> ((traits.torque + 2) & 31);
            }
        }

        // Ground contact at the witnessed call sites — after Position +=
        // velocity, before the yaw apply [orig: the tracked call @0x48d0b1;
        // the wheeled call @0x48a9ef; the light call @0x486672]. Each family
        // solve rests the hull at wheel height above terrain (pads at
        // box_z_lo + r) and conforms attitude from per-corner lifts; rows
        // outside their activity predicates keep the 5-tap bilinear
        // terrain-clamp stand-in (a tracked divergence, D-NET-161).
        if (solve_kind == ContactSolveKind::Tracked) {
            ground_contact_solve(world, veh, traits, m, prev[0], prev[1],
                                 px, py, pz);
        } else if (solve_kind == ContactSolveKind::Wheeled) {
            wheeled_contact_solve(world, veh, traits, m, prev[0], prev[1],
                                  px, py, pz);
        } else if (solve_kind == ContactSolveKind::Light) {
            light_contact_solve(world, veh, traits, m, prev[0], prev[1],
                                px, py, pz);
        } else if (world.terrain != nullptr) {
            const int32_t pos3[3] = {px, py, pz};
            const GroundClearance clearance{};
            const int32_t ground =
                    calc_average_ground_height(*world.terrain, pos3, 0, clearance);
            if (ground != INT32_MIN) {
                if (pz <= ground) {
                    pz = ground;
                    m.slide_z = 0;
                    m.grounded = true;
                } else {
                    // A small clearance still counts as wheel contact (the wheel solver
                    // keeps contact through suspension travel); beyond it = airborne.
                    m.grounded = (pz - ground) <= 0x8000; // 0.5 u suspension margin
                }
            }
        } else {
            m.grounded = true; // no terrain wired (unit worlds): drive on a flat plane
            if (m.slide_z < 0) m.slide_z = 0;
            pz = to_fixed(veh.position.z);
        }

        // Grounded steering applies the wheel yaw rate [orig: @0x48ef60
        // `Yaw += modelPtr0`, gated on ground contact]. The cbik mover ALWAYS
        // applies it, quartered while the airborne/swimming flag is up
        // [orig: @0x486681..0x486697 `Yaw += modelPtr0 >> 2` under Flags 0x2000].
        if (traits.family == VehicleFamily::Bike) {
            // The original's Flags 0x2000 is the suspension solver's current
            // off-contact result. Our portable contact result is m.grounded;
            // veh.flags is not maintained by this stand-in and can be stale.
            const int32_t yaw_step = m.grounded
                    ? m.wheel_rate_bam
                    : io::bam_sar(m.wheel_rate_bam, 2);
            m.yaw_bam = io::bam_add(m.yaw_bam, yaw_step);
        } else if (traits.family == VehicleFamily::Tank) {
            // The tank applies yaw unless PARKED (the +0x2F0 park byte —
            // deferred latch machine, never set for live rows), quartered
            // while airborne — the bike shape keyed on the solve-owned flag
            // [orig: @0x48a9f7..0x48aa1d `if (!parkedByte) Yaw += (Flags &
            // 0x2000) ? modelPtr0 >> 2 : modelPtr0`]. When the solve is
            // inactive (boxless/terrain-less stand-in rows) the airborne flag
            // is stale — key on m.grounded exactly like the bike arm.
            const int32_t yaw_step =
                    (wheeled_solve ? (veh.flags & kEntityFlagInAir) != 0
                                   : !m.grounded)
                            ? io::bam_sar(m.wheel_rate_bam, 2)
                            : m.wheel_rate_bam;
            m.yaw_bam = io::bam_add(m.yaw_bam, yaw_step);
        } else if (m.grounded) {
            m.yaw_bam = io::bam_add(m.yaw_bam, m.wheel_rate_bam);
        }

        veh.position.x = static_cast<float>(from_fixed(px));
        veh.position.y = static_cast<float>(from_fixed(py));
        veh.position.z = static_cast<float>(from_fixed(pz));
        veh.yaw = static_cast<int16_t>(std::lround(
                normalize_mission_yaw_deg(mission_yaw_deg_from_bam_heading(m.yaw_bam))));
    }

    // Publish the final motor state into the generic, host-owned persistent
    // emitter seam. The claimant gate lives in the sound consumer because the
    // motor still needs to settle an unoccupied PlayerControl vehicle.
    // [orig: Entity_ProcessMovementSoundEffects call @0x48d181..0x48d25c]
    update_ground_vehicle_sound(world, veh, traits, wrecked, collided);
    // The part-animation accumulators, at the mover's tail [orig: the
    // Entity_UpdatePartSpinAccumulator call @0x48AE3D in this mover].
    vehicle_part_anim_tick(world, veh, traits);
}

namespace {

// bam_of_atan2 moved to vehicle_motor_detail.h (shared with the light solve's
// axle fit); the exact-constant note rides with it.
using detail::bam_of_atan2;

// The vehicle-template chase bucket [orig: @0x48D480 interp — {6,8,10,15,20,25,30}].
inline int16_t watercraft_chase_bucket(int32_t dist) {
    if (dist < 0x2AAA) return 6;
    if (dist < 0x4000) return 8;
    if (dist < 0x5555) return 10;
    if (dist < 0x8000) return 15;
    if (dist < 0x10000) return 20;
    if (dist < 0x20000) return 25;
    return 30;
}

} // namespace

namespace detail {

// The retail Q22 Rz(yaw)*Ry(-pitch)*Rx(roll) builder shared with collision and
// bone transforms (struct in vehicle_motor_detail.h — the contact solves in
// vehicle_contact_solve.cpp consume the same basis). Keeping one
// quantized/sign-correct basis is important: thrust consumes the PREVIOUS
// solve's pitch/roll, then the platform solver writes the attitude for the
// following tick.
VehicleEulerBasis vehicle_euler_basis(int32_t yaw_bam, int32_t pitch_bam,
                                      int32_t roll_bam) {
    VehicleEulerBasis out;
    const int32_t origin[3] = {};
    out.q22 = collision_matrix_from_euler(
            yaw_bam, pitch_bam, roll_bam, origin);
    constexpr double kInvQ22 = 1.0 / 4194304.0;
    out.fwd[0] = double(out.q22.m[0]) * kInvQ22;
    out.fwd[1] = double(out.q22.m[4]) * kInvQ22;
    out.fwd[2] = double(out.q22.m[8]) * kInvQ22;
    out.side[0] = double(out.q22.m[1]) * kInvQ22;
    out.side[1] = double(out.q22.m[5]) * kInvQ22;
    out.side[2] = double(out.q22.m[9]) * kInvQ22;
    out.up[0] = double(out.q22.m[2]) * kInvQ22;
    out.up[1] = double(out.q22.m[6]) * kInvQ22;
    out.up[2] = double(out.q22.m[10]) * kInvQ22;
    return out;
}

} // namespace detail

namespace {

struct VehicleEulerBasisQ16 {
    int32_t fwd_x = 0;
    int32_t fwd_y = 0;
    int32_t fwd_z = 0;
    int32_t up_z = 0;
};

VehicleEulerBasisQ16 vehicle_euler_basis_q16(int32_t yaw_bam,
                                             int32_t pitch_bam,
                                             int32_t roll_bam) {
    const int32_t origin[3] = {};
    const CollisionMatrix basis = collision_matrix_from_euler(
            yaw_bam, pitch_bam, roll_bam, origin);
    VehicleEulerBasisQ16 out;
    // The mover reads the retail-built Q22 matrix columns as Q16 components.
    out.fwd_x = io::bam_sar(basis.m[0], 6);
    out.fwd_y = io::bam_sar(basis.m[4], 6);
    out.fwd_z = io::bam_sar(basis.m[8], 6);
    out.up_z = io::bam_sar(basis.m[10], 6);
    return out;
}

} // namespace

// The shared per-record vehicle chase (the §5.38e template) applied to a WORLD
// entity's live pose — the interp block every family mover runs before its
// physics [orig: @0x48DB6B..0x48DDD4 (watercraft instance); same constants in
// every family]. Steps the registry position/heading and owns the stale-record
// command coast-down.
static void vehicle_client_chase(Entity &veh) {
    Entity::VehicleMotorState &m = veh.veh;
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);
    if (m.net_interp_progress == 0) {
        const int64_t dx = int64_t(m.net_smooth_target[0]) - px;
        const int64_t dy = int64_t(m.net_smooth_target[1]) - py;
        const int64_t dz = int64_t(m.net_smooth_target[2]) - pz;
        const double dd = std::sqrt(double(dx) * double(dx) +
                                    double(dy) * double(dy) +
                                    double(dz) * double(dz));
        const int32_t dist = dd >= 2147418112.0 ? INT32_MAX
                                                : static_cast<int32_t>(dd);
        // Snap radius 0x60000 while the received speed says "moving" (>= 293),
        // else 0x20000 [orig: @0x48DB9B..0x48DBAC].
        const int32_t snap = m.net_recv_speed >= 293 ? 0x60000 : 0x20000;
        if (dist > snap) {
            px = m.net_smooth_target[0];
            py = m.net_smooth_target[1];
            pz = m.net_smooth_target[2];
            m.yaw_bam = m.net_smooth_heading;
            m.net_smooth_target[0] = 0;
            m.net_smooth_target[1] = 0;
            m.net_smooth_target[2] = 0;
            m.net_interp_steps = 0;
            m.net_smooth_heading = 0;
        } else if (dist < 0x2000) {
            m.net_smooth_target[0] = 0;
            m.net_smooth_target[1] = 0;
            m.net_smooth_target[2] = 0;
            m.net_interp_steps = 0;
            m.net_smooth_heading = io::bam_add(
                    io::bam_sub(m.net_smooth_heading, m.yaw_bam), 10) / 20;
        } else {
            const int32_t n = watercraft_chase_bucket(dist);
            m.net_interp_steps = static_cast<int16_t>(n);
            m.net_smooth_target[0] =
                    (int32_t(dx) + (n >> 1)) / n;
            m.net_smooth_target[1] =
                    (int32_t(dy) + (n >> 1)) / n;
            m.net_smooth_target[2] =
                    (int32_t(dz) + (n >> 1)) / n;
            m.net_smooth_heading = io::bam_add(
                    io::bam_sub(m.net_smooth_heading, m.yaw_bam), 10) / 20;
        }
    }
    {
        const int16_t progress = m.net_interp_progress;
        if (progress < 20)
            m.yaw_bam = io::bam_add(m.yaw_bam, m.net_smooth_heading);
        if (progress < m.net_interp_steps) {
            px += m.net_smooth_target[0];
            py += m.net_smooth_target[1];
            // The Z step is AIRBORNE-ONLY: on contact, Z is contact-solve
            // owned (the platform/ground solves resettle it every tick), so
            // the template chases record Z only while Flags 0x2000 is up
            // [orig: bike @0x4848ae..0x4848e3; the identical ground gate
            // @0x48b7aa..0x48b7b9]. The witnessed !settled && !crashed
            // companions ride the park/wreck latches — dead and wire-frozen
            // rows never reach this mover (the sim clears net_predicted).
            if ((veh.flags & kEntityFlagInAir) != 0)
                pz += m.net_smooth_target[2];
        }
        if (progress >= 128) {
            // Stale records: the mirrored command coasts to zero so an
            // abandoned boat predicts to a stop [orig: @0x48DDC0..0x48DDCE].
            m.net_recv_speed -= (m.net_recv_speed + 64) >> 7;
        } else {
            m.net_interp_progress = static_cast<int16_t>(progress + 1);
        }
    }

    veh.position.x = static_cast<float>(from_fixed(px));
    veh.position.y = static_cast<float>(from_fixed(py));
    veh.position.z = static_cast<float>(from_fixed(pz));
}

// The boat platform solve — the client-executed subset of
// Entity_ProcessPlatformPhysics [orig: @0x481870, sole caller
// Entity_UpdateWatercraftPhysics @0x48ECE7 — every tick, after integration,
// before the yaw apply; runs on clients for remote boats]. Port scope
// (vehicle-client-movers-re.md §3 + vehicle-client-movers-re.md §4, 2026-07-31): the
// 7-probe terrain solve, severity speed sheds, position push, the water leg
// (afloat/draft/heave bob), lever corners + bow-lift/porpoise machine, and
// the grounded/buoyant/settled solve select with the shared 4-normal plane
// fit. Cited deferrals: authority collision damage/sound + the Flags 0x10
// latch upkeep [orig: @0x482336..0x48244D / @0x483494..0x4834C6],
// entity-entity collision + momentum exchange (no proximity list yet)
// [orig: @0x462561.. / @0x482459..], the planing roll-lean machine
// [orig: @0x45AEA0], splash/smoke FX + sounds, and the wreck-tumble path
// [orig: Entity_ClearSuspensionState @0x4592B0 chain]. Euler extraction uses
// the standard atan2 decomposition of the fitted rows — the
// Math_FixedPointMatrixToEulerAngles interior [orig: @0x613310] is a tracked
// pending witness.
namespace detail {

// One bilinear terrain probe force (struct + [orig] cites in
// vehicle_motor_detail.h — shared with the contact solves in
// vehicle_contact_solve.cpp).
int32_t plat_terrain_probe(const World &world, int32_t X, int32_t Y, int32_t Z,
                           int32_t r, int32_t soft, int32_t hard,
                           PlatProbeForce &out) {
    if (world.terrain == nullptr) return 0;
    const GroundClearance clearance{};
    auto sample = [&](int32_t sx, int32_t sy) {
        const int32_t p3[3] = {sx, sy, Z};
        return calc_average_ground_height(*world.terrain, p3, 0, clearance);
    };
    const int32_t h_xm = sample(X - r, Y), h_xp = sample(X + r, Y);
    const int32_t h_ym = sample(X, Y - r), h_yp = sample(X, Y + r);
    if (h_xm == INT32_MIN) return 0;
    // Height over the 4-sample average [orig: @0x4622A3]; clear by > r skips
    // [orig: @0x4622D2].
    const int32_t rel = Z - (h_yp >> 2) - (h_ym >> 2) - (h_xp >> 2) - (h_xm >> 2);
    if (h_xm + r < Z && h_xp + r < Z && h_ym + r < Z && h_yp + r < Z) return 0;
    const int64_t gx = h_xp - h_xm, gy = h_yp - h_ym, gz = -2 * int64_t(r);
    const double dn = std::sqrt(double(gx) * gx + double(gy) * gy + double(gz) * gz);
    const int32_t norm = dn >= 2147418112.0 ? INT32_MAX : int32_t(dn);
    if (norm == 0) return 0;
    int64_t fx = gx * r / norm, fy = gy * r / norm;
    int64_t pen = rel + gz * r / norm; // = rel - 2r^2/norm [orig: @0x462360]
    if (pen >= 0) {
        // Grazing case [orig: @0x4623AF..0x462424].
        const int64_t ax = gx ? gz * pen / gx : 0;
        const int64_t ay = gy ? gz * pen / gy : 0;
        bool hit = false;
        if (std::llabs(ax) < std::llabs(fx)) { fx += ax; hit = true; }
        if (std::llabs(ay) < std::llabs(fy)) { fy += ay; hit = true; }
        if (!hit) return 0;
        pen = 0;
    }
    const double dm = std::sqrt(double(fx) * fx + double(fy) * fy +
                                double(pen - rel) * double(pen - rel));
    const int32_t mag = dm >= 2147418112.0 ? INT32_MAX : int32_t(dm);
    if (mag == 0) return 0;
    const int32_t cosr = int32_t((int64_t(rel - pen) << 22) / mag); // [orig: @0x4624A7]
    out.fz -= int32_t(pen); // push-up [orig: @0x4624AD]
    int32_t sev = 0;
    if (cosr < soft) {
        out.fx -= int32_t(fx); out.fy -= int32_t(fy); sev = 3;
    } else if (cosr < ((hard + soft) >> 1)) {
        out.fx -= int32_t(fx >> 2); out.fy -= int32_t(fy >> 2); sev = 2;
    } else if (cosr < hard) {
        out.fx -= int32_t(fx >> 3); out.fy -= int32_t(fy >> 3); sev = 1;
    } // else climbable: vertical only [orig: @0x4624BB..0x462528]
    return sev;
}

} // namespace detail

// q16_mul_rhu / q16_normalize / q16_cross moved to vehicle_motor_detail.h
// (shared with the wheeled/light solves); the witness notes ride with them.

namespace detail {

// The shared 4-normal plane fit, ported in the witnessed fixed/float mix
// (record §4.1 "Port literally"): 16.16-quantized normalized edges, RHU
// fixed-point cross products, the set-1/set-2 duplicated normal, integer
// aggregation with the float `*0.25` round-trip (flt_7C333C), and a final
// quantized normalize per row — NOT clean double math, whose unquantized
// intermediates drift the fitted attitude/Z by LSBs versus retail.
// [orig: fit 0x46C92B../0x46D4C7../0x46D6D8../0x46D8E8 (orientation solver)
//  and 0x46B332../0x46BC76../0x46BE7C../0x46C091 (wheeled solver);
//  aggregation 0x46DC1F..0x46DC69 / 0x46C3B1..0x46C3FB]
void plat_fit_corners(const int32_t c[4][3], PlatFit &out) {
    auto edge = [&c](int i, int j, int32_t u[3]) {
        const int64_t d[3] = {int64_t(c[i][0]) - c[j][0],
                              int64_t(c[i][1]) - c[j][1],
                              int64_t(c[i][2]) - c[j][2]};
        q16_normalize(d, u);
    };
    // Edge sets, verbatim pairing (set 1 == set 2, real shipped duplication —
    // a fourth distinct corner normal is never computed):
    int32_t a[3], b[3], e3[3], f4[3];
    edge(0, 3, a);  // c0 - c3
    edge(3, 2, b);  // c3 - c2
    edge(1, 2, e3); // c1 - c2
    edge(0, 1, f4); // c0 - c1
    int64_t raw[3];
    int32_t n1[3], n2[3], n3[3], n4[3];
    q16_cross(a, b, raw);   q16_normalize(raw, n1);
    q16_cross(a, b, raw);   q16_normalize(raw, n2); // set 2 recomputes set 1
    q16_cross(e3, b, raw);  q16_normalize(raw, n3);
    q16_cross(e3, f4, raw); q16_normalize(raw, n4);
    int64_t up_s[3], fwd_s[3], side_s[3];
    for (int i = 0; i < 3; ++i) {
        up_s[i] = int64_t(n1[i]) + n2[i] + n3[i] + n4[i];
        fwd_s[i] = 2 * int64_t(e3[i]) + 2 * int64_t(a[i]); // e4+e3+a+e2
        side_s[i] = int64_t(f4[i]) + 3 * int64_t(b[i]);    // f4+f3+b+f2
    }
    // `each *= 0.25` through the float constant, ftol'd back [orig:
    // 0x46DC23..0x46DCF0], then the final quantized row normalize.
    for (int i = 0; i < 3; ++i) {
        up_s[i] = static_cast<int64_t>(double(up_s[i]) * 0.25);
        fwd_s[i] = static_cast<int64_t>(double(fwd_s[i]) * 0.25);
        side_s[i] = static_cast<int64_t>(double(side_s[i]) * 0.25);
    }
    int32_t up[3], fwd[3], side[3];
    q16_normalize(up_s, up);
    q16_normalize(fwd_s, fwd);
    q16_normalize(side_s, side);
    out.fwd_z = double(fwd[2]) / 65536.0;
    const double fxy = std::sqrt(double(fwd[0]) * double(fwd[0]) +
                                 double(fwd[1]) * double(fwd[1]));
    out.pitch_bam = bam_of_atan2(double(fwd[2]), fxy);
    // Roll sign: the extraction must be the exact inverse of the pose builder
    // or any heel flip-flops sign at 62 Hz (the review-confirmed defect). The
    // builder is now the retail-witnessed Q22 collision_matrix_from_euler
    // basis, whose side row pairs with the UN-negated atan2 — pinned
    // end-to-end by run_platform_basis_preserves_roll_sign. Plain atan2
    // handles up[2] <= 0 as the obtuse (capsized) roll; the exact
    // Math_FixedPointMatrixToEulerAngles interior [orig: @0x613310] remains
    // the pending witness for the substitute pair as a whole.
    out.roll_bam = bam_of_atan2(double(side[2]), double(up[2]));
    // Z = the plain corner average in the ORIENTATION solver [orig: solvedPos.Z
    // @0x46E099..0x46E0B3, `*0.25` via flt_7C333C]; the wheeled solver's
    // leg-C variant averages only the corners with z > 0 (blockers §3) —
    // positive_z_avg below.
    out.z_avg = static_cast<int32_t>(
            double(int64_t(c[0][2]) + c[1][2] + c[2][2] + c[3][2]) * 0.25);
    int64_t psum = 0;
    int pn = 0;
    for (int k = 0; k < 4; ++k)
        if (c[k][2] > 0) { psum += c[k][2]; ++pn; }
    // All-nonpositive = the witnessed x87 div-by-zero hazard; guard with the
    // plain average (unreachable at real world heights).
    out.positive_z_avg =
            pn > 0 ? static_cast<int32_t>(psum / pn) : out.z_avg;
}

} // namespace detail

void watercraft_platform_solve(World &world, Entity &veh,
                               const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    // No resolved model boxes (lib-only embedders / unresolved graphics):
    // the level-hull + chase-Z stand-in remains for this row.
    if (traits.box_z_hi == traits.box_z_lo || traits.box_y_hi == traits.box_y_lo)
        return;

    const int32_t W = world.env.water_z;
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);

    // The mover integrates Position.Z += slideDecay (with the witnessed
    // gravity forms) BEFORE this call — see the §6/§7 tail of
    // watercraft_client_tick [orig: @0x48EBB5..0x48ECC6]. Deferrals carried at
    // this entry: the §2 sleep early-out (Z-unwind + slideDecay halving
    // convergence at rest [orig: @0x4818B9..0x481A5C]) and the amphibian
    // draft form (updateCallback == 0x48F010 → avg - q/2 [orig: @0x482B6E] —
    // catv rides the Ground family in our dispatch).
    // The every-call tuning clamps [orig: @0x481ACC..0x481BA3]:
    const int32_t t_pitch = std::clamp(traits.pitch_lift, 0, 10);
    const int32_t t_pitch_vel = std::clamp(traits.pitch_lift_vel, 0, 10);
    const int32_t t_bob = std::clamp(traits.bob, 0, 10);

    // ---- §3 probe geometry. Probe springs +0x2D4.. are provably zero for
    // pure boats (blockers §4). q = beam/4.
    const int32_t q = (traits.box_y_hi - traits.box_y_lo) >> 2;
    if (q <= 0) return;
    m.plat_solve_valid = true;
    const int32_t zb = traits.box_z_lo + q;
    // 4 corner probes + 3 midline probes (model space) [orig: @0x481CB4..].
    int32_t rm = std::min(((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000,
                          ((traits.box_y_hi - traits.box_y_lo) >> 1) - 0x1000);
    if (rm < 0x2000) rm = 0x2000;
    const int32_t lx = traits.box_x_hi - traits.box_x_lo;
    const int32_t ymid = traits.box_y_lo + ((traits.box_y_hi - traits.box_y_lo) >> 1);
    const int32_t zt = traits.box_z_hi - rm;
    const int32_t probes_model[7][3] = {
        {traits.foot_x_hi - q, traits.foot_y_hi - q, zb}, // p0 bow-A
        {traits.foot_x_hi - q, traits.foot_y_lo + q, zb}, // p1 bow-B
        {traits.foot_x_lo + q, traits.foot_y_lo + q, zb}, // p2 stern-B
        {traits.foot_x_lo + q, traits.foot_y_hi - q, zb}, // p3 stern-A
        {traits.box_x_lo + (lx >> 2), ymid, zt},          // p4
        {traits.box_x_lo + ((3 * lx) >> 2), ymid, zt},    // p5 [(3*lx)>>2 verbatim]
        {traits.box_x_lo + (lx >> 1), ymid, zt},          // p6
    };
    const int32_t radii[7] = {q, q, q, q, rm, rm, rm};
    const int32_t v210 = -(traits.box_z_lo + q); // corner-0 model-Z, negated (§3)
    // Half-extents of the lever rectangle (§3 tail).
    const int32_t hb = (traits.foot_y_hi - q) - (traits.foot_y_lo + q);
    const int32_t hl = (traits.foot_x_hi - q) - (traits.foot_x_lo + q);

    // World transform: the pose matrix from {Pos, Yaw, Pitch, Roll} — the
    // boat's live attitude rides air_pitch_bam/air_roll_bam (the shared
    // attitude fields the sim mirrors to the row).
    const VehicleEulerBasis basis = vehicle_euler_basis(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
    const double *fwdv = basis.fwd;
    const double *sidev = basis.side;
    const double *upv = basis.up;
    int32_t probes[7][3];
    place_probes(basis, probes_model, px, py, pz, probes);

    // ---- §4/§6 first force pass + severity response (plat_probe_pass).
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
    const int32_t sev = plat_probe_pass(world, probes, radii, soft, hard, forces);
    if (sev == 1) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x4821E7]
    } else if (sev == 2) {
        m.speed -= m.speed >> ((traits.torque + 1) & 31); // [orig: @0x4822A4]
    } else if (sev == 3) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x4822C9]
        // Authority damage/kill + collision sound + momentum exchange + the
        // dead yaw-kick = cited deferrals (spec §6; the yaw-kick is witnessed
        // DEAD code). The 0.25 cut is gated on the STRONGEST planar force
        // probe sitting > 0x8000 from Position in the plane, and fires only
        // with no hit entity (always true here -- entity-entity collision is a
        // deferral) [orig: the scan @0x482546..0x4825DD; the distance gate
        // @0x4825E3..0x48262D; the cut @0x4826EB] (strongest_probe_beyond_hull).
        if (strongest_probe_beyond_hull(forces, probes, 7, px, py))
            m.speed = int32_t(m.speed * 0.25); // [orig: flt_7C333C @0x4826EB]
    }

    // ---- §7 position push + second pass (severity >= 1 only). zc[] mirrors
    // the SHARED force buffer the grounded leg reads [orig: §10-A
    // @0x483D5C..0x483F04]: pass 1 fills it; the sev>=1 second pass re-zeroes
    // and refills it; when §7 is skipped (sev 0, climbable contact) it still
    // holds the PASS-1 forces -- the saved zf[] carries the averaged values.
    int32_t zf[7], zc[7];
    for (int i = 0; i < 7; ++i) { zf[i] = forces[i].fz; zc[i] = forces[i].fz; }
    if (sev >= 1) {
        int64_t dX = 0, dY = 0;
        for (int i = 0; i < 7; ++i) { dX += forces[i].fx; dY += forces[i].fy; }
        for (int i = 0; i < 7; ++i) { probes[i][0] += int32_t(dX); probes[i][1] += int32_t(dY); }
        PlatProbeForce forces2[7];
        int32_t sev2 = 0;
        for (int i = 0; i < 7; ++i)
            sev2 = std::max(sev2, plat_terrain_probe(world, probes[i][0], probes[i][1],
                                                     probes[i][2], radii[i], soft, hard,
                                                     forces2[i]));
        for (int i = 0; i < 7; ++i) zc[i] = forces2[i].fz;
        if (sev2 != 0) {
            int64_t dX2 = 0, dY2 = 0;
            for (int i = 0; i < 7; ++i) { dX2 += forces2[i].fx; dY2 += forces2[i].fy; }
            for (int i = 0; i < 7; ++i) zf[i] = (forces2[i].fz + zf[i]) >> 1;
            dX = (dX2 + dX) >> 1;
            dY = (dY2 + dY) >> 1;
        }
        px += int32_t(dX);
        py += int32_t(dY); // Z-sum always 0 [orig: @0x482A86..0x482A8C]
    }

    // ---- §5 bob/lift parameters.
    const double F = double(q);
    const int32_t amp = std::min(int32_t(F * 0.0625), 352);
    int32_t floatH, liftHi, liftLo, planeSpd, pitchThr;
    if (traits.mass <= 1) { // LIGHT boat [orig: @0x482105]
        floatH = int32_t(0.65 * F);
        liftHi = t_pitch_vel * 100;
        liftLo = 250;
        planeSpd = 10000;
        pitchThr = int32_t(double(t_pitch) * 0.1 * 4096.0);
    } else { // HEAVY [orig: @0x48221B]
        floatH = q;
        liftHi = int32_t(double(t_pitch_vel) * F * 0.004);
        liftLo = liftHi >> 1;
        planeSpd = 20000;
        const double arz = std::abs(sidev[2]) * 65536.0;
        if (arz > double(0x2000)) { // heavily rolled [orig: @0x482259]
            pitchThr = int32_t(double(t_pitch) * 0.1 * 409.6);
            liftHi = 250;
        } else {
            pitchThr = int32_t(double(t_pitch) * 0.1 * 4096.0);
        }
    }
    const int32_t dipExit = int32_t(double(pitchThr) * (1.0 - 0.1 * double(t_bob)));

    // ---- §8 water leg: per-corner submersion, draft, the afloat flag.
    int32_t sub_k[4], cz_k[4];
    for (int k = 0; k < 4; ++k) {
        cz_k[k] = probes[k][2];
        sub_k[k] = W + q - cz_k[k];
    }
    const int32_t avg = (cz_k[0] + cz_k[1] + cz_k[2] + cz_k[3]) >> 2;
    int32_t draft = avg;
    if (m.plat_afloat) draft = int32_t(double(avg) - 0.9 * F); // boat form [orig: flt_7C459C]
    // W == 0 = a no-water world (our env sentinel; retail worlds always carry
    // a plane -- scope note). Amphibian draft = deferral (entry note).
    if (W == 0 || draft + v210 >= W) {
        m.plat_afloat = false; // [orig: @0x482DB7; emitter release deferred]
    } else {
        // Entry edge: the hull was above the plane and is now under it. Retail
        // spawns the splash emitter and fans the positioned effect ONCE here,
        // gated on the same latch, placing it at the water plane rather than at
        // the hull's own Z. The local emitter stays a deferral; the network fan
        // is recorded for the host to drain.
        // [orig: @0x482BB9..0x482C9D — the `(Flags & 0x8000) == 0` gate,
        //  `dest[6] = Env_WaterHeightFixed`, then
        //  Server_SendOverlayActionToAlive @0x50a1b0]
        if (!m.plat_afloat) {
            world.water_crossings.add(
                    to_fixed(veh.position.x), to_fixed(veh.position.y), W,
                    /*airborne=*/(veh.flags & kEntityFlagInAir) != 0);
        }
        m.plat_afloat = true; // [orig: @0x482CA5]
    }
    veh.flags = m.plat_afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);

    // ---- §9 lever corners around the CURRENT pose + machines. Built through
    // the same Q22 rotate as the probes — the witnessed corner products are
    // round-half-up 16.16 through the pose matrix [orig: @0x482E31..0x48346D],
    // not double-precision basis-row sums.
    int32_t c[4][3];
    {
        const int32_t hb2 = hb >> 1, hl2 = hl >> 1;
        const int32_t corner_model[4][3] = {
            {+hl2, -hb2, 0}, // c0: (-hb/2)*right + (+hl/2)*fwd
            {+hl2, +hb2, 0}, // c1
            {-hl2, -hb2, 0}, // c2
            {-hl2, +hb2, 0}, // c3
        };
        for (int k = 0; k < 4; ++k) {
            int32_t rotated[3];
            basis.q22.rotate_point(corner_model[k], rotated);
            c[k][0] = px + rotated[0];
            c[k][1] = py + rotated[1];
            c[k][2] = pz + rotated[2];
        }
    }
    // Capsize latch (client form; the authority Flags 0x10 upkeep AND the
    // flip-handling CLEAR legs [orig: the righting calls @0x4835DA..0x48367D]
    // = cited deferrals riding the wreck-tumble path).
    if ((veh.flags & kEntityFlagInAir) == 0 && upv[2] < 0.0 && !m.plat_capsized)
        m.plat_capsized = true; // [orig: @0x483474..0x48348B]
    // Accumulator ramp [orig: @0x483680..0x4836E6].
    for (int k = 0; k < 4; ++k)
        if (zf[k] <= 0 && sub_k[k] <= floatH) m.plat_acc[k] += 250;
    // Bow lift / planing / porpoise [orig: @0x4836EC..0x483891]. The command
    // register is brain[136] (brain+0x220) — the COMMANDED speed. On a remote
    // boat the mirror makes it equal the received register, but the local
    // driver's [136] is the §1.8 current/received average, and the solve reads
    // that averaged command, not the raw wire value.
    const int32_t cmd = m.cmd_speed;
    const double fwd_z_now = fwdv[2] * 65536.0;
    if (m.speed > 0 && liftLo > 0 && liftHi > 0) {
        if (m.plat_afloat && cmd > 0) {
            m.plat_acc[0] = m.plat_acc[1] = 0;
            m.plat_planing = true;
            if (!m.plat_porpoise) {
                if (fwd_z_now > double(pitchThr)) {
                    // dist/293 > (cmd/293)*0.5 -> latch (the per-tick travel
                    // is our speed here; the saved-pose dist is equivalent
                    // between integrations).
                    if (m.speed / 293 > (cmd / 293) / 2) m.plat_porpoise = true;
                } else {
                    const int32_t lift = (m.speed >= planeSpd) ? liftHi : liftLo;
                    c[0][2] += lift;
                    c[1][2] += lift; // bow rises
                }
            } else {
                if (fwd_z_now < double(dipExit)) m.plat_porpoise = false;
                else { c[0][2] -= liftHi; c[1][2] -= liftHi; }
            }
        } else if (cmd <= 0) {
            m.plat_planing = false;
        }
    }
    // Post-machine adjustments [orig: @0x483893..0x48398C]. The planing
    // roll-lean machine @0x45AEA0 and the +0x2EF override path = deferrals.
    if (m.plat_planing) {
        if (m.plat_afloat && m.speed > 0x2000)
            m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
    } else if (!m.plat_at_rest) {
        for (int k = 0; k < 4; ++k) c[k][2] -= 500;
        m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
    }

    // ---- §10 solve select.
    bool any_ground = false;
    for (int k = 0; k < 4; ++k) any_ground |= zf[k] > 0;
    PlatFit fit;
    if (any_ground) {
        // A. Grounded: airborne cleared HERE and only here.
        veh.flags &= ~kEntityFlagInAir; // [orig: @0x483D55]
        for (int k = 0; k < 4; ++k) {
            int32_t lift;
            if (sub_k[k] > floatH) {
                // Deep + grounded: the raw call-#2 force when the probe force
                // exceeds the submersion, else raise to the waterline
                // [orig: @0x483D5C..0x483F04 reads zc_k].
                lift = (zf[k] > sub_k[k]) ? zc[k] : std::abs(sub_k[k] - floatH);
                m.plat_acc[k] = 0;
            } else if (zf[k] != 0) {
                lift = zc[k];
                m.plat_acc[k] = 0;
            } else {
                lift = 250 - m.plat_acc[k];
            }
            c[k][2] += lift;
        }
        m.slide_z = 0; // [orig: @0x483F25]
        plat_fit_corners(c, fit);
        pz = fit.z_avg;
        m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
    } else {
        // At-rest bob arm [orig: @0x4839C5..0x483A29].
        int32_t floatH_eff = floatH;
        if (m.speed < 100 && m.plat_afloat &&
            std::abs(fwd_z_now) < 5.0 && std::abs(sidev[2] * 65536.0) < 5.0) {
            if (!m.plat_at_rest) {
                m.plat_at_rest = true;
                m.plat_bob_phase = 4.71f; // 3pi/2 [orig: flt_7C6F84]
            }
            m.plat_bob_phase += 0.03488888964056969f; // [orig: flt_7C6EB4]
            const double f =
                std::min(1.0, (std::sin(double(m.plat_bob_phase)) + 1.0) * 0.5);
            const int32_t inc = int32_t(double(amp) * f);
            for (int k = 0; k < 4; ++k) c[k][2] += inc;
            floatH_eff = floatH - inc;
        } else {
            m.plat_at_rest = false;
        }
        bool any_deep = false;
        for (int k = 0; k < 4; ++k) any_deep |= sub_k[k] > floatH_eff;
        if (any_deep) {
            // B. Buoyancy push-up.
            if (m.slide_z < 0) m.slide_z = 0; // [orig: @0x483C15]
            for (int k = 0; k < 4; ++k) {
                int32_t lift;
                if (sub_k[k] >= floatH_eff) {
                    lift = std::abs(sub_k[k] - floatH_eff);
                    m.plat_acc[k] = 0;
                } else {
                    lift = std::min(0, 250 - m.plat_acc[k]);
                }
                c[k][2] += lift;
            }
            plat_fit_corners(c, fit);
            pz = fit.z_avg;
        } else {
            // C. Settled / airborne.
            if (m.plat_afloat) {
                int lowest = 0, second = 1;
                for (int k = 1; k < 4; ++k)
                    if (c[k][2] < c[lowest][2]) lowest = k;
                second = lowest == 0 ? 1 : 0;
                for (int k = 0; k < 4; ++k)
                    if (k != lowest && c[k][2] < c[second][2]) second = k;
                if (cmd <= 0) {
                    m.plat_acc[lowest] = m.plat_acc[second] = 0;
                    if (fwd_z_now < 100.0)
                        m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] =
                                m.plat_acc[3] = 0;
                }
                for (int k = 0; k < 4; ++k)
                    c[k][2] += std::min(0, 250 - m.plat_acc[k]);
            } else {
                veh.flags |= kEntityFlagInAir; // [orig: @0x483B98]
            }
            // The wheeled solver's settled/airborne fit: same 4-normal fit;
            // Z = the average of the ABOVE-ground corners, rise-clamped
            // +0x2000/tick (blockers §3).
            plat_fit_corners(c, fit);
            if (m.plat_afloat) {
                // The wheeled solver's settled-fit Z: the average of the
                // ABOVE-ZERO corners only, rise-clamped +0x2000/tick
                // (blockers §3).
                int32_t new_z = fit.positive_z_avg;
                if (new_z > pz + 0x2000) new_z = pz + 0x2000;
                pz = new_z;
            }
        }
    }
    // Attitude out: pitch/roll always; yaw-from-fit rides byte+0x2EC (never
    // set for live boats — deferral with the wreck path).
    m.air_pitch_bam = fit.pitch_bam;
    m.air_roll_bam = fit.roll_bam;
    // Airborne tick counter [orig: @0x483FAF/0x483FC8].
    m.plat_airborne_ticks =
            (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;

    veh.position.x = float(from_fixed(px));
    veh.position.y = float(from_fixed(py));
    veh.position.z = float(from_fixed(pz));
}

namespace {

bool watercraft_has_platform_geometry(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           ((traits.box_y_hi - traits.box_y_lo) >> 2) > 0;
}

// Establish only the platform water latch for a newly promoted client hull.
// The full solve's water decision depends on the four transformed corner
// heights but not on its later collision/bob mutations, so this gives the first
// mover tick the state a preceding retail platform frame would have supplied
// without double-running gravity, accumulators, or the attitude fit.
void watercraft_seed_platform_latch(World &world, Entity &veh,
                                    const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    const int32_t q = (traits.box_y_hi - traits.box_y_lo) >> 2;
    if (q <= 0) return;
    const int32_t zb = traits.box_z_lo + q;
    const int32_t corners[4][3] = {
        {traits.foot_x_hi - q, traits.foot_y_hi - q, zb},
        {traits.foot_x_hi - q, traits.foot_y_lo + q, zb},
        {traits.foot_x_lo + q, traits.foot_y_lo + q, zb},
        {traits.foot_x_lo + q, traits.foot_y_hi - q, zb},
    };
    const int32_t pos[3] = {to_fixed(veh.position.x),
                            to_fixed(veh.position.y),
                            to_fixed(veh.position.z)};
    const int32_t origin[3] = {};
    const CollisionMatrix basis = collision_matrix_from_euler(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam, origin);
    int32_t corner_z[4];
    for (int i = 0; i < 4; ++i) {
        int32_t rotated[3];
        basis.rotate_point(corners[i], rotated);
        corner_z[i] = pos[2] + rotated[2];
    }
    const int32_t avg =
            (corner_z[0] + corner_z[1] + corner_z[2] + corner_z[3]) >> 2;
    const int32_t v210 = -(traits.box_z_lo + q);
    // The platform solve's afloat decision is hysteretic: once afloat, the
    // boat-form draft is 0.9*beam-quarter below the corner average. A promoted
    // client row has lost that prior latch, so reconstruct the basin it would
    // already occupy instead of evaluating the dry-entry arm at the waterline.
    const int32_t afloat_draft =
            static_cast<int32_t>(double(avg) - 0.9 * double(q));
    m.plat_afloat = world.env.water_z != 0 &&
            afloat_draft + v210 < world.env.water_z;
    m.plat_solve_valid = true;
    veh.flags = m.plat_afloat ? (veh.flags | 0x8000u)
                              : (veh.flags & ~0x8000u);
}

// The npruntime headless host boots World without the shell's model-box
// resolution (VehicleTraits.box_z_* is stamped only by the adapter's
// nova_simulation_assets resolve), so its watercraft have no platform
// geometry and the solver cannot produce an afloat latch. This water-plane
// stand-in covers exactly that path; resolved hulls always consume the
// prior platform solve.
void watercraft_refresh_fallback_afloat(World &world, Entity &veh) {
    Entity::VehicleMotorState &m = veh.veh;
    bool afloat = false;
    if (world.env.water_z != 0) {
        if (world.terrain == nullptr) {
            afloat = true;
        } else {
            const int32_t pos[3] = {to_fixed(veh.position.x),
                                    to_fixed(veh.position.y),
                                    to_fixed(veh.position.z)};
            const GroundClearance clearance{};
            const int32_t ground = calc_average_ground_height(
                    *world.terrain, pos, 0, clearance);
            afloat = ground == INT32_MIN || ground < world.env.water_z;
        }
    }
    m.plat_afloat = afloat;
    veh.flags = afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);
}

} // namespace

static void watercraft_motor_core(World &world, Entity &veh,
                                  const VehicleTraits &traits);

// [orig: Entity_UpdateWatercraftPhysics @0x48D480 — the client-executed subset for a
// remote boat; disasm-verified spec 2026-07-31 (net-re §5.38e). Block cites inline.
// Deliberate stand-in (both this and the air mover): the client-run
// deck-carrier follow (groundEntity tick-delta + parent-rotation re-seat
// @0x48D6DA..0x48DACD / @0x4905BC..0x49095B) is replaced by the embedding
// sim freezing carried rows to the row-level seat-follow — the D-NET-196
// client-subset facet (divergence-ledger permanent register; the INFANTRY
// deck-ride twins ARE ported: client_replica_pipeline row_deck_ride).]
void carrier_pose_fixed(const Entity &e, int32_t pos[3], int32_t &yaw,
                        int32_t &pitch, int32_t &roll) {
    pos[0] = to_fixed(e.position.x);
    pos[1] = to_fixed(e.position.y);
    pos[2] = to_fixed(e.position.z);
    if (e.veh.net_predicted && e.veh.yaw_seeded) {
        yaw = e.veh.yaw_bam;
        pitch = e.veh.air_pitch_bam;
        roll = e.veh.air_roll_bam;
    } else {
        yaw = bam_heading_from_mission_yaw_deg(e.yaw);
        pitch = static_cast<int32_t>(
            std::llround(static_cast<double>(e.pitch) / kDegreesPerBam));
        roll = static_cast<int32_t>(
            std::llround(static_cast<double>(e.roll) / kDegreesPerBam));
    }
}

void stamp_saved_live_pose(Entity &e) {
    carrier_pose_fixed(e, e.saved_live_pos, e.saved_live_yaw,
                       e.saved_live_pitch, e.saved_live_roll);
    e.saved_live_valid = true;
}

void watercraft_client_tick(World &world, Entity &veh, const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    if (!m.net_predicted) return;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(veh.yaw);
        m.yaw_seeded = true;
    }

    // ---- 1. Per-record chase (the §5.38e vehicle template) on the world pose.
    vehicle_client_chase(veh);
    if (!watercraft_has_platform_geometry(traits)) {
        watercraft_refresh_fallback_afloat(world, veh);
    } else if (!m.plat_solve_valid) {
        // An exact-handle wire-materialized joiner hull has no earlier local frame, but the mover
        // consumes the PREVIOUS platform solve's afloat/attitude state. Seed
        // that state before its first thrust/drag pass instead of treating a
        // resolved floating hull as landed for one frame.
        watercraft_seed_platform_latch(world, veh, traits);
    }

    // ---- 2. Register mirror: the non-driver machine adopts the received
    // speed/steer as its own drive command, every tick
    // [orig: brain[136]=[177], brain[132]=[179] @0x48DDD4..0x48DDF4].
    // Deferred witnessed gate: retail mirrors only when occupantEntity !=
    // g_local_player_entity — the local driver's machine runs the input leg
    // instead ([136] = ([136]+[177])>>1 averaging) [orig: @0x48DDD4/@0x490C9E].
    // The joiner resolves the local controlling occupant below; other rows
    // remain remote-occupied and take the verbatim register mirror.
    if (Entity *local_driver =
                resolve_local_vehicle_controller(world, veh, traits)) {
        stage_player_vehicle_input(veh, *local_driver, traits);
        // Retail reconciles only longitudinal command on the controlling
        // client; its steer target stays owned by current local LOOK/input.
        m.cmd_speed = io::bam_sar(
                io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
    } else {
        m.cmd_speed = m.net_recv_speed;
        m.steer_target_bam = m.net_recv_steer_bam;
    }

    watercraft_motor_core(world, veh, traits);
}

// Blocks 12..19 of the cbot mover — the steer integrator through the yaw apply.
// ONE sequence in the original, executed by every machine that runs the function
// past the input gate: the authority (with the occupant/AI/parked staging done),
// the driver's client (local prediction), and remote clients (register mirror).
// Extracted verbatim from the 2026-07-31 client-subset port; the authority tick
// below stages its inputs and runs the same core.
// [orig: Entity_UpdateWatercraftPhysics @0x48D480, blocks @0x48E82C..0x48ED76]
static void watercraft_motor_core(World &world, Entity &veh,
                                  const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);

    // ---- 3. Steer/rudder integrator [orig: @0x48E82C..0x48E926].
    {
        const int32_t turn_rate = traits.turn_rate;
        // Retail reads ONLY itemDef waterSpeed here; a zero pins the speed
        // fraction at 0 so the effective turn rate stays at min_rate — no
        // player_speed fallback exists [orig: the jz to the f=0 arm @0x48E844].
        const int32_t water_spd = traits.water_speed;
        int32_t min_rate = io::bam_sar(turn_rate, 2);
        if (traits.turn_rate2 != 0) min_rate = traits.turn_rate2;
        int32_t f = 0;
        if (water_spd != 0) {
            f = 0x10000 - static_cast<int32_t>(
                    (static_cast<int64_t>(m.speed) * 0x10000) / water_spd);
        }
        if (f < 0) f = 0;
        // No upper clamp — a reversing hull over-rotates, witnessed absent.
        const int32_t eff = io::bam_add(min_rate, static_cast<int32_t>(
                (static_cast<int64_t>(io::bam_sub(turn_rate, min_rate)) * f +
                        0x8000) >> 16));
        int32_t delta = io::bam_sar(io::bam_add(
                io::bam_sub(m.steer_target_bam, m.yaw_bam), 32), 6);
        if (delta > eff) delta = eff;
        const int32_t neg_eff = io::bam_sub(0, eff);
        if (delta < neg_eff) delta = neg_eff;
        const int32_t steer_error = io::bam_sub(
                io::bam_sub(4, bam_shl_wrap(delta, 5)), m.steer_state);
        m.steer_state = io::bam_add(
                m.steer_state, io::bam_sar(steer_error, 3));
        // Airborne, non-afloat hulls retain their existing yaw rate; grounded
        // or floating hulls recompute from the rudder state.
        // [orig: @0x48E8E3..0x48E920]
        if ((veh.flags & kEntityFlagInAir) == 0 || m.plat_afloat) {
            const int32_t neg_speed = io::bam_sub(0, m.speed);
            m.wheel_rate_bam = static_cast<int32_t>(
                    (static_cast<int64_t>(neg_speed) *
                             io::bam_sar(m.steer_state, 2) + 0x8000) >> 16);
        }
    }

    // ---- 4. Thrust [orig: @0x48E926..0x48EA12].
    int32_t vertical_thrust = 0;
    {
        const int32_t cmd = m.cmd_speed;
        if (cmd == 0) {
            if (io::bam_abs(m.vel_x) < 384) m.vel_x = 0;
            if (io::bam_abs(m.vel_y) < 384) m.vel_y = 0;
        } else {
            const int32_t a = io::bam_abs(cmd);
            int32_t acc = io::bam_add(
                    traits.acceleration,
                    io::bam_add(io::bam_sar(a, 8), io::bam_sar(a, 7)));
            const int32_t accel = cmd >= 0 ? std::min(acc, cmd)
                                           : std::max(io::bam_sub(0, acc), cmd);
            // Retail consumes the full Euler matrix from the PREVIOUS platform
            // solve. Roll contributes the capsize up[2] gate; pitch tilts the
            // forward thrust into X/Y/Z. The new solve runs later in this tick,
            // after position integration [orig: @0x48E972..0x48E9FF].
            const VehicleEulerBasisQ16 basis = vehicle_euler_basis_q16(
                    m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
            if (basis.up_z > 0) {
                m.vel_x += static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_x +
                                0x8000) >> 16);
                m.vel_y += static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_y +
                                0x8000) >> 16);
                vertical_thrust = static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_z +
                                0x8000) >> 16);
            }
        }
    }

    // ---- 5. Drag / slip / keel [orig: @0x48EA14..0x48EBB3, FPU-reconstructed].
    {
        m.vel_x -= m.vel_x >> 6;
        m.vel_y -= m.vel_y >> 6;
        const int32_t vx = m.vel_x, vy = m.vel_y;
        const int32_t vel_heading = bam_of_atan2(double(vy), double(vx));
        const int32_t slip = io::bam_sub(m.yaw_bam, vel_heading);
        const int32_t s22 = sin22_of_bam_x87(slip);
        const int32_t c22 = cos22_of_bam_x87(slip);
        const double dm = std::sqrt(double(vx) * double(vx) +
                                    double(vy) * double(vy));
        int32_t mag = dm >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(dm);
        if (mag > 0x10000) mag = 0x10000; // 1.0 u/tick planar clamp
        const int32_t lateral = static_cast<int32_t>(
                (static_cast<int64_t>(s22) * mag) >> 22); // no rounding bias
        const int32_t along = static_cast<int32_t>(
                (static_cast<int64_t>(c22) * mag) >> 22);
        m.speed = along; // currentSpeed — signed, negative in reverse
        // Keel: bleed 1/32 of the cross-track speed along the beam axis. The
        // beam constant is the raw 0x3FFFFFC0 (+90 deg minus 0x40 under the
        // pi=0x7FFF8000 convention) — port verbatim, do not "fix" it.
        const int32_t beam = io::bam_add(m.yaw_bam, 0x3FFFFFC0);
        const int32_t bs22 = sin22_of_bam_x87(beam);
        const int32_t bc22 = cos22_of_bam_x87(beam);
        m.vel_x += static_cast<int32_t>(
                (static_cast<int64_t>(lateral >> 5) * bc22) >> 22);
        m.vel_y += static_cast<int32_t>(
                (static_cast<int64_t>(lateral >> 5) * bs22) >> 22);
        if (along > m.cmd_speed) { // overspeed shed
            m.vel_x -= m.vel_x >> 6;
            m.vel_y -= m.vel_y >> 6;
        }
    }

    // ---- 6. Contact drags [orig: @0x48EBB5..0x48ECA6]. This tick consumes
    // the prior platform solve's authoritative afloat latch [orig: test
    // Flags 0x8000 @0x48EBB5]. NOT afloat (landed AND airborne): planar
    // sheds + yaw-rate shed + gravity slideDecay -= 167 [orig:
    // @0x48EBD3..0x48EC13, the -167 @0x48EBD9]; afloat + at-rest: the heave
    // counterweight slideDecay -= 8350 [orig: @0x48EBBE..0x48EBC7]; afloat +
    // moving: no vertical write. A BOXLESS row (the solve early-returns)
    // keeps the terrain-derived stand-in and no gravity (Z stays
    // chase-owned there).
    const bool solve_active =
            traits.box_z_hi != traits.box_z_lo && traits.box_y_hi != traits.box_y_lo;
    {
        bool afloat;
        if (solve_active) {
            afloat = m.plat_afloat;
        } else {
            afloat = false;
            int32_t ground_here = INT32_MIN;
            if (world.terrain != nullptr) {
                const int32_t pos3[3] = {px, py, pz};
                const GroundClearance clearance{};
                ground_here =
                        calc_average_ground_height(*world.terrain, pos3, 0, clearance);
            }
            if (world.env.water_z != 0 && ground_here != INT32_MIN)
                afloat = ground_here < world.env.water_z;
            else if (world.env.water_z != 0 && world.terrain == nullptr)
                afloat = true; // headless/no-terrain world with water: float
        }
        if (!afloat) {
            m.vel_x -= (m.vel_x + 4) >> 3;
            m.vel_y -= (m.vel_y + 4) >> 3;
            m.wheel_rate_bam = io::bam_sub(
                    m.wheel_rate_bam,
                    io::bam_sar(io::bam_add(m.wheel_rate_bam, 2), 2));
            if (solve_active) m.slide_z -= 167; // [orig: @0x48EBD9]
        } else if (solve_active && m.plat_at_rest) {
            m.slide_z -= 8350; // [orig: @0x48EBC7]
        }
        // Shore look-ahead: the witnessed single bilinear sample at the NEXT
        // position [orig: Terrain_SampleHeightBilinear(pos + vel)
        // @0x48EC19..0x48EC2D] — radius 0 with default clearance collapses
        // calc_average_ground_height to exactly that center-only column.
        if (world.terrain != nullptr && world.env.water_z != 0) {
            const int32_t ahead3[3] = {px + m.vel_x, py + m.vel_y, pz};
            const GroundClearance clearance{};
            const int32_t ground =
                    calc_average_ground_height(*world.terrain, ahead3, 0, clearance);
            if (ground != INT32_MIN && ground >= world.env.water_z) {
                m.vel_x -= (m.vel_x + 4) >> 3;
                m.vel_y -= (m.vel_y + 4) >> 3;
                m.wheel_rate_bam = io::bam_sub(
                        m.wheel_rate_bam,
                        io::bam_sar(io::bam_add(m.wheel_rate_bam, 2), 2));
                if (vertical_thrust > 0 && ground - world.env.water_z > 30583 &&
                        !veh.ground_target.valid()) {
                    m.vel_x = 0;
                    m.vel_y = 0;
                    m.wheel_rate_bam = 0;
                }
            }
        }
    }

    // ---- 7. Integration -> solve -> yaw, the witnessed order
    // [orig: Position += velocity (X/Y/Z all UNCONDITIONAL)
    // @0x48ECA8..0x48ECC6; the wheel-rate self-decay @0x48ECC9..0x48ECE1;
    // the platform call @0x48ECE7; Yaw += modelPtr0 AFTER it @0x48ECF2]. A
    // boxless row keeps its chase-owned Z (no slide integration).
    px += m.vel_x;
    py += m.vel_y;
    if (solve_active) pz += m.slide_z; // [orig: add [esi+0Ch] @0x48ECC6]
    {
        const int32_t r = m.wheel_rate_bam;
        m.wheel_rate_bam = io::bam_sub(
                io::bam_sub(r, io::bam_sar(io::bam_add(r, 16), 5)),
                io::bam_sar(r, 31)); // decay toward zero
    }

    veh.position.x = static_cast<float>(from_fixed(px));
    veh.position.y = static_cast<float>(from_fixed(py));
    veh.position.z = static_cast<float>(from_fixed(pz));

    // The platform solve runs every tick after integration, exactly where the
    // retail caller sits [orig: call @0x48ECE7 — after Position += velocity,
    // before the yaw apply]. It owns Z + Pitch/Roll + the afloat/airborne
    // flags from here (the chase-staged Z above is its seed, matching the
    // template's airborne-only Z-step gate). Yaw applies only AFTER this call.
    watercraft_platform_solve(world, veh, traits);
    m.yaw_bam = io::bam_add(m.yaw_bam, m.wheel_rate_bam);
    veh.yaw = static_cast<int16_t>(std::lround(
            mission_yaw_deg_from_bam_heading(m.yaw_bam)));
    // The wheel phase — the watercraft mover's inline form, run by every
    // machine that executes the core [orig: `+0x2B8 += +0x220 << 13`
    //  @0x48E9F0..0x48E9F9 inside Entity_UpdateWatercraftPhysics; the mover
    //  calls no rotor machine]. The ONE call per tick: the authority tick
    // below runs this core and adds nothing.
    vehicle_part_anim_tick(world, veh, traits);
}

// The AUTHORITY watercraft tick — the host-side cbot mover (the D-NET-161
// watercraft deferral, witnessed 2026-08-06): occupant/AI/parked input staging
// through the gate `attrib & 0x40 && (is_authority || occupant == local)`
// [orig: @0x48DF7F..0x48DFA2], then the shared core. Deferrals that stay with
// D-NET-161: the every-8th-tick groundEntity carrier refresh @0x48D51F and the
// deck-carrier follow @0x48D6DA..0x48DACD, the fire-FX/regen-drain leg (step 3),
// the MoveOrder analog bit-0x10 merge @0x48DE04..0x48DE7B (it mutates the
// occupant's own MoveOrder word, which our input model rebuilds from the wire
// every tick), the submerged-driver head-under-water input cut
// @0x48DFD3..0x48DFDF, and the wake-anim lerp @0x48ECF5.
// [orig: Entity_UpdateWatercraftPhysics @0x48D480 — the authority path]
void tick_watercraft_motor(World &world, Entity &veh, const VehicleTraits &traits,
                           const VehicleDriveCmd *ai_cmd) {
    if (traits.physics == 0) return; // selector-gated like the ground rows
    Entity::VehicleMotorState &m = veh.veh;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));
        m.air_pitch_bam = static_cast<int32_t>(veh.pitch) * 11930464;
        m.air_roll_bam = static_cast<int32_t>(veh.roll) * 11930464;
        m.yaw_seeded = true;
    }
    // The platform solve owns Z/attitude/afloat from the first tick; an authored
    // hull starts with no prior solve frame, so seed the latch exactly like the
    // client path does before its first thrust pass.
    if (!watercraft_has_platform_geometry(traits)) {
        watercraft_refresh_fallback_afloat(world, veh);
    } else if (!m.plat_solve_valid) {
        watercraft_seed_platform_latch(world, veh, traits);
    }

    // A DEAD hull skips everything to the matrix-build tail — no input, no
    // integration [orig: `test Flags, 2 -> jnz 0x48EF4B` @0x48DDFA]. Retail has
    // ONE flags word; our death chain latches the dead bit on engine_flags
    // (destruction.cpp), so read the established combined view.
    if (((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0) return;

    // Capsize damage, authority-only: past ~100 deg of roll OR pitch the hull
    // drains 200 health per tick to zero [orig: @0x48DE84..0x48DECD —
    // |Roll|/|Pitch| > 0x471C7180, Health -= 200, floor 0; the overlayFlags
    // zero at the kill edge is an unmodeled slot].
    if (veh.health > 0 &&
        (io::bam_abs(m.air_roll_bam) > 0x471C7180 ||
         io::bam_abs(m.air_pitch_bam) > 0x471C7180)) {
        veh.health -= 200;
        if (veh.health < 0) veh.health = 0;
    }

    // Sound-lane classification only. The boat ENTRY split is occupant-NULL or
    // the dead flag — the witness records no health term (a 0-hp hull that never
    // took the kill edge, e.g. the capsize drain floor, keeps driving); the
    // ground family's mode-21 health check is its own witness and stays there.
    const bool wrecked = veh.health <= 0;

    // ------------------------------------------------------------------ input block
    // [orig: the gate @0x48DF7F..0x48DFA2; occupant resolve + class split
    // @0x48DFA8..0x48DFCD — no attrib 0x40 means the whole block is skipped and
    // the core runs on the persisted registers]
    if (traits.player_control) {
        Entity *occ = resolve_vehicle_controller(world, veh);
        if (occ != nullptr && (!occ->alive || occ->health <= 0)) occ = nullptr;
        const bool player_occupant =
                occ != nullptr && occ->handle.pool() == 0 && occ->player_class != 0;
        if (occ == nullptr) {
            // Parked/no controller: hold heading, zero command and ramp, lights
            // off. The state-22 stamp lives with the brain in watercraft_ai_drive;
            // AI_CheckVehicleStuckState stays a D-NET-161 deferral.
            // [orig: @0x48E7EE..0x48E81E]
            m.steer_target_bam = m.yaw_bam;
            m.cmd_speed = 0;
            m.steer_ramp_bam = 0;
            veh.flags &= ~0x80u;
        } else if (player_occupant) {
            // The human-driver leg: 8-way keys + analog through waterSpeed, the
            // boat bit6/bit7 overrides, the 45-deg ramp cap, walk/creep halving,
            // lights [orig: @0x48DFE5..0x48E20F].
            stage_player_vehicle_input(veh, *occ, traits);
        } else if (ai_cmd != nullptr && ai_cmd->ai_drive) {
            // The AI-driver leg's outputs (AiSystem::watercraft_ai_drive)
            // [orig: @0x48E247..0x48E756 writes aiComp[132]/[136]].
            m.steer_target_bam = ai_cmd->steer_target_bam;
            m.cmd_speed = ai_cmd->cmd_speed;
            m.steer_ramp_bam = 0;
        }
        // A live non-player controller with no drive command holds the previous
        // targets (the boarding-wait stop @0x48E75B..0x48E7EC rides D-NET-161
        // with the boarding think).
    }

    watercraft_motor_core(world, veh, traits);

    // Movement-sound presentation, same per-tick site as the ground core's tail
    // [orig: the cbot movement-sound call in step 18 @0x48ED76..].
    update_ground_vehicle_sound(world, veh, traits, wrecked, /*collided=*/false);
    // The part-animation tick is the core's (@0x48E9F0..0x48E9F9 runs once
    // per mover pass); a second call here would double the wheel phase.
}

// The GROUND-family prediction leg (net-re §5.38e B-facet): run the shared
// chase, stage local-driver input or remote mirrored registers, then drive
// tick_vehicle_motor's core with its input block bypassed
// (player_control=false leaves the registers untouched and skips the occupant
// resolve; the handbrake/aim-lock/tire-slip legs inherit their existing
// D-NET-161 deferrals).
void ground_client_tick(World &world, Entity &veh, const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    if (!m.net_predicted) return;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(veh.yaw);
        m.yaw_seeded = true;
    }
    vehicle_client_chase(veh);
    // Register mirror / local-driver gate (see the watercraft mirror note).
    if (Entity *local_driver =
                resolve_local_vehicle_controller(world, veh, traits)) {
        stage_player_vehicle_input(veh, *local_driver, traits);
        // The controlling client halves its staged command against the
        // received register every tick — the same longitudinal-only
        // reconciliation as the boat; steer stays owned by current local
        // LOOK/input [orig: ground @0x48bbef; bike @0x484dad —
        // [136] = ([136] + [177]) >> 1].
        m.cmd_speed = io::bam_sar(
                io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
    } else {
        m.cmd_speed = m.net_recv_speed;
        m.steer_target_bam = m.net_recv_steer_bam;
    }
    VehicleTraits core = traits;
    core.player_control = false; // bypass the occupant/input block, keep the core
    tick_vehicle_motor(world, veh, core, nullptr);
}

// The AIR-family prediction leg (CHel + cpln — the plane callback is a thunk onto
// the same function): the client-executed subset of Entity_UpdateAircraftPhysics
// [orig: @0x490310; disasm-verified spec 2026-07-31]. Commanded speeds TILT the
// airframe and the airborne aero block converts attitude into acceleration; the
// vertical axis is an altitude-hold servo on the record-seeded target Z — there
// is no gravity constant in the air mover. Residual: the air contact/suspension
// solve (loc_47EF10, undefined in the IDB) is unported — replaced by a terrain
// clamp; the airborne/water branch picks derive locally from the ground cache
// and water plane instead of the solve's Flags 0x2000/0x8000.
// The AIR local-driver input map — the client-executed occupant block of the
// air mover, for the LOCAL PILOT of a predicted row [orig:
// Entity_UpdateAircraftPhysics @0x490310, the occupant input block; key-dir
// thrust table, analog arm, stance mods, the collective and its clamps].
// Brain registers: [544] fwd cmd, [540] lateral cmd, [548] climb-above-ground,
// [524] absolute altitude target, [528] steer heading. Our state stores the
// ABSOLUTE altitude target only (net_alt_target); the [548]/[524] split is
// carried as climb = target - ground — the same quantity retail derives at
// every near-ground boundary. Cited deferrals: the analog COLLECTIVE channel
// (`analogThrottle << 7` — a fourth analog axis our C2S uplink does not
// carry), the occupant's own analog-yaw write (D-NET-161: local look owns the
// client row), the flare-release weapon scan and the authority engine-flag
// upkeep.
static void stage_air_vehicle_input(Entity &veh, const Entity &occ,
                                    const VehicleTraits &traits,
                                    int32_t ground, int32_t pz) {
    Entity::VehicleMotorState &m = veh.veh;
    const uint32_t move_order = static_cast<uint32_t>(occ.net_move_input) |
                                (static_cast<uint32_t>(occ.net_stance_bits) << 8);
    const int analog_sum = int(occ.net_analog_x) + occ.net_analog_y +
                           occ.net_analog_z;
    const int32_t fs = traits.player_speed; // itemDef+0x8E8 — the air speed slot
    if ((move_order & Entity::kMoveOrderMoving) != 0) {
        // The 8-way key thrust table [orig: the dir switch — fwd full, every
        // diagonal/side/reverse component at HALF]:
        switch (move_order & Entity::kMoveOrderDirMask) {
            case 0: m.cmd_speed = fs;          m.cmd_lateral_speed = 0;       break;
            case 1: m.cmd_speed = fs >> 1;     m.cmd_lateral_speed = fs >> 1; break;
            case 2: m.cmd_speed = 0;           m.cmd_lateral_speed = fs >> 1; break;
            case 3: m.cmd_speed = -fs >> 1;    m.cmd_lateral_speed = fs >> 1; break;
            case 4: m.cmd_speed = -fs >> 1;    m.cmd_lateral_speed = 0;       break;
            case 5: m.cmd_speed = -fs >> 1;    m.cmd_lateral_speed = -fs >> 1; break;
            case 6: m.cmd_speed = 0;           m.cmd_lateral_speed = -fs >> 1; break;
            case 7: m.cmd_speed = fs >> 1;     m.cmd_lateral_speed = -fs >> 1; break;
            default: break;
        }
    } else {
        // Analog cyclic: fwd = -(fs*x)>>7, lateral = -(fs*y)>>8 (half-scale),
        // steer walks with analog yaw [orig: the analog arm — the occupant
        // Yaw -= write rides D-NET-161].
        m.cmd_speed = -(fs * static_cast<int32_t>(occ.net_analog_x)) >> 7;
        m.cmd_lateral_speed = -(fs * static_cast<int32_t>(occ.net_analog_y)) >> 8;
        m.steer_target_bam = io::bam_sub(
                m.steer_target_bam,
                (kAnalogSteerScale * static_cast<int32_t>(occ.net_analog_z)) >> 1);
    }
    if ((move_order & Entity::kMoveOrderCrouch) != 0) {
        m.cmd_speed >>= 1;
        m.cmd_lateral_speed >>= 1;
    }
    if ((move_order & Entity::kMoveOrderProne) != 0) {
        m.cmd_speed >>= 2;
        m.cmd_lateral_speed >>= 2;
    }
    // The collective (the 0x40/0x80 MoveOrder pair — the lean bits' air
    // meaning): 0x4000/tick down/up on the altitude target, with the 256 u
    // climb ceiling and the ground floor. Retail splits the write across the
    // near-ground boundary (2 * boundRadius) between the climb register and
    // the absolute target; both sides reduce to the same absolute step.
    if (ground != INT32_MIN) {
        if ((move_order & 0x40u) != 0) m.net_alt_target -= 0x4000;
        if ((move_order & 0x80u) != 0) m.net_alt_target += 0x4000;
        int32_t climb = m.net_alt_target - ground;
        // The ceiling is ABSOLUTE, not above-ground: retail tests
        // `ground + climb > 0x1000000` and parks the target AT 0x1000000, so a
        // helicopter over a 100 u ridge may only climb 156 u, not another 256.
        // [orig: Entity_UpdateAircraftPhysics @0x4912a0 — the LABEL_175 clamp
        //  pair, `v78 + [548] > 0x1000000 -> [548] = 0x1000000 - v78,
        //  [524] = 0x1000000` with v78 the average ground height]
        if (ground + climb > 0x1000000) {
            climb = 0x1000000 - ground;
            m.net_alt_target = 0x1000000;
        }
        if (climb < 0) {
            // The companion clamp, and ALL it does: a target below the ground
            // floors AT the ground. It is a floor, not a shutdown -- retail
            // neither zeroes the commands here nor abandons the rest of the
            // input staging, which is why a pilot can still slide along at
            // hover height with the collective held down.
            //
            // We used to fold retail's separate engine-off landed reset into
            // this arm (parking the target at ground - 0x4000, zeroing both
            // commands, pinning the steer, and returning early). That reset is
            // a DIFFERENT branch on a different gate, and the rotor start-up
            // hold now covers the case that folding was standing in for.
            // [orig: LABEL_175 @0x4912xx -- `if ([548] < 0) { [548] = 0;
            //  [524] = v78; }` with v78 the average ground height]
            m.net_alt_target = ground;
        }
    }
    (void)pz;
    // Hover/flight steer source: no analog input -> the pilot's LOOK steers
    // (freelook holds the hull heading instead).
    if (analog_sum == 0) {
        m.steer_target_bam =
                (move_order & Entity::kMoveOrderFreeLook) != 0
                        ? m.yaw_bam
                        : bam_heading_from_mission_yaw_deg(
                                  static_cast<double>(occ.yaw));
    }
}

void aircraft_client_tick(World &world, Entity &veh, const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    // Authority AI flight: chel_ai_drive staged this tick's commands; run the
    // same servos/integration the predicted path uses, skipping the client
    // interp/mirror blocks. Retail is ONE function for both.
    // [orig: Entity_UpdateAircraftPhysics @0x490310]
    const bool ai_drive = m.ai_drive;
    m.ai_drive = false;
    // Retail runs this mover for every aircraft row unconditionally - the class
    // table dispatches it and the occupant-input block gates itself. Our
    // net_predicted/ai_drive pair is a reimpl guard, so a PLAYER-piloted row has
    // to be admitted explicitly or the pilot commands nothing.
    // [orig: the class-table dispatch -> Entity_UpdateAircraftPhysics @0x490310]
    const bool player_piloted =
            resolve_piloting_player(world, veh, traits) != nullptr;
    if (!m.net_predicted && !ai_drive && !player_piloted) return;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(veh.yaw);
        m.yaw_seeded = true;
    }
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);
    // The mover-entry pose — savedLivePose for the solve's sleep compare
    // [orig: the prologue 6-dword capture @0x490326..0x49036B].
    const int32_t start_x = px, start_y = py;

    // Ground cache — the every-8th-tick sample [orig: entity+0x2A4 @0x4903A8..];
    // staggering is a load-spreading detail, refreshed here per tick when cheap.
    // The sample runs at Z - brain[11]: retail subtracts the probe offset,
    // samples, and restores Z [orig: @0x4903AD..0x4903D8].
    if (world.terrain != nullptr) {
        const int32_t pos3[3] = {px, py, pz - m.air_probe_z_off};
        const GroundClearance clearance{};
        const int32_t g =
                calc_average_ground_height(*world.terrain, pos3, 0, clearance);
        if (g != INT32_MIN) m.ground_cache = g;
    }
    const int32_t ground = m.ground_cache;

    // ---- 1. The air interp block [orig: @0x49095E..0x490C98]. 3D distance,
    // snap 0xA0000 (0x20000 when BOTH received cmds < 293), buckets
    // {8,10,15,20,25,32}, yaw (d+10)/20 over 20 ticks, Z stepped like X/Y.
    // The authority AI leg skips it: no wire targets exist on the host row.
    // The interp block consumes RECEIVED state, so it belongs only to rows the
    // wire drives. Before player pilots reached this function, `!ai_drive`
    // implied net_predicted by construction; admitting them broke that
    // invariant and snapped a locally-piloted hull to the never-received
    // smooth target at the world origin. Gate it explicitly.
    // [orig: @0x49095E..0x490C98 is the CLIENT interp leg]
    if (m.net_predicted && !ai_drive && m.net_interp_progress == 0) {
        const int64_t dx = int64_t(m.net_smooth_target[0]) - px;
        const int64_t dy = int64_t(m.net_smooth_target[1]) - py;
        const int64_t dz = int64_t(m.net_smooth_target[2]) - pz;
        const double dd = std::sqrt(double(dx) * double(dx) +
                                    double(dy) * double(dy) +
                                    double(dz) * double(dz));
        const int32_t dist = dd >= 2147418112.0 ? INT32_MAX
                                                : static_cast<int32_t>(dd);
        const int32_t snap =
                (m.net_recv_speed < 293 && m.net_recv_lat < 293) ? 0x20000
                                                                  : 0xA0000;
        // Altitude register seeding [orig: @0x490998..0x490B33]: engine on ->
        // target Z exactly; engine off -> target Z - 0x4000. (brain[137] is
        // client-unused; the ground-at-target sample is skipped with it.)
        m.net_alt_target = m.net_engine_on
                ? m.net_smooth_target[2]
                : m.net_smooth_target[2] - 0x4000;
        if (dist > snap) {
            px = m.net_smooth_target[0];
            py = m.net_smooth_target[1];
            pz = m.net_smooth_target[2];
            m.yaw_bam = m.net_smooth_heading;
            m.net_smooth_target[0] = 0;
            m.net_smooth_target[1] = 0;
            m.net_smooth_target[2] = 0;
            m.net_interp_steps = 0;
            m.net_smooth_heading = 0;
        } else if (dist < 0x2AAA) {
            m.net_smooth_target[0] = 0;
            m.net_smooth_target[1] = 0;
            m.net_smooth_target[2] = 0;
            m.net_interp_steps = 0;
            m.net_smooth_heading = io::bam_add(
                    io::bam_sub(m.net_smooth_heading, m.yaw_bam), 10) / 20;
        } else {
            int32_t n;
            if (dist < 0x4000) n = 8;
            else if (dist < 0x5555) n = 10;
            else if (dist < 0x8000) n = 15;
            else if (dist < 0x10000) n = 20;
            else if (dist < 0x20000) n = 25;
            else n = 32;
            m.net_interp_steps = static_cast<int16_t>(n);
            m.net_smooth_target[0] = (int32_t(dx) + (n >> 1)) / n;
            m.net_smooth_target[1] = (int32_t(dy) + (n >> 1)) / n;
            m.net_smooth_target[2] = (int32_t(dz) + (n >> 1)) / n;
            m.net_smooth_heading = io::bam_add(
                    io::bam_sub(m.net_smooth_heading, m.yaw_bam), 10) / 20;
        }
    }
    if (!ai_drive) {
        const int16_t progress = m.net_interp_progress;
        if (progress < 20)
            m.yaw_bam = io::bam_add(m.yaw_bam, m.net_smooth_heading);
        if (progress < m.net_interp_steps) {
            px += m.net_smooth_target[0];
            py += m.net_smooth_target[1];
            pz += m.net_smooth_target[2];
        }
        if (progress >= 128) {
            // BOTH mirrored commands coast; steer and altitude never decay —
            // an abandoned aircraft predicts to a hover [orig: @0x490C64..].
            m.net_recv_speed -= (m.net_recv_speed + 64) >> 7;
            m.net_recv_lat -= (m.net_recv_lat + 64) >> 7;
        } else {
            m.net_interp_progress = static_cast<int16_t>(progress + 1);
        }
    }

    // ---- 2. Register mirror [orig: @0x490C9E..0x490CCA]: the non-pilot
    // machine adopts the received commands verbatim; the LOCAL PILOT'S
    // machine runs the input block instead and reconciles BOTH air commands
    // with the received mirrors — ([544]+[708])>>1 fwd, ([540]+[712])>>1
    // lateral [orig: the occupantEntity == g_local_player_entity leg —
    // `([2C4]+[220])>>1 -> [220]; ([2C8]+[21C])>>1 -> [21C]`
    // @0x491546..0x491568; the input block is stage_air_vehicle_input above].
    if (ai_drive) {
        // Commands already staged by AiSystem::chel_ai_drive — the AI leg fills
        // the same registers the pilot input block does. [orig: one function]
    } else if (Entity *local_pilot =
                       resolve_piloting_player(world, veh, traits)) {
        stage_air_vehicle_input(veh, *local_pilot, traits, ground, pz);
        // The blend reconciles the pilot's staged command against what the
        // SERVER echoed back, so it only means anything on a row the wire
        // drives. An authority-owned hull receives nothing, and blending
        // against a zero mirror halves the pilot's command every tick.
        if (m.net_predicted) {
            m.cmd_speed = io::bam_sar(
                    io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
            m.cmd_lateral_speed = io::bam_sar(
                    io::bam_add(m.cmd_lateral_speed, m.net_recv_lat), 1);
        }
    } else if (m.net_predicted) {
        m.cmd_speed = m.net_recv_speed;
        m.cmd_lateral_speed = m.net_recv_lat;
        m.steer_target_bam = m.net_recv_steer_bam;
    }

    // ---- 2a. Engine flag. Retail splits this by role: the AUTHORITY DERIVES the
    // flag from the climb-above-ground register every tick, and only a CLIENT
    // runs the engine-off override (`if (!is_authority) goto LABEL_305`). We
    // fold [548] into the absolute target, so the climb is
    // (net_alt_target - ground) and the derivation is the same test against it.
    //
    // Without this upkeep a player-piloted aircraft deadlocks: the override
    // zeroes the cyclic because the engine reads off, and nothing ever turns the
    // engine on. It was recorded as the deferred "authority engine-flag upkeep".
    // [orig: Entity_UpdateAircraftPhysics @0x490310 — the brain[137] (+0x224)
    //  test @0x491dfd, `Flags |= 0x80` @0x491e05 / `&= ~0x80` @0x491e11, reached
    //  only when is_authority; the client engine-off override is the other arm
    //  (its `&= ~0x80` @0x491c6d precedes the is_authority test @0x491c88)]
    const bool motor_is_authority = world.ai != nullptr && world.ai->is_authority;
    if (motor_is_authority) {
        const int32_t climb =
                ground != INT32_MIN ? io::bam_sub(m.net_alt_target, ground) : 0;
        m.net_engine_on = climb != 0;
    } else if (!m.net_engine_on) {
        if (ground != INT32_MIN) m.net_alt_target = ground - 0x2000;
        m.cmd_speed = 0;
        m.cmd_lateral_speed = 0;
        m.steer_target_bam = m.yaw_bam;
    }

    // ---- 3. Yaw servo (second order) [orig: @0x491CC8..0x491D27].
    {
        int32_t step = io::bam_sar(io::bam_add(
                io::bam_sub(m.steer_target_bam, m.yaw_bam), 8), 4);
        const int32_t tr = traits.turn_rate;
        if (step > tr) step = tr;
        const int32_t neg_tr = io::bam_sub(0, tr);
        if (step < neg_tr) step = neg_tr;
        m.wheel_rate_bam = io::bam_add(
                m.wheel_rate_bam, io::bam_sar(io::bam_add(step, 4), 3));
        const int32_t astep = io::bam_abs(step);
        if (m.wheel_rate_bam > astep) m.wheel_rate_bam = astep;
        const int32_t neg_astep = io::bam_sub(0, astep);
        if (m.wheel_rate_bam < neg_astep) m.wheel_rate_bam = neg_astep;
    }

    // ---- 4. Tilt commands + climb servo [orig: @0x491D2D..0x491E2F].
    {
        const int32_t cap = bam_shl_wrap(traits.acceleration, 12);
        int32_t fwd = bam_shl_wrap(m.cmd_speed, 11);
        int32_t lat = bam_shl_wrap(m.cmd_lateral_speed, 11);
        if (fwd > cap) fwd = cap;
        const int32_t neg_cap = io::bam_sub(0, cap);
        if (fwd < neg_cap) fwd = neg_cap;
        if (lat > cap) lat = cap;
        if (lat < neg_cap) lat = neg_cap;
        m.air_pitch_rate = io::bam_sub(m.air_pitch_rate, fwd);
        m.air_roll_rate = io::bam_sub(m.air_roll_rate, lat);
        if (to_fixed(veh.bound_radius) >= 0xF0000)
            m.slide_z += (m.net_alt_target - pz + 0x100) >> 9; // heavy 1/512
        else
            m.slide_z += (m.net_alt_target - pz + 0x80) >> 8;  // light 1/256
    }

    // Airborne/water picks: the contact solve (below, at the tick tail)
    // produced Flags 0x2000/0x8000 LAST tick — the witnessed source for these
    // branches [orig: the mover reads Flags @0x491E35 region]. Boxless rows
    // (the solve stand-in) keep the local derivation.
    const bool solve_active = air_contact_solve_active(traits);
    const bool airborne = solve_active
            ? (veh.flags & kEntityFlagInAir) != 0
            : (ground == INT32_MIN || pz - ground > 0x10000);
    const bool in_water = solve_active
            ? (veh.flags & 0x8000u) != 0
            : (world.env.water_z != 0 && pz <= world.env.water_z);

    if (airborne) {
        // ---- 5. Airborne aero block [orig: @0x491E35..0x4922BC].
        const int32_t vx = m.vel_x, vy = m.vel_y;
        const int32_t vel_heading = bam_of_atan2(double(vy), double(vx));
        const int32_t slip = io::bam_sub(m.yaw_bam, vel_heading);
        const int32_t s22 = sin22_of_bam_x87(slip);
        const int32_t c22 = cos22_of_bam_x87(slip);
        const double dm = std::sqrt(double(vx) * double(vx) +
                                    double(vy) * double(vy));
        const int32_t mag =
                dm >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(dm);
        const int32_t lateral = static_cast<int32_t>(
                (static_cast<int64_t>(s22) * mag) >> 22);
        const int32_t along = static_cast<int32_t>(
                (static_cast<int64_t>(c22) * mag) >> 22);
        // `along` stays a LOCAL: the witnessed aircraft mover never writes
        // currentSpeed (+0x29C is absent from the §14 write set), which keeps
        // the contact solve's severity sheds inert (0 stays 0) and the §15
        // sleep gate's `currentSpeed == 0` genuinely reachable after landing.
        // An earlier `m.speed = along` here was an uncited addition that made
        // the sheds live and could starve the sleep fast-path forever.
        const int32_t alat = io::bam_abs(lateral);
        const int32_t aclat = io::bam_abs(m.cmd_lateral_speed);
        if (alat > aclat)
            m.air_roll_rate = io::bam_add(
                    m.air_roll_rate,
                    io::bam_sub(bam_shl_wrap(m.cmd_lateral_speed, 3),
                                bam_shl_wrap(lateral, 3)));
        const int32_t aalong = io::bam_abs(along);
        const int32_t acmd = io::bam_abs(m.cmd_speed);
        if (aalong > acmd) {
            const int32_t e = along - m.cmd_speed;
            const int32_t p32 = io::bam_add(
                    m.air_pitch_bam, bam_shl_wrap(e, 5));
            const int32_t ap32 = io::bam_abs(p32);
            const int32_t ap = io::bam_abs(m.air_pitch_bam);
            if (ap32 < ap) {
                m.air_pitch_rate = io::bam_add(
                        m.air_pitch_rate, bam_shl_wrap(e, 4));
                m.air_pitch_bam = io::bam_add(
                        m.air_pitch_bam, bam_shl_wrap(e, 5));
            } else {
                m.air_pitch_rate = io::bam_add(
                        m.air_pitch_rate, bam_shl_wrap(e, 2));
                m.air_pitch_bam = io::bam_add(
                        m.air_pitch_bam, bam_shl_wrap(e, 2));
            }
        }
        // Weathervane [orig: @0x491FA8..0x491FD9] (the occupant-analog steer
        // feedback is input-leg, absent here). The multiplier is
        // abs(lateral >> 6) — retail shifts FIRST, then takes the absolute
        // value, one larger than (abs >> 6) for negative non-multiples of 64.
        {
            const int32_t wv = lateral >> 6;
            const int32_t yaw_error = io::bam_sub(vel_heading, m.yaw_bam);
            m.yaw_bam = io::bam_add(m.yaw_bam, static_cast<int32_t>(
                    (static_cast<int64_t>(io::bam_abs(wv)) * yaw_error +
                            0x8000) >> 16));
        }
        // Tilt -> acceleration in the yaw frame [orig: @0x492006..0x492152].
        const int32_t a_fwd = -static_cast<int32_t>(
                (1169LL * sin22_of_bam_x87(m.air_pitch_bam)) >> 22);
        const int32_t roll_k = alat < aclat ? 501 : 334;
        const int32_t a_lat = static_cast<int32_t>(
                (static_cast<int64_t>(roll_k) *
                         sin22_of_bam_x87(m.air_roll_bam)) >> 22);
        int32_t zp = static_cast<int32_t>(
                (static_cast<int64_t>(along) *
                         sin22_of_bam_x87(m.air_pitch_bam)) >> 22);
        if (zp < 0) zp >>= 2;
        m.slide_z += zp >> 2;
        int32_t zr = static_cast<int32_t>(
                (static_cast<int64_t>(lateral) *
                         sin22_of_bam_x87(m.air_roll_bam)) >> 22);
        if (zr < 0) zr >>= 2;
        m.slide_z -= zr >> 3;
        const int32_t sy = sin22_of_bam_x87(m.yaw_bam);
        const int32_t cy = cos22_of_bam_x87(m.yaw_bam);
        m.vel_x += static_cast<int32_t>((static_cast<int64_t>(a_fwd) * cy) >> 22) +
                   static_cast<int32_t>((static_cast<int64_t>(a_lat) * sy) >> 22);
        m.vel_y += static_cast<int32_t>((static_cast<int64_t>(a_fwd) * sy) >> 22) -
                   static_cast<int32_t>((static_cast<int64_t>(a_lat) * cy) >> 22);
        m.vel_x = static_cast<int32_t>((1019LL * m.vel_x + 512) >> 10);
        m.vel_y = static_cast<int32_t>((1019LL * m.vel_y + 512) >> 10);
        // Deferred: retail applies a SECOND vel_x/vel_y 1019/1024 pass when the
        // occupant exists WITHOUT the in-control flag [orig: @0x49217E..
        // 0x4921C7, occupant && !(occupant->Flags & 0x100)]. Occupant flags are
        // not replicated to a joiner; remote pilots normally carry the flag,
        // which skips the pass — revisit with the occupant mirror gate below.
        m.slide_z = static_cast<int32_t>((240LL * m.slide_z + 128) >> 8);
        // Attitude self-righting + rate damping [orig: @0x4921CD..0x492246].
        const int32_t pitch_right = io::bam_sar(
                io::bam_add(m.air_pitch_bam, 0x100), 9);
        const int32_t roll_right = io::bam_sar(
                io::bam_add(m.air_roll_bam, 0x100), 9);
        m.air_pitch_rate = io::bam_sub(m.air_pitch_rate, pitch_right);
        m.air_pitch_bam = io::bam_sub(m.air_pitch_bam, pitch_right);
        m.air_roll_rate = io::bam_sub(m.air_roll_rate, roll_right);
        m.air_roll_bam = io::bam_sub(m.air_roll_bam, roll_right);
        m.air_roll_rate = io::bam_sub(
                m.air_roll_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_roll_rate, 8), 4),
                            io::bam_sar(m.air_roll_rate, 31)));
        m.air_pitch_rate = io::bam_sub(
                m.air_pitch_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_pitch_rate, 8), 4),
                            io::bam_sar(m.air_pitch_rate, 31)));
        if (traits.speed_pitch != 0) {
            const int32_t pc = bam_mul_wrap(traits.speed_pitch, 192426);
            if (m.air_pitch_rate > pc) m.air_pitch_rate = pc;
            const int32_t neg_pc = io::bam_sub(0, pc);
            if (m.air_pitch_rate < neg_pc) m.air_pitch_rate = neg_pc;
        }
        if (traits.turn_roll != 0) {
            const int32_t rc = bam_mul_wrap(traits.turn_roll, 192426);
            if (m.air_roll_rate > rc) m.air_roll_rate = rc;
            const int32_t neg_rc = io::bam_sub(0, rc);
            if (m.air_roll_rate < neg_rc) m.air_roll_rate = neg_rc;
        }
    } else {
        // ---- 6. Grounded shed block [orig: @0x4922C1..0x492378].
        m.vel_x -= ((m.vel_x + 2) >> 2) + (m.vel_x >> 31);
        m.vel_y -= ((m.vel_y + 2) >> 2) + (m.vel_y >> 31);
        m.slide_z -= ((m.slide_z + 8) >> 4) + (m.slide_z >> 31);
        if (m.slide_z < 0) m.slide_z >>= 2;
        m.wheel_rate_bam = io::bam_sub(
                m.wheel_rate_bam,
                io::bam_add(io::bam_sar(io::bam_add(m.wheel_rate_bam, 4), 3),
                            io::bam_sar(m.wheel_rate_bam, 31)));
        m.air_roll_rate = io::bam_sub(
                m.air_roll_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_roll_rate, 4), 3),
                            io::bam_sar(m.air_roll_rate, 31)));
        m.air_pitch_rate = io::bam_sub(
                m.air_pitch_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_pitch_rate, 4), 3),
                            io::bam_sar(m.air_pitch_rate, 31)));
        m.net_alt_target += (pz - m.net_alt_target) >> 2;
    }

    // ---- 7. Common tail [orig: @0x49237E..0x492776].
    if (m.cmd_speed == 0 && m.cmd_lateral_speed == 0) {
        if (io::bam_abs(m.vel_x) < 384) m.vel_x = 0;
        if (io::bam_abs(m.vel_y) < 384) m.vel_y = 0;
    }
    // Unconditional [orig: @0x4923C0..0x4923E9] — a zero climb_speed def pins
    // the vertical rate to exactly 0 (both bounds collapse), the witnessed
    // degenerate behavior; never guard it away.
    if (m.slide_z > traits.climb_speed) m.slide_z = traits.climb_speed;
    if (m.slide_z < -2 * traits.climb_speed)
        m.slide_z = -2 * traits.climb_speed;
    if (!m.net_engine_on) {
        m.air_roll_rate = io::bam_sub(
                m.air_roll_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_roll_rate, 8), 4),
                            io::bam_sar(m.air_roll_rate, 31)));
        m.air_pitch_rate = io::bam_sub(
                m.air_pitch_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_pitch_rate, 8), 4),
                            io::bam_sar(m.air_pitch_rate, 31)));
    }
    if (in_water) {
        m.vel_x -= ((m.vel_x + 4) >> 3) + (m.vel_x >> 31);
        m.vel_y -= ((m.vel_y + 4) >> 3) + (m.vel_y >> 31);
        m.slide_z -= ((m.slide_z + 4) >> 3) + (m.slide_z >> 31);
        m.wheel_rate_bam = io::bam_sub(
                m.wheel_rate_bam,
                io::bam_add(io::bam_sar(io::bam_add(m.wheel_rate_bam, 8), 4),
                            io::bam_sar(m.wheel_rate_bam, 31)));
        m.air_roll_rate = io::bam_sub(
                m.air_roll_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_roll_rate, 8), 4),
                            io::bam_sar(m.air_roll_rate, 31)));
        m.air_pitch_rate = io::bam_sub(
                m.air_pitch_rate,
                io::bam_add(io::bam_sar(io::bam_add(m.air_pitch_rate, 8), 4),
                            io::bam_sar(m.air_pitch_rate, 31)));
    }
    px += m.vel_x;
    py += m.vel_y;
    pz += m.slide_z;
    {
        const int32_t r = m.wheel_rate_bam;
        m.wheel_rate_bam = io::bam_sub(
                io::bam_sub(r, io::bam_sar(io::bam_add(r, 16), 5)),
                io::bam_sar(r, 31));
    }
    // The contact/suspension solve at the witnessed call site — after
    // integration, before the attitude-rate integration [orig: @0x49254E].
    // It owns Flags 0x2000/0x8000, the grounded Z/attitude conform, and the
    // planar slope/wall separation; boxless rows keep the terrain-clamp
    // stand-in inside.
    aircraft_contact_solve(world, veh, traits, m, start_x, start_y, px, py, pz);
    constexpr int32_t kAttitudeRateClamp = 178956960; // 0xAAAAAA0
    if (m.wheel_rate_bam > kAttitudeRateClamp) m.wheel_rate_bam = kAttitudeRateClamp;
    if (m.wheel_rate_bam < -kAttitudeRateClamp) m.wheel_rate_bam = -kAttitudeRateClamp;
    if (m.air_pitch_rate > kAttitudeRateClamp) m.air_pitch_rate = kAttitudeRateClamp;
    if (m.air_pitch_rate < -kAttitudeRateClamp) m.air_pitch_rate = -kAttitudeRateClamp;
    if (m.air_roll_rate > kAttitudeRateClamp) m.air_roll_rate = kAttitudeRateClamp;
    if (m.air_roll_rate < -kAttitudeRateClamp) m.air_roll_rate = -kAttitudeRateClamp;
    m.air_pitch_bam = io::bam_add(m.air_pitch_bam, m.air_pitch_rate);
    m.air_roll_bam = io::bam_add(m.air_roll_bam, m.air_roll_rate);
    m.yaw_bam = io::bam_add(m.yaw_bam, m.wheel_rate_bam);

    veh.position.x = static_cast<float>(from_fixed(px));
    veh.position.y = static_cast<float>(from_fixed(py));
    veh.position.z = static_cast<float>(from_fixed(pz));
    veh.yaw = static_cast<int16_t>(std::lround(
            mission_yaw_deg_from_bam_heading(m.yaw_bam)));
    // ...and the attitude with it. In the original these ARE the entity's own
    // Pitch/Roll -- the integration above writes entity+0x14/+0x18 directly, so
    // there is no separate motor copy to publish. Our split kept
    // air_pitch_bam/air_roll_bam private and only ever mirrored them from the
    // GROUND conform, which left the whole aerodynamic bank computed and then
    // discarded on the host: a helicopter turned and slid sideways with the
    // hull dead level, and the mounted camera (which reads entity roll) stayed
    // level with it.
    //
    // Republishing the value the contact solve wrote is harmless on the ground
    // and matches retail, whose common tail integrates unconditionally too with
    // the next tick's conform overwriting.
    // [orig: Entity_UpdateAircraftPhysics common tail @0x49237E --
    //  `entity+20 += entity+168; entity+24 += entity+172`]
    veh.pitch = static_cast<int16_t>(std::lround(
            static_cast<double>(m.air_pitch_bam) * kDegreesPerBam));
    veh.roll = static_cast<int16_t>(std::lround(
            static_cast<double>(m.air_roll_bam) * kDegreesPerBam));
    // The part-animation accumulators — the air mover's tail call [orig:
    // Entity_UpdatePartSpinAccumulator @0x4928B0 from the CHel/cpln callback].
    vehicle_part_anim_tick(world, veh, traits);
}

} // namespace opennova::world
