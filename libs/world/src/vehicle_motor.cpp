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

// [orig: Entity_UpdateWatercraftPhysics @0x48D480 — the client-executed subset for a
// remote boat; disasm-verified spec 2026-07-31 (net-re §5.38e). Block cites inline.]
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
    m.cmd_speed = m.net_recv_speed;
    m.steer_target_bam = m.net_recv_steer_bam;

    // ---- 3. Steer/rudder integrator [orig: @0x48E82C..0x48E926].
    {
        const int32_t turn_rate = traits.turn_rate;
        const int32_t water_spd =
                traits.water_speed != 0 ? traits.water_speed : traits.player_speed;
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
            const int32_t c = cos22_of_bam(m.yaw_bam) >> 6; // 2^22 -> 16.16
            const int32_t s = sin22_of_bam(m.yaw_bam) >> 6;
            m.vel_x += static_cast<int32_t>(
                    (static_cast<int64_t>(accel) * c + 0x8000) >> 16);
            m.vel_y += static_cast<int32_t>(
                    (static_cast<int64_t>(accel) * s + 0x8000) >> 16);
            vertical_thrust = accel; // level hull: the fwd[2] beach gate term
                                     // reduces to the thrust sign
        }
    }

    // ---- 5. Drag / slip / keel [orig: @0x48EA14..0x48EBB3, FPU-reconstructed].
    {
        m.vel_x -= m.vel_x >> 6;
        m.vel_y -= m.vel_y >> 6;
        const int32_t vx = m.vel_x, vy = m.vel_y;
        const int32_t vel_heading = bam_of_atan2(double(vy), double(vx));
        const int32_t slip = m.yaw_bam - vel_heading;
        const int32_t s22 = sin22_of_bam(slip);
        const int32_t c22 = cos22_of_bam(slip);
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
        const int32_t bs22 = sin22_of_bam(beam);
        const int32_t bc22 = cos22_of_bam(beam);
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
        // Shore look-ahead: sample terrain at the NEXT position.
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

    // ---- 2. Register mirror [orig: @0x490C9E..0x490CCA].
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
        const int32_t s22 = sin22_of_bam(slip);
        const int32_t c22 = cos22_of_bam(slip);
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
        // feedback is input-leg, absent here).
        m.yaw_bam += static_cast<int32_t>(
                (static_cast<int64_t>(alat >> 6) *
                         (vel_heading - m.yaw_bam) + 0x8000) >> 16);
        // Tilt -> acceleration in the yaw frame [orig: @0x492006..0x492152].
        const int32_t a_fwd = -static_cast<int32_t>(
                (1169LL * sin22_of_bam(m.air_pitch_bam)) >> 22);
        const int32_t roll_k = alat < aclat ? 501 : 334;
        const int32_t a_lat = static_cast<int32_t>(
                (static_cast<int64_t>(roll_k) *
                         sin22_of_bam(m.air_roll_bam)) >> 22);
        int32_t zp = static_cast<int32_t>(
                (static_cast<int64_t>(along) *
                         sin22_of_bam(m.air_pitch_bam)) >> 22);
        if (zp < 0) zp >>= 2;
        m.slide_z += zp >> 2;
        int32_t zr = static_cast<int32_t>(
                (static_cast<int64_t>(lateral) *
                         sin22_of_bam(m.air_roll_bam)) >> 22);
        if (zr < 0) zr >>= 2;
        m.slide_z -= zr >> 3;
        const int32_t sy = sin22_of_bam(m.yaw_bam);
        const int32_t cy = cos22_of_bam(m.yaw_bam);
        m.vel_x += static_cast<int32_t>((static_cast<int64_t>(a_fwd) * cy) >> 22) +
                   static_cast<int32_t>((static_cast<int64_t>(a_lat) * sy) >> 22);
        m.vel_y += static_cast<int32_t>((static_cast<int64_t>(a_fwd) * sy) >> 22) -
                   static_cast<int32_t>((static_cast<int64_t>(a_lat) * cy) >> 22);
        m.vel_x = static_cast<int32_t>((1019LL * m.vel_x + 512) >> 10);
        m.vel_y = static_cast<int32_t>((1019LL * m.vel_y + 512) >> 10);
        m.slide_z = static_cast<int32_t>((240LL * m.slide_z + 128) >> 8);
        // Attitude self-righting + rate damping [orig: @0x4921CD..0x492246].
        m.air_pitch_rate -= (m.air_pitch_bam + 0x100) >> 9;
        m.air_pitch_bam -= (m.air_pitch_bam + 0x100) >> 9;
        m.air_roll_rate -= (m.air_roll_bam + 0x100) >> 9;
        m.air_roll_bam -= (m.air_roll_bam + 0x100) >> 9;
        m.air_roll_rate -= ((m.air_roll_rate + 8) >> 4) - (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 8) >> 4) - (m.air_pitch_rate >> 31);
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
        m.vel_x -= ((m.vel_x + 2) >> 2) - (m.vel_x >> 31);
        m.vel_y -= ((m.vel_y + 2) >> 2) - (m.vel_y >> 31);
        m.slide_z -= ((m.slide_z + 8) >> 4) - (m.slide_z >> 31);
        if (m.slide_z < 0) m.slide_z >>= 2;
        m.wheel_rate_bam -= ((m.wheel_rate_bam + 4) >> 3) - (m.wheel_rate_bam >> 31);
        m.air_roll_rate -= ((m.air_roll_rate + 4) >> 3) - (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 4) >> 3) - (m.air_pitch_rate >> 31);
        m.net_alt_target += (pz - m.net_alt_target) >> 2;
    }

    // ---- 7. Common tail [orig: @0x49237E..0x492776].
    if (m.cmd_speed == 0 && cmd_lat == 0) {
        if (std::abs(m.vel_x) < 384) m.vel_x = 0;
        if (std::abs(m.vel_y) < 384) m.vel_y = 0;
    }
    if (traits.climb_speed != 0) {
        if (m.slide_z > traits.climb_speed) m.slide_z = traits.climb_speed;
        if (m.slide_z < -2 * traits.climb_speed)
            m.slide_z = -2 * traits.climb_speed;
    }
    if (!m.net_engine_on) {
        m.air_roll_rate -= ((m.air_roll_rate + 8) >> 4) - (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 8) >> 4) - (m.air_pitch_rate >> 31);
    }
    if (in_water) {
        m.vel_x -= ((m.vel_x + 4) >> 3) - (m.vel_x >> 31);
        m.vel_y -= ((m.vel_y + 4) >> 3) - (m.vel_y >> 31);
        m.slide_z -= ((m.slide_z + 4) >> 3) - (m.slide_z >> 31);
        m.wheel_rate_bam -= ((m.wheel_rate_bam + 8) >> 4) - (m.wheel_rate_bam >> 31);
        m.air_roll_rate -= ((m.air_roll_rate + 8) >> 4) - (m.air_roll_rate >> 31);
        m.air_pitch_rate -=
                ((m.air_pitch_rate + 8) >> 4) - (m.air_pitch_rate >> 31);
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
