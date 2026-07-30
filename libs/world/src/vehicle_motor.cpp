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
// `slideDecay -= 324`].
constexpr int32_t kGravityStep = 324;

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
        int32_t cmd = m.cmd_speed;
        if (!m.grounded && (veh.flags & kEntityFlagInAir) == 0) {
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
        if (!m.grounded) {
            // Wheels off the ground: coast clamp at half deceleration
            // [orig: @0x48bacf `±deceleration >> 1`].
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
        m.speed += m.speed_accel; // [orig: @0x48bbe6 `currentSpeed += speedAccel`]
        if (target_speed == 0 && std::abs(m.speed) < 48) m.speed = 0; // [orig: @0x48bbf7]
        if (m.speed_accel == 0) m.speed = target_speed;               // [orig: @0x48bc0d]
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
        m.slide_z -= kGravityStep; // [orig: @0x48d69b `slideDecay -= 324`]
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
        // `Yaw += modelPtr0`, gated on ground contact].
        if (m.grounded) m.yaw_bam += m.wheel_rate_bam;

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

} // namespace opennova::world
