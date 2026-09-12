#include <runtime/world/vehicle_system.h>
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

// Terrain-relative AI altitude caps the requested rise, not the absolute height.
// [orig: Entity_UpdateAircraftPhysics @0x4912A0]
namespace opennova::world {

using namespace detail; // the shared solve/trig sub-contract, unqualified as before

// CHel/cpln share the occupant input block, attitude and contact servos.
// Brain [548] carries climb above ground; [524] holds absolute altitude.
// [orig: Entity_UpdateAircraftPhysics @0x490310]
static void stage_air_vehicle_input(World &world, Entity &veh, Entity &occ,
		const VehicleTraits &traits, int32_t ground, int32_t pz) {
	Entity::VehicleMotorState &m = veh.veh;
	const uint32_t move_order = static_cast<uint32_t>(occ.net_move_input) |
			(static_cast<uint32_t>(occ.net_stance_bits) << 8) | occ.local_view_input;
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
		if ((move_order & Entity::kMoveOrderFreeLook) == 0)
			turn_pilot_view(world, occ, -((kAnalogSteerScale * int32_t(occ.net_analog_z)) >> 1));
	}
	if ((move_order & Entity::kMoveOrderCrouch) != 0) {
		m.cmd_speed >>= 1;
        m.cmd_lateral_speed >>= 1;
	}
	if ((move_order & Entity::kMoveOrderProne) != 0) {
        m.cmd_speed >>= 2;
        m.cmd_lateral_speed >>= 2;
    }
	// Below 2*radius the collective changes climb; above it, altitude.
	// The inclusive reconciliation boundary follows the strict input boundary.
	// [orig: Entity_UpdateAircraftPhysics @0x490310, collective/flare/steer block]
	if (ground != INT32_MIN) {
		const bool near = io::bam_sub(pz, ground) < bam_mul_wrap(to_fixed(veh.bound_radius), 2);
		const auto collective = [&](int32_t delta) {
			if (near)
				m.net_climb = io::bam_add(m.net_climb, delta);
			else {
				m.net_alt_target = io::bam_add(m.net_alt_target, delta);
				m.net_climb = io::bam_sub(m.net_alt_target, ground);
			}
		};
		if ((move_order & 0xC0u) != 0) {
			if ((move_order & 0x40u) != 0)
				collective(-0x4000);
			if ((move_order & 0x80u) != 0)
				collective(0x4000);
		} else
			collective(-int32_t(occ.analog_throttle) * 128);
		if (io::bam_add(ground, m.net_climb) > 0x1000000) {
			m.net_climb = io::bam_sub(0x1000000, ground);
			m.net_alt_target = 0x1000000;
		}
		if (m.net_climb < 0) {
			m.net_climb = 0;
			m.net_alt_target = ground;
		}
		world.vehicles.tick_flare_input(veh);
		if (io::bam_sub(pz, ground) <= bam_mul_wrap(to_fixed(veh.bound_radius), 2))
			m.net_alt_target = io::bam_add(ground, m.net_climb);
	}
	if (m.net_climb != 0) {
		if (analog_sum == 0) {
			const AiEntity *pilot = world.ai.for_handle(occ.handle);
			m.steer_target_bam = (move_order & Entity::kMoveOrderFreeLook) != 0
					? m.yaw_bam
					: (pilot != nullptr ? pilot->heading
										: bam_heading_from_mission_yaw_deg(occ.yaw));
		}
	} else if (ground != INT32_MIN) {
		m.net_alt_target = io::bam_sub(ground, 0x4000);
		m.cmd_speed = 0;
		m.cmd_lateral_speed = 0;
		m.steer_target_bam = m.yaw_bam;
	}
}

void VehicleSystem::aircraft_client_tick(Entity &veh, const VehicleTraits &traits) {
    World &world = world_;
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

	vehicle_refresh_ground_link(world, veh, traits);
	const int32_t ground = m.ground_cache;

	const bool motor_is_authority = world.ai.is_authority;
	const bool burning = veh.health > 0 && veh.health <= traits.critical_hp;
	tick_health(veh, traits);
	// The hull spiral shares the authority health cadence; the pilot look
	// follow below it is a separate, per-tick write.
	// [orig: Entity_UpdateAircraftPhysics @0x490310, hull altitude/yaw
	// @0x49048E..0x49049F; the separate pilot altitude read is @0x4904BE]
	if (motor_is_authority && burning &&
			((world.logic_tick + 36u * static_cast<uint32_t>(veh.net_id)) & 63u) == 0 &&
			(veh.flags & kEntityFlagInAir) != 0 && ground != INT32_MIN &&
			m.net_alt_target - ground > 0x10000) {
		m.yaw_bam = io::bam_sub(m.yaw_bam, 2886390);
	}
	if (burning && (veh.flags & kEntityFlagInAir) != 0 && ground != INT32_MIN &&
			io::bam_sub(m.net_alt_target, ground) > 0x10000) {
		if (Entity *pilot = world.vehicles.resolve_controller(veh)) {
			if ((pilot->net_move_input & Entity::kMoveOrderFreeLook) == 0)
				turn_pilot_view(world, *pilot, -2886390);
		}
	}
	// Update BEFORE the rotor-up/input gate: collective becomes available on
	// the very tick the rotor reaches full speed.
	// [orig: Entity_UpdateAircraftPhysics @0x490310, call gate @0x490592..0x4905A6]
	if (veh.health > 0 && (veh.flags & 1u) == 0)
		part_anim_tick(veh, traits);

	vehicle_follow_carrier(world, veh);
	px = to_fixed(veh.position.x);
	py = to_fixed(veh.position.y);
	pz = to_fixed(veh.position.z);

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
		// Both altitude registers seed on a new record. The engine-on arm
		// samples ground at the received pose; engine-off clears climb.
		// [orig: @0x490998..0x490B33]
		m.net_alt_target =
				m.net_engine_on ? m.net_smooth_target[2] : m.net_smooth_target[2] - 0x4000;
		if (m.net_engine_on) {
			const int32_t target_ground =
					vehicle_ground_height_at(world, veh, traits, m.net_smooth_target);
			if (target_ground != INT32_MIN)
				m.net_climb = io::bam_sub(m.net_smooth_target[2], target_ground);
		} else
			m.net_climb = 0;
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

	Entity *input_pilot = resolve_piloting_player(world, veh, traits);
	if (!ai_drive && input_pilot == nullptr && m.net_predicted) {
		m.cmd_speed = m.net_recv_speed;
		m.cmd_lateral_speed = m.net_recv_lat;
		m.steer_target_bam = m.net_recv_steer_bam;
	}

	// Dead hulls finish the carried/interpolated pose but skip their live
	// controls, velocity and gear servos [orig: @0x490CD0 -> @0x49274C].
	if (((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0) {
		veh.position = { float(from_fixed(px)), float(from_fixed(py)), float(from_fixed(pz)) };
		return;
	}

	// +0x45C follows the pilot's relative pitch. The unoccupied arm adds
	// its own signed step without the occupied clamp; this odd sign is
	// present in retail. [orig: @0x490E66..0x490EF3]
	Entity *view_pilot = world.vehicles.resolve_controller(veh);
	int32_t tilt_error = m.view_tilt_bam;
	if (view_pilot != nullptr) {
		const AiEntity *body = world.ai.for_handle(view_pilot->handle);
		const int32_t pitch =
				body != nullptr ? body->pitch : bam_from_degrees_wrapped(view_pilot->pitch);
		tilt_error = io::bam_sub(io::bam_sub(pitch, m.air_pitch_bam), m.view_tilt_bam);
	}
	int32_t tilt_step = io::bam_sar(io::bam_add(tilt_error, 4), 3);
	if (tilt_step > 1924260)
		tilt_step = 3848520;
	else if (tilt_step < -1924260)
		tilt_step = -3848520;
	m.view_tilt_bam = io::bam_add(m.view_tilt_bam, tilt_step);
	if (view_pilot != nullptr)
		m.view_tilt_bam = std::clamp(m.view_tilt_bam, -298261600, 0);

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
	} else if (input_pilot != nullptr) {
		stage_air_vehicle_input(world, veh, *input_pilot, traits, ground, pz);
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
	}

	// The non-drivable authority arm keeps brain[137]'s hover clearance and
	// writes brain[131] = ground + clearance before the ordinary rotor gate.
	// Clients retain their received altitude target.
	// [orig: Entity_UpdateAircraftPhysics @0x491da7..0x491dc3]
	if (!traits.player_control && motor_is_authority && ground != INT32_MIN)
		m.net_alt_target = io::bam_add(ground, m.net_climb);

	// ---- 2a. Engine flag. Retail splits this by role: the AUTHORITY DERIVES the
	// flag from the climb-above-ground register every tick, and only a CLIENT
	// runs the engine-off override (`if (!is_authority) goto LABEL_305`). We
	// retain [548] independently near the ground.
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
		// cold helicopter sits through. The head spin tick makes this gate see
		// the current tick's rate, including the exact full-speed crossing.
		const AiEntity *rotor_ai = world.ai.for_handle(veh.handle);
		// The helo helper returns success immediately for a different profile.
		// [orig: Entity_UpdateHeloRotorSpin @0x48FA70, entry class gate]
		const bool needs_runup = rotor_ai == nullptr || rotor_ai->profile.type == 1;
		const bool rotor_up = veh.health > 0 && (veh.flags & 0x1u) == 0 &&
				(!needs_runup || m.part_spin.speed >= kRotorSpeedMax);
		if (!rotor_up) {
			if (ground != INT32_MIN)
				m.net_alt_target = ground - 0x2000;
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
		if (ground != INT32_MIN)
			m.net_alt_target = ground - 0x2000;
		m.cmd_speed = 0;
        m.cmd_lateral_speed = 0;
        m.steer_target_bam = m.yaw_bam;
	}

	int32_t turret_step = 0;
	// ---- 3. Yaw servo (second order) [orig: @0x491CC8..0x491D27].
	{
		int32_t step = io::bam_sar(io::bam_add(
                io::bam_sub(m.steer_target_bam, m.yaw_bam), 8), 4);
        const int32_t tr = traits.turn_rate;
        if (step > tr) step = tr;
        const int32_t neg_tr = io::bam_sub(0, tr);
        if (step < neg_tr) step = neg_tr;
		turret_step = step;
		m.wheel_rate_bam = io::bam_add(m.wheel_rate_bam, io::bam_sar(io::bam_add(step, 4), 3));
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
		m.vel_x = (io::bam_add(bam_mul_wrap(1019, m.vel_x), 512) >> 10);
		m.vel_y = (io::bam_add(bam_mul_wrap(1019, m.vel_y), 512) >> 10);
		// Occupied aircraft whose pilot lacks Flags 0x100 receive another
		// planar drag pass. IMUL/ADD are low-dword operations, then SAR10.
		// [orig: Entity_UpdateAircraftPhysics @0x49217E..0x4921C7]
		const Entity *drag_pilot = world.registry.get(veh.primary_occupant);
		if (drag_pilot != nullptr &&
				((drag_pilot->flags | drag_pilot->engine_flags) & 0x100u) == 0) {
			m.vel_x = io::bam_add(bam_mul_wrap(1019, m.vel_x), 512) >> 10;
			m.vel_y = io::bam_add(bam_mul_wrap(1019, m.vel_y), 512) >> 10;
		}
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
			if (veh.health <= 0) {
				veh.health = 0;
				veh.last_attacker = {};
			}
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
	// attacker slot, cleared on the kill edge].
	if (motor_is_authority && veh.health > 0 &&
        (io::bam_abs(m.air_roll_bam) > 0x471C7180 ||
         io::bam_abs(m.air_pitch_bam) > 0x471C7180)) {
        veh.health -= 200;
		if (veh.health <= 0) {
			veh.health = 0;
			veh.last_attacker = {};
		}
	}

	// The automatic gear register uses the already-sampled ground clearance.
	// Five units or more retracts it toward 0xFFFF; below five extends to zero.
	// [orig: @0x492676..0x49268D (+0x472 bit 0), @0x492703..0x492743]
	const int32_t gear_delta = io::bam_sub(pz, 0x50000) >= ground ? 1598 : -1598;
	m.gear_phase = static_cast<uint16_t>(std::clamp(int32_t(m.gear_phase) + gear_delta, 0, 65535));

	// Aircraft uses the retained, signed yaw demand as the slew amount.
	// [orig: @0x4926EB/@0x4926F9 reads var_B4, stored at @0x491CE4..0x491CF8]
	slew_turret(veh, turret_step);

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
}

} // namespace opennova::world
