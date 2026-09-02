#include <runtime/world/vehicle_motor.h>

// Split out of vehicle_motor.cpp (the oversize-TU ratchet). Motion only — every
// body is unchanged, and each original-code citation moved with the code it
// annotates.
//
// The AIR family (CHel + cpln — one mover, the plane callback is a thunk): the
// occupant input block, the client prediction leg and the authority legs of
// Entity_UpdateAircraftPhysics [orig: @0x490310], consumed by the AiSystem
// vehicle pass (host) and the joiner's client pass.

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/io/bam.h>

#include "vehicle_motor_detail.h"

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

namespace opennova::world {

using namespace detail; // the shared solve/trig sub-contract, unqualified as before

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
            climb = 0;
        }
        m.net_climb = climb; // the authority's [548] (engine flag source)
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

    // ---- 0. The authority health machine at the mover head [orig:
    // @0x4903F0..0x490480, on the (tick + 9*DcbId) & 0x3F cadence]: above
    // criticalHp the hull regens nonCriticalRegen up to healthMax - regen
    // (@0x4903f9..0x49042d); at or below it the hull BURNS — criticalDrain per
    // cadence (@0x490434..0x490480) and, airborne with the altitude target more
    // than 1 u above the ground, the yaw spins 2886390 BAM per tick — the
    // tail-rotor-loss spiral (@0x49048e..0x4904be; the pilot's own yaw follows
    // it unless free-looking, a look write the client owns — D-NET-161). The
    // smoke/fire emitters and the every-64th-tick fire sound are presentation
    // seams. A dead hull takes none of it.
    const bool motor_is_authority = world.ai != nullptr && world.ai->is_authority;
    if (motor_is_authority && veh.health > 0) {
        const bool cadence64 =
                ((world.logic_tick + 9u * static_cast<uint32_t>(veh.net_id)) & 0x3Fu) == 0;
        if (veh.health > traits.critical_hp) {
            if (cadence64 && traits.non_critical_regen != 0 &&
                veh.health < veh.health_max - traits.non_critical_regen)
                veh.health += traits.non_critical_regen;
        } else {
            if (cadence64) {
                veh.health -= traits.critical_drain;
                if (veh.health < 0) veh.health = 0;
            }
            if ((veh.flags & kEntityFlagInAir) != 0 && ground != INT32_MIN &&
                m.net_alt_target - ground > 0x10000)
                m.yaw_bam = io::bam_sub(m.yaw_bam, 2886390);
        }
    }

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
    if (motor_is_authority) {
        // The rotor gate [orig: `updated = Health > 0 && !(Flags & 1) ?
        //  Entity_UpdateHeloRotorSpin(...) : 0` @0x490592..0x4905a6, whose
        //  return is `!is_authority || speed >= 0x0CCCCCC0`; the LABEL_328 arm
        //  @0x491ca7..0x491cc2 parks every command until it holds]. A hull
        // commands nothing until its rotor reaches full speed — the spool-up a
        // cold helicopter sits through. Our part-anim machine runs at the mover
        // tail, so the gate reads the previous tick's speed.
        const bool rotor_up = veh.health > 0 && (veh.flags & 0x1u) == 0 &&
                              m.part_spin.speed >= kRotorSpeedMax;
        if (!rotor_up) {
            if (ground != INT32_MIN) m.net_alt_target = ground - 0x2000;
            m.net_climb = 0;
            m.cmd_speed = 0;
            m.cmd_lateral_speed = 0;
            m.steer_target_bam = m.yaw_bam;
        }
        // The engine flag IS the climb register's non-zero test; the register
        // is clamped at zero on every write, so a parked target under the
        // ground reads as engine off [orig: @0x491dfd..0x491e11].
        m.net_engine_on = m.net_climb != 0;
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
        // A submerged hull drowns on the authority: 100 health per tick to
        // zero [orig: @0x4924e2..0x492503].
        if (motor_is_authority && veh.health > 0) {
            veh.health -= 100;
            if (veh.health < 0) veh.health = 0;
        }
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
    // The crash drain: past 100 deg of roll OR pitch the hull loses 200 health
    // per tick on the authority, floored at zero [orig: @0x492637..0x49266f —
    // |+0x18| / |+0x14| > 0x471C7180; the kill edge's +0x178 zero is the
    // attacker slot, an unmodeled write].
    if (motor_is_authority && veh.health > 0 &&
        (io::bam_abs(m.air_roll_bam) > 0x471C7180 ||
         io::bam_abs(m.air_pitch_bam) > 0x471C7180)) {
        veh.health -= 200;
        if (veh.health < 0) veh.health = 0;
    }

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
