#include "world/vehicle_motor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "world/ai.h"
#include "world/angle.h"
#include "world/geom.h"
#include "world/vehicle_sound.h"
#include "world/world.h"

namespace opennova::world {

namespace {

// Key-steer ramp: +2.0 deg/tick, capped at 50 deg [orig: @0x48b4e0 — `[137] +=
// 0x16C16C0` while `< 0x238E38C0`]. Constants verbatim.
constexpr int32_t kSteerRampStep = 0x16C16C0;
constexpr int32_t kSteerRampCap = 596523200; // 0x238E38C0

// Gravity on the vertical velocity, 16.16 u/tick per tick [orig: @0x48d69b
// `slideDecay -= 324`]. The cbik mover uses 250 [orig: @0x4865a6] — remote
// bikes fell ~30% too fast riding the ground constant.
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

// Boat/air family trig: retail computes THESE movers' sin/cos with x87
// fsin/fcos scaled by the verbatim BAM->radian constant dbl_7C3608 =
// 1.4629627251502471e-9 (~pi/0x7FF..., deliberately NOT the exact inverse of
// the atan2 scale dbl_7C19D8 — port both constants verbatim)
// [orig: fld dbl_7C3608 @0x48EB32/@0x4905BF/@0x492006; the ground family
// keeps the 1024-entry table equivalents above (D-INF-4)].
constexpr double kBamToRadX87 = 1.4629627251502471e-9; // dbl_7C3608, exact bits
int32_t cos22_of_bam_x87(int32_t bam) {
    return static_cast<int32_t>(
            std::cos(static_cast<double>(bam) * kBamToRadX87) * 4194304.0);
}
int32_t sin22_of_bam_x87(int32_t bam) {
    return static_cast<int32_t>(
            std::sin(static_cast<double>(bam) * kBamToRadX87) * 4194304.0);
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

void tick_vehicle_motor(World &world, Entity &veh, const VehicleTraits &traits,
                        const VehicleDriveCmd *ai_cmd) {
    if (traits.physics == 0) return; // no vehicle physics selected [orig: @0x48efc7]

    Entity::VehicleMotorState &m = veh.veh;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));
        m.yaw_seeded = true;
    }

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
            // The above-water gate (`occ->Position.Z + CameraOffset.Z > waterHeight`
            // [orig: the player-leg head @0x48b993]) is unmodeled — no world water
            // height; divergence noted in D-NET-161.
            const uint32_t move_order = static_cast<uint32_t>(occ->net_move_input) |
                                        (static_cast<uint32_t>(occ->net_stance_bits) << 8);
            const int dir = static_cast<int>(move_order & 7u);
            const bool moving = ((move_order >> 3) & 1u) != 0;
            const int32_t analog_sum = static_cast<int32_t>(occ->net_analog_x) +
                                       static_cast<int32_t>(occ->net_analog_y) +
                                       static_cast<int32_t>(occ->net_analog_z);
            const int32_t driver_yaw_bam =
                    bam_heading_from_mission_yaw_deg(static_cast<double>(occ->yaw));

            if (moving) {
                m.cmd_speed = traits.player_speed; // [orig: @0x48b3d3 `[136] = playerSpeed`]
            } else {
                // The analog leg runs for every non-moving frame (axes 0 -> both terms
                // vanish) [orig: @0x48b783-0x48b7c6].
                int32_t steer_delta =
                        (kAnalogSteerScale * static_cast<int32_t>(occ->net_analog_z)) >> 1;
                const int32_t alt =
                        (kAnalogSteerScale * static_cast<int32_t>(occ->net_analog_y)) >> 1;
                if (std::abs(alt) > std::abs(steer_delta)) steer_delta = alt;
                m.steer_target_bam -= steer_delta; // [orig: `+528 -= v64`]
                m.cmd_speed = -(traits.player_speed * static_cast<int32_t>(occ->net_analog_x)) >> 7;
                // The original also turns the DRIVER entity's own yaw by the analog
                // delta when free-look is off (@0x48b7ce `v61->Yaw -= v64`); a remote
                // driver's yaw is wire-owned on our host, so that write is skipped
                // (D-NET-161 note).
            }

            // Modifier bits [orig: LABEL_123 @0x48b490-0x48b4d8].
            if ((move_order & Entity::kMoveOrderCrouch) != 0) m.cmd_speed >>= 1;
            if ((move_order & Entity::kMoveOrderProne) != 0) m.cmd_speed >>= 2;
            if ((move_order & 0x20u) != 0) veh.flags |= 0x80u;
            else veh.flags &= ~0x80u;
            if ((move_order & 0x40u) != 0) veh.flags |= 0x20u;
            else veh.flags &= ~0x20u;
            if ((move_order & 0x80u) != 0) veh.flags |= 0x8u; // [orig: SLOBYTE sign bit]
            else veh.flags &= ~0x8u;

            // Steer target: the driver's replicated heading (mouse steer), or the
            // vehicle's own heading under free-look [orig: @0x48b4a8-0x48b4c0].
            if (analog_sum == 0) {
                m.steer_target_bam = (move_order & Entity::kMoveOrderFreeLook) != 0 ? m.yaw_bam : driver_yaw_bam;
            }

            // Key-steer ramp + the 8-way direction cases [orig: @0x48b4e0-0x48b57a;
            // dir map F=0 FL=1 L=2 BL=3 B=4 BR=5 R=6 FR=7 (§5.38)].
            if (dir != 0) {
                if (m.steer_ramp_bam < kSteerRampCap) m.steer_ramp_bam += kSteerRampStep;
            } else {
                m.steer_ramp_bam = 0;
            }
            switch (dir) {
                case 1: m.steer_target_bam = m.yaw_bam + m.steer_ramp_bam; break;
                case 2:
                    m.steer_target_bam = m.yaw_bam + m.steer_ramp_bam;
                    m.cmd_speed = 0; // turn in place
                    break;
                case 3:
                    m.steer_target_bam = m.yaw_bam + m.steer_ramp_bam;
                    m.cmd_speed = (-m.cmd_speed) >> 1; // reverse at half target
                    break;
                case 4:
                    m.steer_target_bam = m.yaw_bam;
                    m.cmd_speed = (-m.cmd_speed) >> 1;
                    break;
                case 5:
                    m.steer_target_bam = m.yaw_bam - m.steer_ramp_bam;
                    m.cmd_speed = (-m.cmd_speed) >> 1;
                    break;
                case 6:
                    m.steer_target_bam = m.yaw_bam - m.steer_ramp_bam;
                    m.cmd_speed = 0;
                    break;
                case 7: m.steer_target_bam = m.yaw_bam - m.steer_ramp_bam; break;
                default: break;
            }
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
        int32_t min_rate = turn_rate >> 2;
        if (traits.turn_rate2 != 0) min_rate = traits.turn_rate2;
        int32_t f = 0x10000;
        if (traits.player_speed != 0) {
            f = 0x10000 - static_cast<int32_t>((static_cast<int64_t>(m.speed) << 16) /
                                               traits.player_speed);
            if (f < 0) f = 0;
        }
        const int32_t eff = static_cast<int32_t>(
                                    (static_cast<int64_t>(turn_rate - min_rate) * f + 0x8000) >> 16) +
                            min_rate;
        // Proportional step: 1/64 of the heading error, clamped to the effective rate
        // [orig: @0x48b9e9 `v106 = (target - Yaw + 32) >> 6` + the +-clamp].
        int32_t delta = (m.steer_target_bam - m.yaw_bam + 32) >> 6;
        if (delta > eff) delta = eff;
        if (delta < -eff) delta = -eff;
        // Smoothed wheel deflection [orig: @0x48ba17 `aiState += (4 - 32*delta -
        // aiState) >> 3` — entity->aiState is the wheel state on vehicles].
        m.steer_state += (4 - 32 * delta - m.steer_state) >> 3;
        // Yaw rate = -speed * (wheel >> 2) >> 16, applied while grounded
        // [orig: @0x48ba33 modelPtr0 write; the aim/AI lock bytes are unmodeled].
        m.wheel_rate_bam = static_cast<int32_t>(
                (static_cast<int64_t>(-m.speed) * (m.steer_state >> 2) + 0x8000) >> 16);
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
            !m.grounded && (veh.flags & kEntityFlagInAir) == 0) {
            if (m.speed < 0) {
                if (cmd < 0) cmd = -cmd;
            } else if (cmd > 0) {
                cmd = -cmd;
            }
        }
        // Slope factor: cos^2(pitch) in 22-bit fixed [orig: @0x48ba47 — off_849934
        // cos-table sample squared >> 22].
        const int32_t pitch_bam = static_cast<int32_t>(
                static_cast<int64_t>(veh.pitch) * 11930464); // deg -> BAM (spawn frame)
        const int32_t c = cos22_of_bam(pitch_bam);
        const int32_t c2 = static_cast<int32_t>((static_cast<int64_t>(c) * c) >> 22);
        target_speed = static_cast<int32_t>((static_cast<int64_t>(c2) * cmd) >> 22);

        // Chase 1/32 of the gap per tick, then family clamps [orig: @0x48baa2
        // `rawAccel = (target - speed + 16) >> 5` + the branch tree @0x48bac0-0x48bbe0].
        const int32_t raw_accel = (target_speed - m.speed + 16) >> 5;
        m.speed_accel = raw_accel;
        if (!m.grounded && traits.family != VehicleFamily::Bike) {
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
        // The cbik mover integrates speed only in CONTACT [orig: the
        // `!crashed && !(Flags & 0x2000) && BYTE2(aiRef0)` gate
        // @0x485501..0x485534]; the ground core integrates unconditionally
        // [orig: @0x48c302..0x48c32a].
        if (traits.family != VehicleFamily::Bike || m.grounded) {
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
            const int32_t c = cos22_of_bam(m.yaw_bam) >> 6; // 2^22 -> 16.16 unit
            const int32_t s = sin22_of_bam(m.yaw_bam) >> 6;
            m.vel_x = static_cast<int32_t>((static_cast<int64_t>(m.speed) * c + 0x8000) >> 16);
            m.vel_y = static_cast<int32_t>((static_cast<int64_t>(m.speed) * s + 0x8000) >> 16);
            m.slide_z = 0; // level dir frame — the slope vertical term rides the clamp
                           // below (pitch/roll contact solve deferred, D-NET-161)
        }
        if (traits.family == VehicleFamily::Bike) {
            // Bike-only vertical up-cap; the airborne input latch that can lift
            // it is input-side, so the client-run form caps unconditionally
            // [orig: vZ = min(vZ, 0x4000) @0x48659b..0x48659d].
            if (m.slide_z > 0x4000) m.slide_z = 0x4000;
            m.slide_z -= kGravityStepBike; // [orig: @0x4865a6 `slideDecay -= 250`]
        } else {
            m.slide_z -= kGravityStep; // [orig: @0x48d69b `slideDecay -= 324`]
        }
        // The in-water 25% drag + authority drown-drain block remains unmodeled
        // (the water plane is available, but not yet consumed by motor physics;
        // D-NET-161) [orig: @0x48d6a4-0x48d6f8].

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

        // Ground contact: the original resolves per-wheel contact + entity collision in
        // Entity_ProcessTrackedVehiclePhysics [orig: the physics tick @0x47c1c0; our
        // ground substitute is the shared 5-tap bilinear terrain column (the AI
        // grounding sampler) — a tracked divergence (D-NET-161)].
        if (world.terrain != nullptr) {
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
            m.yaw_bam += (veh.flags & kEntityFlagInAir) != 0
                    ? (m.wheel_rate_bam >> 2)
                    : m.wheel_rate_bam;
        } else if (m.grounded) {
            m.yaw_bam += m.wheel_rate_bam;
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
}

namespace {

// atan2 in this engine's BAM convention: radians * 2^31/pi, with the exact
// binary constant (the pair with the sin/cos scale below is deliberately NOT an
// exact inverse — port both verbatim) [orig: dbl_7C19D8 = 683565275.5764316].
inline int32_t bam_of_atan2(double y, double x) {
    if (y == 0.0 && x == 0.0) return 0; // fpatan(0,0) == 0
    return static_cast<int32_t>(std::atan2(y, x) * 683565275.5764316);
}

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
            m.net_smooth_heading =
                    (m.net_smooth_heading - m.yaw_bam + 10) / 20;
        } else {
            const int32_t n = watercraft_chase_bucket(dist);
            m.net_interp_steps = static_cast<int16_t>(n);
            m.net_smooth_target[0] =
                    (int32_t(dx) + (n >> 1)) / n;
            m.net_smooth_target[1] =
                    (int32_t(dy) + (n >> 1)) / n;
            m.net_smooth_target[2] =
                    (int32_t(dz) + (n >> 1)) / n;
            m.net_smooth_heading =
                    (m.net_smooth_heading - m.yaw_bam + 10) / 20;
        }
    }
    {
        const int16_t progress = m.net_interp_progress;
        if (progress < 20) m.yaw_bam += m.net_smooth_heading;
        if (progress < m.net_interp_steps) {
            px += m.net_smooth_target[0];
            py += m.net_smooth_target[1];
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
// (boat_platform_solve_spec + platform_solve_blockers, 2026-07-31): the
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
namespace {

// One bilinear terrain probe force [orig: Entity_ComputeCollisionForces
// @0x462150, terrain loop @0x462246..0x4624CA]: 4 samples at ±r, gradient
// force, penetration, slope classing against soft/hard cos22 thresholds.
struct PlatProbeForce {
    int32_t fx = 0, fy = 0, fz = 0; // force[i]; fz = push-up (-pen)
};

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

// The shared 4-normal plane fit [orig: identical in both solvers —
// Entity_ComputeSuspensionOrientation @0x46CAB7.. / the wheeled twin
// @0x46BAF9..; verbatim ASYMMETRIC aggregation, blockers §1]: rows from the
// 4 lever-corner targets; Z = the plain corner average [orig: solvedPos.Z
// @0x46E099..0x46E0B3]. Outputs pitch/roll BAM via the standard atan2
// decomposition (the @0x613310 interior = pending witness).
struct PlatFit {
    int32_t pitch_bam = 0;
    int32_t roll_bam = 0;
    int32_t z_avg = 0;
    double fwd_z = 0.0; // unit forward vertical component (beach term feed)
};

void plat_fit_corners(const double c[4][3], PlatFit &out) {
    auto sub = [](const double a[3], const double b[3], double r[3]) {
        r[0] = a[0] - b[0]; r[1] = a[1] - b[1]; r[2] = a[2] - b[2];
    };
    auto normalize = [](double v[3]) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n <= 0.0) { v[0] = v[1] = v[2] = 0.0; return; }
        v[0] /= n; v[1] /= n; v[2] /= n;
    };
    auto cross = [](const double a[3], const double b[3], double r[3]) {
        r[0] = a[1] * b[2] - a[2] * b[1];
        r[1] = a[2] * b[0] - a[0] * b[2];
        r[2] = a[0] * b[1] - a[1] * b[0];
    };
    // Edge sets, verbatim pairing (set 1 == set 2, real shipped duplication):
    double a[3], b[3], e3[3], f4[3], n1[3], n3[3], n4[3];
    sub(c[0], c[3], a); normalize(a);   // c0 - c3
    sub(c[3], c[2], b); normalize(b);   // c3 - c2
    sub(c[1], c[2], e3); normalize(e3); // c1 - c2
    sub(c[0], c[1], f4); normalize(f4); // c0 - c1
    cross(a, b, n1); normalize(n1);
    cross(e3, b, n3); normalize(n3);
    cross(e3, f4, n4); normalize(n4);
    double up[3], fwd[3], side[3];
    for (int i = 0; i < 3; ++i) {
        up[i] = 2.0 * n1[i] + n3[i] + n4[i]; // n1 counted twice (set 1+2)
        fwd[i] = 2.0 * e3[i] + 2.0 * a[i];   // e4+e3+a+e2
        side[i] = f4[i] + 3.0 * b[i];        // f4+f3+b+f2
    }
    normalize(up); normalize(fwd); normalize(side);
    out.fwd_z = fwd[2];
    const double fxy = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
    out.pitch_bam = bam_of_atan2(fwd[2], fxy);
    out.roll_bam = bam_of_atan2(side[2], up[2] <= 0.0 ? 1e-9 : up[2]);
    out.z_avg = int32_t((c[0][2] + c[1][2] + c[2][2] + c[3][2]) * 0.25);
}

} // namespace

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

    // The watercraft mover's own vertical integration runs BEFORE the solve
    // [orig: Position += slideDecay + the gravity leg `slideDecay -= 324`
    // @0x48d69b, ahead of the call @0x48ECE7]: an airborne hull falls; the
    // solve below re-owns Z once water or ground catches it.
    if ((veh.flags & kEntityFlagInAir) != 0) {
        m.slide_z -= kGravityStep;
        pz += m.slide_z;
    }

    // ---- §3 probe geometry. Probe springs +0x2D4.. are provably zero for
    // pure boats (blockers §4). q = beam/4.
    const int32_t q = (traits.box_y_hi - traits.box_y_lo) >> 2;
    if (q <= 0) return;
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
        {traits.box_x_lo + 3 * (lx >> 2), ymid, zt},      // p5
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
    const double cy = std::cos(double(m.yaw_bam) * kBamToRadX87);
    const double sy = std::sin(double(m.yaw_bam) * kBamToRadX87);
    const double cp = std::cos(double(m.air_pitch_bam) * kBamToRadX87);
    const double sp = std::sin(double(m.air_pitch_bam) * kBamToRadX87);
    const double cr = std::cos(double(m.air_roll_bam) * kBamToRadX87);
    const double sr = std::sin(double(m.air_roll_bam) * kBamToRadX87);
    // Row frame consistent with the fit's extraction above (fwd/side/up).
    const double fwdv[3] = {cy * cp, sy * cp, sp};
    const double sidev[3] = {cy * sr * sp - sy * cr, sy * sr * sp + cy * cr, -sr * cp};
    // up = fwd x side (right-handed with the row order used by the fit)
    const double upv[3] = {fwdv[1] * sidev[2] - fwdv[2] * sidev[1],
                           fwdv[2] * sidev[0] - fwdv[0] * sidev[2],
                           fwdv[0] * sidev[1] - fwdv[1] * sidev[0]};
    int32_t probes[7][3];
    for (int i = 0; i < 7; ++i) {
        const double mx = double(probes_model[i][0]);
        const double my = double(probes_model[i][1]);
        const double mz = double(probes_model[i][2]);
        probes[i][0] = px + int32_t(mx * fwdv[0] + my * sidev[0] + mz * upv[0]);
        probes[i][1] = py + int32_t(mx * fwdv[1] + my * sidev[1] + mz * upv[1]);
        probes[i][2] = pz + int32_t(mx * fwdv[2] + my * sidev[2] + mz * upv[2]);
    }

    // ---- §4/§6 first force pass + severity response.
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
    int32_t sev = 0;
    for (int i = 0; i < 7; ++i)
        sev = std::max(sev, plat_terrain_probe(world, probes[i][0], probes[i][1],
                                               probes[i][2], radii[i], soft, hard,
                                               forces[i]));
    if (sev == 1) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x4821E7]
    } else if (sev == 2) {
        m.speed -= m.speed >> ((traits.torque + 1) & 31); // [orig: @0x4822A4]
    } else if (sev == 3) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x4822C9]
        // Authority damage/kill + collision sound + momentum exchange + the
        // dead yaw-kick = cited deferrals (spec §6; the yaw-kick is witnessed
        // DEAD code). No hit-entity here -> the 0.25 speed cut applies:
        m.speed = int32_t(m.speed * 0.25); // [orig: flt_7C333C @0x4826EB]
    }

    // ---- §7 position push + second pass (severity >= 1 only).
    int32_t zf[7];
    for (int i = 0; i < 7; ++i) zf[i] = forces[i].fz;
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
        liftHi = traits.pitch_lift_vel * 100;
        liftLo = 250;
        planeSpd = 10000;
        pitchThr = int32_t(double(traits.pitch_lift) * 0.1 * 4096.0);
    } else { // HEAVY [orig: @0x48221B]
        floatH = q;
        liftHi = int32_t(double(traits.pitch_lift_vel) * F * 0.004);
        liftLo = liftHi >> 1;
        planeSpd = 20000;
        const double arz = std::abs(sidev[2]) * 65536.0;
        if (arz > double(0x2000)) { // heavily rolled [orig: @0x482259]
            pitchThr = int32_t(double(traits.pitch_lift) * 0.1 * 409.6);
            liftHi = 250;
        } else {
            pitchThr = int32_t(double(traits.pitch_lift) * 0.1 * 4096.0);
        }
    }
    const int32_t dipExit = int32_t(double(pitchThr) * (1.0 - 0.1 * double(traits.bob)));

    // ---- §8 water leg: per-corner submersion, draft, the afloat flag.
    int32_t sub_k[4], cz_k[4];
    for (int k = 0; k < 4; ++k) {
        cz_k[k] = probes[k][2];
        sub_k[k] = W + q - cz_k[k];
    }
    const int32_t avg = (cz_k[0] + cz_k[1] + cz_k[2] + cz_k[3]) >> 2;
    int32_t draft = avg;
    if (m.plat_afloat) draft = int32_t(double(avg) - 0.9 * F); // boat form [orig: flt_7C459C]
    if (W == 0 || draft + v210 >= W) {
        m.plat_afloat = false; // [orig: @0x482DB7; emitter release deferred]
    } else {
        // Splash FX on entry = cited deferral [orig: @0x482BB9..0x482C9D].
        m.plat_afloat = true; // [orig: @0x482CA5]
    }
    veh.flags = m.plat_afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);

    // ---- §9 lever corners around the CURRENT pose + machines.
    double c[4][3];
    const double hbd = double(hb), hld = double(hl);
    const double half[4][2] = {{-0.5, +0.5}, {+0.5, +0.5}, {-0.5, -0.5}, {+0.5, -0.5}};
    for (int k = 0; k < 4; ++k) {
        const double sb = half[k][0] * hbd, sf = half[k][1] * hld;
        c[k][0] = double(px) + sb * sidev[0] + sf * fwdv[0];
        c[k][1] = double(py) + sb * sidev[1] + sf * fwdv[1];
        c[k][2] = double(pz) + sb * sidev[2] + sf * fwdv[2];
    }
    // Capsize latch (client form; the authority Flags 0x10 upkeep = deferral).
    if ((veh.flags & kEntityFlagInAir) == 0 && upv[2] < 0.0 && !m.plat_capsized)
        m.plat_capsized = true; // [orig: @0x483474..0x48348B]
    // Accumulator ramp [orig: @0x483680..0x4836E6].
    for (int k = 0; k < 4; ++k)
        if (zf[k] <= 0 && sub_k[k] <= floatH) m.plat_acc[k] += 250;
    // Bow lift / planing / porpoise [orig: @0x4836EC..0x483891]. The command
    // register = the mirrored net_recv_speed (brain+0x220 on a remote boat).
    const int32_t cmd = m.net_recv_speed;
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
                    c[0][2] += double(lift);
                    c[1][2] += double(lift); // bow rises
                }
            } else {
                if (fwd_z_now < double(dipExit)) m.plat_porpoise = false;
                else { c[0][2] -= double(liftHi); c[1][2] -= double(liftHi); }
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
        for (int k = 0; k < 4; ++k) c[k][2] -= 500.0;
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
                lift = (zf[k] > sub_k[k]) ? zf[k] : std::abs(sub_k[k] - floatH);
                m.plat_acc[k] = 0;
            } else if (zf[k] != 0) {
                lift = zf[k];
                m.plat_acc[k] = 0;
            } else {
                lift = 250 - m.plat_acc[k];
            }
            c[k][2] += double(lift);
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
            for (int k = 0; k < 4; ++k) c[k][2] += double(inc);
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
                c[k][2] += double(lift);
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
                    c[k][2] += double(std::min(0, 250 - m.plat_acc[k]));
            } else {
                veh.flags |= kEntityFlagInAir; // [orig: @0x483B98]
            }
            // The wheeled solver's settled/airborne fit: same 4-normal fit;
            // Z = the average of the ABOVE-ground corners, rise-clamped
            // +0x2000/tick (blockers §3).
            plat_fit_corners(c, fit);
            if (m.plat_afloat) {
                int32_t new_z = fit.z_avg;
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

// [orig: Entity_UpdateWatercraftPhysics @0x48D480 — the client-executed subset for a
// remote boat; disasm-verified spec 2026-07-31 (net-re §5.38e). Block cites inline.
// Residual (both this and the air mover): the client-run deck-carrier follow
// (groundEntity tick-delta + parent-rotation re-seat @0x48D6DA..0x48DACD /
// @0x4905BC..0x49095B) is unported — the embedding sim freezes carried rows to
// the row-level seat-follow instead (D-NET-196 residuals).]
void watercraft_client_tick(World &world, Entity &veh, const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    if (!m.net_predicted) return;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(veh.yaw);
        m.yaw_seeded = true;
    }

    // ---- 1. Per-record chase (the §5.38e vehicle template) on the world pose.
    vehicle_client_chase(veh);
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);

    // ---- 2. Register mirror: the non-driver machine adopts the received
    // speed/steer as its own drive command, every tick
    // [orig: brain[136]=[177], brain[132]=[179] @0x48DDD4..0x48DDF4].
    // Deferred witnessed gate: retail mirrors only when occupantEntity !=
    // g_local_player_entity — the local driver's machine runs the input leg
    // instead ([136] = ([136]+[177])>>1 averaging) [orig: @0x48DDD4/@0x490C9E].
    // Joiner-side vehicle drive input is not wired yet, so every predicted row
    // is remote-occupied and the gate is vacuously satisfied; add it with the
    // occupant mirror when the input leg lands (D-NET-196 residuals).
    m.cmd_speed = m.net_recv_speed;
    m.steer_target_bam = m.net_recv_steer_bam;

    // ---- 3. Steer/rudder integrator [orig: @0x48E82C..0x48E926].
    {
        const int32_t turn_rate = traits.turn_rate;
        // Retail reads ONLY itemDef waterSpeed here; a zero pins the speed
        // fraction at 0 so the effective turn rate stays at min_rate — no
        // player_speed fallback exists [orig: the jz to the f=0 arm @0x48E844].
        const int32_t water_spd = traits.water_speed;
        int32_t min_rate = turn_rate >> 2;
        if (traits.turn_rate2 != 0) min_rate = traits.turn_rate2;
        int32_t f = 0;
        if (water_spd != 0) {
            f = 0x10000 - static_cast<int32_t>(
                    (static_cast<int64_t>(m.speed) << 16) / water_spd);
        }
        if (f < 0) f = 0;
        // No upper clamp — a reversing hull over-rotates, witnessed absent.
        const int32_t eff = min_rate + static_cast<int32_t>(
                (static_cast<int64_t>(turn_rate - min_rate) * f + 0x8000) >> 16);
        int32_t delta = (m.steer_target_bam - m.yaw_bam + 32) >> 6;
        if (delta > eff) delta = eff;
        if (delta < -eff) delta = -eff;
        m.steer_state += (4 - 32 * delta - m.steer_state) >> 3;
        // Yaw-rate recompute is airborne-gated in retail; without the platform
        // solve the airborne flag never sets here, so the gate is always open —
        // the afloat case retail always recomputes too [orig: @0x48E8E3..0x48E920].
        m.wheel_rate_bam = static_cast<int32_t>(
                (static_cast<int64_t>(-m.speed) * (m.steer_state >> 2) + 0x8000) >> 16);
    }

    // ---- 4. Thrust [orig: @0x48E926..0x48EA12].
    int32_t vertical_thrust = 0;
    {
        const int32_t cmd = m.cmd_speed;
        if (cmd == 0) {
            if (std::abs(m.vel_x) < 384) m.vel_x = 0;
            if (std::abs(m.vel_y) < 384) m.vel_y = 0;
        } else {
            const int32_t a = std::abs(cmd);
            int32_t acc = traits.acceleration + (a >> 8) + (a >> 7);
            const int32_t accel = cmd >= 0 ? std::min(acc, cmd)
                                           : std::max(-acc, cmd);
            // Forward from the live heading. Retail builds the full euler
            // matrix from the entity pose (incl. platform pitch/roll); the
            // platform solve is unported so the hull is level here — the
            // capsize up[2]>0 gate is trivially open [orig: @0x48E972..0x48E9FF].
            const int32_t c = cos22_of_bam_x87(m.yaw_bam) >> 6; // 2^22 -> 16.16
            const int32_t s = sin22_of_bam_x87(m.yaw_bam) >> 6;
            m.vel_x += static_cast<int32_t>(
                    (static_cast<int64_t>(accel) * c + 0x8000) >> 16);
            m.vel_y += static_cast<int32_t>(
                    (static_cast<int64_t>(accel) * s + 0x8000) >> 16);
            // Retail's beach gate term is (accel * fwd[2] + 0x8000) >> 16 with
            // fwd from the full euler matrix incl. the platform solve's
            // pitch/roll [orig: @0x48E9DC..0x48E9FF]. Under the level-hull
            // stand-in fwd[2] = 0, so the faithful reduction is 0 — the 30583
            // beach full-stop never fires until the platform solve lands
            // (tracked with the @0x481870 stand-in, D-NET-196 residuals).
        }
    }

    // ---- 5. Drag / slip / keel [orig: @0x48EA14..0x48EBB3, FPU-reconstructed].
    {
        m.vel_x -= m.vel_x >> 6;
        m.vel_y -= m.vel_y >> 6;
        const int32_t vx = m.vel_x, vy = m.vel_y;
        const int32_t vel_heading = bam_of_atan2(double(vy), double(vx));
        const int32_t slip = m.yaw_bam - vel_heading;
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
        const int32_t beam = m.yaw_bam + 0x3FFFFFC0;
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

    // ---- 6. Contact drags [orig: @0x48EBB5..0x48ECA6]. The afloat/land branch
    // keys on the platform solve's afloat flag; unported, so derive it from the
    // water plane: in water = terrain under the hull below the water line.
    {
        bool afloat = false;
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
        if (!afloat) {
            // Landed hull: planar and yaw-rate sheds (the slideDecay gravity
            // leg stays with the unported platform solve; Z is chase-owned).
            m.vel_x -= (m.vel_x + 4) >> 3;
            m.vel_y -= (m.vel_y + 4) >> 3;
            m.wheel_rate_bam -= (m.wheel_rate_bam + 2) >> 2;
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
                m.wheel_rate_bam -= (m.wheel_rate_bam + 2) >> 2;
                if (vertical_thrust > 0 && ground - world.env.water_z > 30583 &&
                        !veh.ground_target.valid()) {
                    m.vel_x = 0;
                    m.vel_y = 0;
                    m.wheel_rate_bam = 0;
                }
            }
        }
    }

    // ---- 7. Integration + yaw application [orig: @0x48ECA8..0x48ECF2]. Z is
    // chase-only (platform-solve residual, see the header note).
    px += m.vel_x;
    py += m.vel_y;
    {
        const int32_t r = m.wheel_rate_bam;
        m.wheel_rate_bam = r - ((r + 16) >> 5) - (r >> 31); // decay toward zero
    }
    m.yaw_bam += m.wheel_rate_bam;

    veh.position.x = static_cast<float>(from_fixed(px));
    veh.position.y = static_cast<float>(from_fixed(py));
    veh.position.z = static_cast<float>(from_fixed(pz));

    // The platform solve runs every tick after integration, exactly where the
    // retail caller sits [orig: call @0x48ECE7 — after Position += velocity,
    // before the yaw apply]. It owns Z + Pitch/Roll + the afloat/airborne
    // flags from here (the chase-staged Z above is its seed, matching the
    // template's airborne-only Z-step gate).
    watercraft_platform_solve(world, veh, traits);
    veh.yaw = static_cast<int16_t>(std::lround(
            mission_yaw_deg_from_bam_heading(m.yaw_bam)));
}

// The GROUND-family prediction leg (net-re §5.38e B-facet): the client subset of
// Entity_UpdateVehiclePhysics is structurally the authority drive core minus the
// input block [spec part F, decompile-level] — run the shared chase, adopt the
// mirrored registers [orig: [136]=[177]/[132]=[179] @ the family mirror], and
// drive tick_vehicle_motor's core with the input block bypassed
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
    // Register mirror — the occupant != local-player gate is deferred with the
    // input leg (see the watercraft mirror note) [orig: @0x48DDD4 pattern].
    m.cmd_speed = m.net_recv_speed;
    m.steer_target_bam = m.net_recv_steer_bam;
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
void aircraft_client_tick(World &world, Entity &veh, const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    if (!m.net_predicted) return;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(veh.yaw);
        m.yaw_seeded = true;
    }
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);

    // Ground cache — the every-8th-tick sample [orig: entity+0x2A4 @0x4903A8..];
    // staggering is a load-spreading detail, refreshed here per tick when cheap.
    if (world.terrain != nullptr) {
        const int32_t pos3[3] = {px, py, pz};
        const GroundClearance clearance{};
        const int32_t g =
                calc_average_ground_height(*world.terrain, pos3, 0, clearance);
        if (g != INT32_MIN) m.ground_cache = g;
    }
    const int32_t ground = m.ground_cache;

    // ---- 1. The air interp block [orig: @0x49095E..0x490C98]. 3D distance,
    // snap 0xA0000 (0x20000 when BOTH received cmds < 293), buckets
    // {8,10,15,20,25,32}, yaw (d+10)/20 over 20 ticks, Z stepped like X/Y.
    if (m.net_interp_progress == 0) {
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
            m.net_smooth_heading =
                    (m.net_smooth_heading - m.yaw_bam + 10) / 20;
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
            m.net_smooth_heading =
                    (m.net_smooth_heading - m.yaw_bam + 10) / 20;
        }
    }
    {
        const int16_t progress = m.net_interp_progress;
        if (progress < 20) m.yaw_bam += m.net_smooth_heading;
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

    // ---- 2. Register mirror [orig: @0x490C9E..0x490CCA] — the occupant !=
    // local-player gate is deferred with the input leg (see the watercraft
    // mirror note).
    m.cmd_speed = m.net_recv_speed;
    int32_t cmd_lat = m.net_recv_lat;
    m.steer_target_bam = m.net_recv_steer_bam;

    // ---- 2a. Client engine-off override [orig: LABEL_305 @0x491C95..0x491CC2].
    if (!m.net_engine_on) {
        if (ground != INT32_MIN) m.net_alt_target = ground - 0x2000;
        m.cmd_speed = 0;
        cmd_lat = 0;
        m.steer_target_bam = m.yaw_bam;
    }

    // ---- 3. Yaw servo (second order) [orig: @0x491CC8..0x491D27].
    {
        int32_t step = (m.steer_target_bam - m.yaw_bam + 8) >> 4;
        const int32_t tr = traits.turn_rate;
        if (step > tr) step = tr;
        if (step < -tr) step = -tr;
        m.wheel_rate_bam += (step + 4) >> 3;
        const int32_t astep = step < 0 ? -step : step;
        if (m.wheel_rate_bam > astep) m.wheel_rate_bam = astep;
        if (m.wheel_rate_bam < -astep) m.wheel_rate_bam = -astep;
    }

    // ---- 4. Tilt commands + climb servo [orig: @0x491D2D..0x491E2F].
    {
        const int32_t cap = traits.acceleration << 12;
        int32_t fwd = m.cmd_speed << 11;
        int32_t lat = cmd_lat << 11;
        if (fwd > cap) fwd = cap;
        if (fwd < -cap) fwd = -cap;
        if (lat > cap) lat = cap;
        if (lat < -cap) lat = -cap;
        m.air_pitch_rate -= fwd;
        m.air_roll_rate -= lat;
        if (to_fixed(veh.bound_radius) >= 0xF0000)
            m.slide_z += (m.net_alt_target - pz + 0x100) >> 9; // heavy 1/512
        else
            m.slide_z += (m.net_alt_target - pz + 0x80) >> 8;  // light 1/256
    }

    // Airborne/grounded pick: the 0x47EF10 solve's Flags 0x2000 is unported —
    // derive from the ground cache (well clear of the ground = airborne).
    const bool airborne =
            ground == INT32_MIN || pz - ground > 0x10000;
    const bool in_water =
            world.env.water_z != 0 && pz <= world.env.water_z;

    if (airborne) {
        // ---- 5. Airborne aero block [orig: @0x491E35..0x4922BC].
        const int32_t vx = m.vel_x, vy = m.vel_y;
        const int32_t vel_heading = bam_of_atan2(double(vy), double(vx));
        const int32_t slip = m.yaw_bam - vel_heading;
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
        m.speed = along;
        const int32_t alat = lateral < 0 ? -lateral : lateral;
        const int32_t aclat = cmd_lat < 0 ? -cmd_lat : cmd_lat;
        if (alat > aclat)
            m.air_roll_rate += 8 * cmd_lat - 8 * lateral;
        const int32_t aalong = along < 0 ? -along : along;
        const int32_t acmd = m.cmd_speed < 0 ? -m.cmd_speed : m.cmd_speed;
        if (aalong > acmd) {
            const int32_t e = along - m.cmd_speed;
            const int32_t p32 = m.air_pitch_bam + 32 * e;
            const int32_t ap32 = p32 < 0 ? -p32 : p32;
            const int32_t ap = m.air_pitch_bam < 0 ? -m.air_pitch_bam
                                                   : m.air_pitch_bam;
            if (ap32 < ap) {
                m.air_pitch_rate += 16 * e;
                m.air_pitch_bam += 32 * e;
            } else {
                m.air_pitch_rate += 4 * e;
                m.air_pitch_bam += 4 * e;
            }
        }
        // Weathervane [orig: @0x491FA8..0x491FD9] (the occupant-analog steer
        // feedback is input-leg, absent here). The multiplier is
        // abs(lateral >> 6) — retail shifts FIRST, then takes the absolute
        // value, one larger than (abs >> 6) for negative non-multiples of 64.
        {
            const int32_t wv = lateral >> 6;
            m.yaw_bam += static_cast<int32_t>(
                    (static_cast<int64_t>(wv < 0 ? -wv : wv) *
                             (vel_heading - m.yaw_bam) + 0x8000) >> 16);
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
        m.air_pitch_rate -= (m.air_pitch_bam + 0x100) >> 9;
        m.air_pitch_bam -= (m.air_pitch_bam + 0x100) >> 9;
        m.air_roll_rate -= (m.air_roll_bam + 0x100) >> 9;
        m.air_roll_bam -= (m.air_roll_bam + 0x100) >> 9;
        m.air_roll_rate -= ((m.air_roll_rate + 8) >> 4) + (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 8) >> 4) + (m.air_pitch_rate >> 31);
        if (traits.speed_pitch != 0) {
            const int32_t pc = traits.speed_pitch * 192426;
            if (m.air_pitch_rate > pc) m.air_pitch_rate = pc;
            if (m.air_pitch_rate < -pc) m.air_pitch_rate = -pc;
        }
        if (traits.turn_roll != 0) {
            const int32_t rc = traits.turn_roll * 192426;
            if (m.air_roll_rate > rc) m.air_roll_rate = rc;
            if (m.air_roll_rate < -rc) m.air_roll_rate = -rc;
        }
    } else {
        // ---- 6. Grounded shed block [orig: @0x4922C1..0x492378].
        m.vel_x -= ((m.vel_x + 2) >> 2) + (m.vel_x >> 31);
        m.vel_y -= ((m.vel_y + 2) >> 2) + (m.vel_y >> 31);
        m.slide_z -= ((m.slide_z + 8) >> 4) + (m.slide_z >> 31);
        if (m.slide_z < 0) m.slide_z >>= 2;
        m.wheel_rate_bam -= ((m.wheel_rate_bam + 4) >> 3) + (m.wheel_rate_bam >> 31);
        m.air_roll_rate -= ((m.air_roll_rate + 4) >> 3) + (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 4) >> 3) + (m.air_pitch_rate >> 31);
        m.net_alt_target += (pz - m.net_alt_target) >> 2;
    }

    // ---- 7. Common tail [orig: @0x49237E..0x492776].
    if (m.cmd_speed == 0 && cmd_lat == 0) {
        if (std::abs(m.vel_x) < 384) m.vel_x = 0;
        if (std::abs(m.vel_y) < 384) m.vel_y = 0;
    }
    // Unconditional [orig: @0x4923C0..0x4923E9] — a zero climb_speed def pins
    // the vertical rate to exactly 0 (both bounds collapse), the witnessed
    // degenerate behavior; never guard it away.
    if (m.slide_z > traits.climb_speed) m.slide_z = traits.climb_speed;
    if (m.slide_z < -2 * traits.climb_speed)
        m.slide_z = -2 * traits.climb_speed;
    if (!m.net_engine_on) {
        m.air_roll_rate -= ((m.air_roll_rate + 8) >> 4) + (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 8) >> 4) + (m.air_pitch_rate >> 31);
    }
    if (in_water) {
        m.vel_x -= ((m.vel_x + 4) >> 3) + (m.vel_x >> 31);
        m.vel_y -= ((m.vel_y + 4) >> 3) + (m.vel_y >> 31);
        m.slide_z -= ((m.slide_z + 4) >> 3) + (m.slide_z >> 31);
        m.wheel_rate_bam -= ((m.wheel_rate_bam + 8) >> 4) + (m.wheel_rate_bam >> 31);
        m.air_roll_rate -= ((m.air_roll_rate + 8) >> 4) + (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 8) >> 4) + (m.air_pitch_rate >> 31);
    }
    px += m.vel_x;
    py += m.vel_y;
    pz += m.slide_z;
    {
        const int32_t r = m.wheel_rate_bam;
        m.wheel_rate_bam = r - ((r + 16) >> 5) - (r >> 31);
    }
    // Contact stub (the unported 0x47EF10 solve): never sink below the ground.
    if (ground != INT32_MIN && pz < ground) {
        pz = ground;
        if (m.slide_z < 0) m.slide_z = 0;
    }
    constexpr int32_t kAttitudeRateClamp = 178956960; // 0xAAAAAA0
    if (m.wheel_rate_bam > kAttitudeRateClamp) m.wheel_rate_bam = kAttitudeRateClamp;
    if (m.wheel_rate_bam < -kAttitudeRateClamp) m.wheel_rate_bam = -kAttitudeRateClamp;
    if (m.air_pitch_rate > kAttitudeRateClamp) m.air_pitch_rate = kAttitudeRateClamp;
    if (m.air_pitch_rate < -kAttitudeRateClamp) m.air_pitch_rate = -kAttitudeRateClamp;
    if (m.air_roll_rate > kAttitudeRateClamp) m.air_roll_rate = kAttitudeRateClamp;
    if (m.air_roll_rate < -kAttitudeRateClamp) m.air_roll_rate = -kAttitudeRateClamp;
    m.air_pitch_bam += m.air_pitch_rate;
    m.air_roll_bam += m.air_roll_rate;
    m.yaw_bam += m.wheel_rate_bam;

    veh.position.x = static_cast<float>(from_fixed(px));
    veh.position.y = static_cast<float>(from_fixed(py));
    veh.position.z = static_cast<float>(from_fixed(pz));
    veh.yaw = static_cast<int16_t>(std::lround(
            mission_yaw_deg_from_bam_heading(m.yaw_bam)));
}

} // namespace opennova::world
