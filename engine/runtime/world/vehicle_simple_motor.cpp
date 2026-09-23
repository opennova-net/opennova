#include "vehicle_motor_detail.h"

#include <runtime/audio/sound_profile.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/vehicle_system.h>
#include <runtime/world/world.h>

#include <optional>

namespace opennova::world {
using namespace detail;
namespace {

int32_t planar_length(int32_t x, int32_t y) {
	return int32_t(std::min(2147418112.0, std::hypot(double(x), double(y))));
}

void drain_health(World &world, Entity &e, int amount) {
	if (!world.ai.is_authority || e.health <= 0)
		return;
	e.health = int16_t(uint16_t(uint32_t(e.health) - uint32_t(amount)));
	if (e.health <= 0) {
		e.health = 0;
		e.last_attacker = {};
	}
}

// Both simple callbacks share this speed-dependent steering chase.
// [orig: Entity_ProcessInfantryPhysics @0x46E100;
// Entity_ProcessAirVehiclePhysics @0x46FA00 (the simple boat, despite its IDB name)]
void steer(Entity &e, const VehicleTraits &t, bool boat) {
	auto &m = e.veh;
	const int32_t maximum = boat ? t.water_speed : t.player_speed;
	const int32_t minimum = t.turn_rate2 != 0 ? t.turn_rate2 : t.turn_rate >> 2;
	int32_t fraction = 0;
	if (maximum != 0)
		fraction = std::max(0, io::bam_sub(65536, int32_t(int64_t(m.speed) * 65536 / maximum)));
	const int32_t limit =
			io::bam_add(minimum, q16_mul_rhu(io::bam_sub(t.turn_rate, minimum), fraction));
	int32_t delta = io::bam_sar(io::bam_add(io::bam_sub(m.steer_target_bam, m.yaw_bam), 32), 6);
	delta = std::max(io::bam_sub(0, limit), std::min(delta, limit));
	m.steer_state = io::bam_add(m.steer_state,
			io::bam_sar(io::bam_sub(io::bam_sub(4, bam_shl_wrap(delta, 5)), m.steer_state), 3));
	if ((e.flags & kEntityFlagInAir) == 0 || (boat && (e.flags & 0x8000u) != 0))
		m.wheel_rate_bam = q16_mul_rhu(io::bam_sub(0, m.speed), m.steer_state >> 2);
}

// Physics=0 speed, skidding, and the deliberately weaker -167 gravity.
// [orig: Entity_ProcessInfantryPhysics @0x46E100]
void ground_step(World &world, Entity &e, const VehicleTraits &t, int32_t &z) {
	auto &m = e.veh;
	const bool grounded = (e.flags & kEntityFlagInAir) == 0;
	if (grounded && m.speed != 0) {
		const int32_t seed =
				bam_shl_wrap(io::bam_add(to_fixed(e.position.x), to_fixed(e.position.y)), 14);
		m.air_roll_rate = io::bam_add(m.air_roll_rate, (seed ^ 0x16968000) >> 15);
		m.air_pitch_rate = io::bam_add(m.air_pitch_rate, (seed ^ 0x29690000) >> 15);
	}
	// The simple path samples the same 1024-entry cosine table as its input.
	const uint32_t index = (uint32_t(m.air_pitch_bam) + 0x200000u) >> 22;
	const int32_t cosine =
			int32_t(std::cos(double(index) * io::kRadiansPerBam * 4194304.0) * io::kQ22One);
	const int32_t slope = int32_t((int64_t(cosine) * cosine) >> 22);
	const int32_t target = int32_t((int64_t(slope) * m.cmd_speed) >> 22);
	m.speed_accel = io::bam_sar(io::bam_add(io::bam_sub(target, m.speed), 16), 5);
	const bool same_direction = (target >= 0 && m.speed > 0) || (target <= 0 && m.speed < 0);
	if (same_direction) {
		const Entity *parent = world.registry.get(e.emplacement_parent);
		if (target != 0 || parent == nullptr || (parent->flags & 0x100u) != 0) {
			const int32_t limit = target != 0 ? t.acceleration : t.deceleration;
			m.speed_accel = std::max(io::bam_sub(0, limit), std::min(m.speed_accel, limit));
		}
	}
	m.speed = io::bam_add(m.speed, m.speed_accel);
	if (target == 0 && io::bam_abs(m.speed) < 48)
		m.speed = 0;
	if (m.speed_accel == 0)
		m.speed = target;
	m.wheel_phase = io::bam_add(m.wheel_phase, bam_shl_wrap(m.speed, 13));
	if (grounded) {
		const auto basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
		const int32_t desired_x = q16_mul_rhu(m.speed, (basis.q22.m[0] >> 6));
		const int32_t desired_y = q16_mul_rhu(m.speed, (basis.q22.m[4] >> 6));
		const int32_t difference = io::bam_sub(m.vel_x, desired_x);
		// Retail loads the X difference twice in this length test.
		const int32_t deviation = planar_length(difference, difference);
		if (t.slip_speed != 0 && deviation > t.slip_speed) {
			m.vel_x = io::bam_add(
					m.vel_x, io::bam_sar(io::bam_add(io::bam_sub(desired_x, m.vel_x), 16), 5));
			m.vel_y = io::bam_add(
					m.vel_y, io::bam_sar(io::bam_add(io::bam_sub(desired_y, m.vel_y), 16), 5));
			int32_t heading = bam_of_atan2(m.vel_y, m.vel_x);
			const int32_t projection =
					int32_t((int64_t(planar_length(m.vel_x, m.vel_y)) *
									cos22_of_bam_x87(io::bam_sub(m.yaw_bam, heading))) >>
							22);
			if (projection < 0)
				heading = io::bam_add(heading, INT32_MIN);
			m.wheel_rate_bam = io::bam_add(m.wheel_rate_bam,
					q16_mul_rhu(int32_t(io::bam_abs(projection)) >> 4,
							io::bam_sub(heading, m.yaw_bam)));
			if (!m.skid_sound_latched)
				world.vehicles.play_contact_sound(e, t, 25);
			m.skid_sound_latched = true;
		} else {
			z = io::bam_add(z, q16_mul_rhu(m.speed, (basis.q22.m[8] >> 6)));
			m.vel_x = desired_x;
			m.vel_y = desired_y;
			m.skid_sound_latched = false;
		}
	}
	vehicle_sample_trails(world, e, t, target);
	m.slide_z = io::bam_sub(m.slide_z, 167);
	if ((e.flags & 0x8000u) != 0) {
		for (int32_t *v : { &m.vel_x, &m.vel_y, &m.slide_z })
			*v = io::bam_sub(*v, io::bam_sar(io::bam_add(*v, 2), 2));
		drain_health(world, e, 2);
	}
	if (grounded) {
		m.air_roll_rate =
				io::bam_add(m.air_roll_rate, bam_mul_wrap(t.turn_roll, m.wheel_rate_bam >> 6));
		m.air_pitch_rate = io::bam_add(
				m.air_pitch_rate, bam_mul_wrap(t.speed_pitch, bam_shl_wrap(m.speed_accel, 9)));
	}
}

// Return signed lateral speed for the post-contact banking write.
// [orig: Entity_ProcessAirVehiclePhysics @0x46FA00]
int32_t boat_step(
		World &world, Entity &e, const VehicleTraits &t, int32_t x, int32_t y, int32_t z) {
	auto &m = e.veh;
	if (m.cmd_speed == 0) {
		if (io::bam_abs(m.vel_x) < 384)
			m.vel_x = 0;
		if (io::bam_abs(m.vel_y) < 384)
			m.vel_y = 0;
	} else {
		const int32_t magnitude = int32_t(io::bam_abs(m.cmd_speed));
		const int32_t push =
				io::bam_add(t.acceleration, io::bam_add(magnitude >> 7, magnitude >> 8));
		const int32_t thrust = m.cmd_speed >= 0 ? std::min(push, m.cmd_speed)
												: std::max(io::bam_sub(0, push), m.cmd_speed);
		const auto basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
		m.vel_x = io::bam_add(m.vel_x, q16_mul_rhu(thrust, (basis.q22.m[0] >> 6)));
		m.vel_y = io::bam_add(m.vel_y, q16_mul_rhu(thrust, (basis.q22.m[4] >> 6)));
		m.wheel_phase = io::bam_add(m.wheel_phase, bam_shl_wrap(m.cmd_speed, 13));
	}
	m.vel_x = io::bam_sub(m.vel_x, m.vel_x >> 6);
	m.vel_y = io::bam_sub(m.vel_y, m.vel_y >> 6);
	const int32_t slip = io::bam_sub(m.yaw_bam, bam_of_atan2(m.vel_y, m.vel_x));
	const int32_t magnitude = std::min(65536, planar_length(m.vel_x, m.vel_y));
	const int32_t lateral = int32_t((int64_t(magnitude) * sin22_of_bam_x87(slip)) >> 22);
	m.speed = int32_t((int64_t(magnitude) * cos22_of_bam_x87(slip)) >> 22);
	const int32_t beam = io::bam_add(m.yaw_bam, 0x3fffffc0);
	m.vel_x = io::bam_add(m.vel_x, int32_t((int64_t(lateral >> 5) * cos22_of_bam_x87(beam)) >> 22));
	m.vel_y = io::bam_add(m.vel_y, int32_t((int64_t(lateral >> 5) * sin22_of_bam_x87(beam)) >> 22));
	m.air_roll_rate = io::bam_sub(m.air_roll_rate, io::bam_sar(io::bam_add(m.air_roll_rate, 8), 4));
	m.air_pitch_rate =
			io::bam_sub(m.air_pitch_rate, io::bam_sar(io::bam_add(m.air_pitch_rate, 8), 4));
	m.air_roll_bam = io::bam_sub(m.air_roll_bam, io::bam_sar(io::bam_add(m.air_roll_bam, 16), 5));
	m.air_pitch_bam =
			io::bam_sub(m.air_pitch_bam, io::bam_sar(io::bam_add(m.air_pitch_bam, 16), 5));
	if (m.speed > m.cmd_speed) {
		m.vel_x = io::bam_sub(m.vel_x, m.vel_x >> 6);
		m.vel_y = io::bam_sub(m.vel_y, m.vel_y >> 6);
	}
	auto shore_drag = [&] {
		m.vel_x = io::bam_sub(m.vel_x, io::bam_sar(io::bam_add(m.vel_x, 4), 3));
		m.vel_y = io::bam_sub(m.vel_y, io::bam_sar(io::bam_add(m.vel_y, 4), 3));
		m.wheel_rate_bam =
				io::bam_sub(m.wheel_rate_bam, io::bam_sar(io::bam_add(m.wheel_rate_bam, 2), 2));
	};
	if ((e.flags & 0x8000u) != 0) {
		// The heave/roll wave, the x87 sequence verbatim [orig:
		// Entity_ProcessAirVehiclePhysics @0x46FA00 (site @0x47115D..0x4711D9)]:
		// P = fild(((x+y)>>10) + tick*4) * flt_7C6950 (1/256);
		// pitch = ftol(cos(1.4P)*327680 + cos(P)*655360) (dbl_7C6F58/7C6F50/7C6F48);
		// roll = ftol(sin(0.8P)*524288 + sin(1.2P)*655360) (dbl_7C6F40/7C6F38 and the
		// leftover 655360) — NO 1.6 on the roll path; the 1.6 (dbl_7C6F28) scales the
		// HEIGHT phase: height = -1024 - ftol(sin(1.6P) * -1024.0) (dbl_7C6F20,
		// @0x4711BD..0x4711D9). The >>4 / >>2 bound-radius shifts are @0x4711DD..0x4711FC.
		const int32_t phase_word = io::bam_add(
				io::bam_sar(io::bam_add(x, y), 10), bam_shl_wrap(int32_t(world.logic_tick), 2));
		const double phase = double(phase_word) * 0.00390625;
		int32_t pitch = int32_t(std::cos(phase * 1.4) * 327680.0 + std::cos(phase) * 655360.0);
		int32_t roll = int32_t(std::sin(phase * 0.8) * 524288.0 + std::sin(phase * 1.2) * 655360.0);
		const int32_t radius = to_fixed(e.bound_radius);
		const int shift = radius > 786432 ? 4 : radius > 393216 ? 2 : 0;
		m.air_pitch_bam = io::bam_add(m.air_pitch_bam, pitch >> shift);
		m.air_roll_bam = io::bam_add(m.air_roll_bam, roll >> shift);
		const int32_t height = -1024 - int32_t(std::sin(phase * 1.6) * -1024.0);
		m.slide_z = io::bam_sar(io::bam_sub(io::bam_add(world.env.water_z, height), z), 1);
	} else {
		m.slide_z = io::bam_sub(m.slide_z, 167);
		shore_drag();
	}
	if (world.tables.terrain != nullptr) {
		const int32_t ahead[3] = { io::bam_add(x, m.vel_x), io::bam_add(y, m.vel_y), z };
		if (calc_average_ground_height(*world.tables.terrain, ahead, 0, GroundClearance{}) >=
				world.env.water_z)
			shore_drag();
	}
	return lateral;
}
} // namespace

// Selector-zero callbacks, reached by cveh/ctrn/catv/cbot dispatchers.
// The amphibian selects the entire dry/wet motor before staging input.
// [orig: Entity_ProcessInfantryPhysics @0x46E100;
// Entity_ProcessAirVehiclePhysics @0x46FA00; Entity_DispatchPhysicsUpdate @0x48F010]
void VehicleSystem::tick_simple_motor(
		Entity &e, const VehicleTraits &traits, const VehicleDriveCmd *ai_cmd, bool prediction) {
	// The family override is the only difference the afloat amphibian needs.
	// Every other row — and an amphibian handed the already-overridden row by
	// the watercraft entry — reads the table entry in place, so the hot path
	// never copies the strings/vectors/trail block per tick.
	std::optional<VehicleTraits> afloat_override;
	if (traits.amphibian && (e.flags & 0x8000u) != 0 &&
			traits.family != VehicleFamily::Watercraft) {
		afloat_override.emplace(traits);
		afloat_override->family = VehicleFamily::Watercraft;
	}
	const VehicleTraits &t = afloat_override ? *afloat_override : traits;
	const bool boat = t.family == VehicleFamily::Watercraft;
	auto &m = e.veh;
	if (prediction && !m.net_predicted)
		return;
	if (!m.yaw_seeded) {
		// The unmoved row's placement angles, as the entry copy reads them.
		// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6;
		//  Entity_ProcessInfantryPhysics @0x46E133/@0x46E13C/@0x46E14C]
		m.yaw_bam = spawn_angle_bam(90 - e.yaw);
		m.air_pitch_bam = spawn_angle_bam(e.pitch);
		m.air_roll_bam = spawn_angle_bam(e.roll);
		m.yaw_seeded = true;
	}
	stamp_saved_live_pose(e);
	vehicle_refresh_ground_link(world_, e, t);
	vehicle_follow_carrier(world_, e);
	tick_health(e, t);
	if (((e.flags | e.engine_flags) & kEntityFlagDead) != 0)
		return;
	Entity *controller = t.player_control ? resolve_controller(e) : nullptr;
	if (controller != nullptr && (!controller->alive || controller->health <= 0))
		controller = nullptr;
	if (boat && t.player_control) {
		// The selector-zero boat's PlayerControl block sits at its HEAD, before
		// the authority split, so it runs for the authority and for a client's
		// prediction of a hull it does not drive: the claimant engine start/stop
		// edge, then the GROUND-profile part-spin machine (d_lcac is `type
		// GROUND`; DLCAC1's fan consumes HELO_TAILROTOR) — never the wheel-phase
		// step, which this mover writes from its command inside boat_step.
		// [orig: Entity_ProcessAirVehiclePhysics @0x46FA00 (the selector-zero
		//  boat mover): `test byte [itemDef+54h],40h` @0x47004B, claimant edge
		//  @0x470055..0x4700EB, Entity_UpdatePartSpinAccumulator @0x4700F5, the
		//  is_authority split @0x470109; dispatch Entity_DispatchPhysics_cbot
		//  @0x48EF97/@0x48EFAC, Entity_DispatchPhysicsUpdate @0x48F023/@0x48F035]
		update_claimant_engine_sound(e, t);
		rotor_machine_tick(e, t);
	}
	if (prediction) {
		// Both selector-zero movers run the plain chase template [orig:
		// Entity_ProcessInfantryPhysics @0x46E62E..0x46E85A;
		// Entity_ProcessAirVehiclePhysics @0x470129..0x470355].
		vehicle_client_chase(e, VehicleChaseFamily::Plain, 0);
		if (controller != nullptr && controller->handle == world_.cached.local_player) {
			stage_player_vehicle_input(world_, e, *controller, t);
			m.cmd_speed = io::bam_sar(io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
		} else {
			m.cmd_speed = m.net_recv_speed;
			m.steer_target_bam = m.net_recv_steer_bam;
		}
	} else if (t.player_control) {
		if (controller == nullptr) {
			m.steer_target_bam = m.yaw_bam;
			m.cmd_speed = 0;
			m.steer_ramp_bam = 0;
			e.flags &= ~0x80u;
		} else if (controller->handle.pool() == 0 && controller->player_class != 0 &&
				!watercraft_driver_submerged(world_, *controller)) {
			// A player driver whose eye (CameraOffset.z + Z) sits at or below the
			// water plane loses the wheel on the ground selector-zero mover too,
			// not only the boat [orig: Entity_ProcessInfantryPhysics @0x46E100
			// (site @0x46EA2E..0x46EA3A `add eax,[ecx+0Ch]; cmp eax,
			// Env_WaterHeightFixed; jle loc_46ECB1` — the AI waypoint leg)].
			stage_player_vehicle_input(world_, e, *controller, t);
			m.stuck_ticks = 0;
		} else if (ai_cmd != nullptr && ai_cmd->ai_drive) {
			m.steer_target_bam = ai_cmd->steer_target_bam;
			m.cmd_speed = ai_cmd->cmd_speed;
			m.steer_ramp_bam = 0;
			m.stuck_ticks = 0;
		}
	}
	steer(e, t, boat);
	int32_t x = to_fixed(e.position.x), y = to_fixed(e.position.y), z = to_fixed(e.position.z);
	int32_t lateral = 0;
	if (boat)
		lateral = boat_step(world_, e, t, x, y, z);
	else
		ground_step(world_, e, t, z);
	const int32_t longitudinal = m.speed;
	x = io::bam_add(x, m.vel_x);
	y = io::bam_add(y, m.vel_y);
	z = io::bam_add(z, m.slide_z);
	m.wheel_rate_bam = io::bam_sub(
			io::bam_sub(m.wheel_rate_bam, io::bam_sar(io::bam_add(m.wheel_rate_bam, 16), 5)),
			m.wheel_rate_bam >> 31);
	vehicle_simple_contact(world_, e, t, x, y, z);
	for (int32_t *rate : { &m.air_roll_rate, &m.air_pitch_rate, &m.wheel_rate_bam })
		*rate = std::clamp(*rate, -178956960, 178956960);
	m.air_roll_bam = io::bam_add(m.air_roll_bam, m.air_roll_rate);
	m.air_pitch_bam = io::bam_add(m.air_pitch_bam, m.air_pitch_rate);
	m.yaw_bam = io::bam_add(m.yaw_bam, m.wheel_rate_bam);
	if (io::bam_abs(m.air_roll_bam) > 1193046400u || io::bam_abs(m.air_pitch_bam) > 1193046400u)
		drain_health(world_, e, boat ? 200 : 20);
	if (boat && (e.flags & 0x8000u) != 0) {
		const int32_t pitch = bam_mul_wrap(longitudinal, t.speed_pitch);
		if (pitch > 0) {
			m.air_pitch_bam = io::bam_add(m.air_pitch_bam, bam_shl_wrap(pitch, 7));
			m.air_roll_bam = io::bam_sub(
					m.air_roll_bam, bam_shl_wrap(bam_mul_wrap(lateral, t.turn_roll), 8));
		}
	}
	e.position = { float(from_fixed(x)), float(from_fixed(y)), float(from_fixed(z)) };
	e.yaw = int16_t(std::lround(mission_yaw_deg_from_bam_heading(m.yaw_bam)));
	e.pitch = int16_t(std::lround(double(m.air_pitch_bam) * 360.0 / 4294967296.0));
	e.roll = int16_t(std::lround(double(m.air_roll_bam) * 360.0 / 4294967296.0));
	e.flags |= 0x20000u;
	slew_turret(e, 34636833);
	if (boat) {
		const int32_t contacted_speed = m.speed;
		m.speed = longitudinal;
		vehicle_sample_trails(world_, e, t, m.cmd_speed);
		m.speed = contacted_speed;
	}
	if (boat) {
		// The selector-zero boat sounds its lights first, then runs the movement
		// fold with the velocity magnitude (ftol of sqrt(vx^2 + vy^2 + vz^2),
		// flt_7C19E0 clamp) standing in for a zero speed word and no fold at all
		// when neither waterSpeed, brain speed A nor the command resolves, then
		// the direction latch inside the fold. It has no high-rev edge; its
		// claimant start/stop edge and part spin ran at the HEAD (the
		// PlayerControl block above, before the authority split).
		// [IDB: Entity_ProcessAirVehiclePhysics @0x46FA00 (a misnomer: the
		//  selector-zero boat mover): lights @0x4714EA..0x471523 (slot 24,
		//  +0x318 bit 2), the sound-system gate dword_24E0E80 @0x471523, fold
		//  gate @0x471533..0x471546, speed substitute @0x47154C..0x4715A5,
		//  maximum @0x4715AA..0x4715C6, fold @0x4715D2, direction latch
		//  @0x4715DA..0x471667, tail @0x47166F]
		const bool lights = (e.flags & 0x80u) != 0;
		if (lights && !m.light_sound_latched)
			play_contact_sound(e, t, audio::kSlotAudio1);
		m.light_sound_latched = lights;
		// The water client wrapper's caller emits continuous sound once.
		if (!prediction || traits.family != VehicleFamily::Watercraft) {
			int32_t maximum = t.water_speed;
			if (maximum == 0)
				if (const AiEntity *ai = world_.ai.for_handle(e.handle))
					maximum = ai->brain.f[AiBrain::kSpeedA];
			if (maximum == 0)
				maximum = m.cmd_speed;
			if (maximum != 0) {
				const int32_t speed_word = m.speed;
				if (speed_word == 0)
					m.speed = static_cast<int32_t>(std::min(
							std::sqrt(double(m.vel_x) * m.vel_x + double(m.vel_y) * m.vel_y +
									double(m.slide_z) * m.slide_z),
							2147418112.0));
				update_ground_sound(e, t, e.health <= 0, false);
				m.speed = speed_word;
			}
		}
		return;
	}
	// The selector-zero ground mover shares the cveh sound tail verbatim: the
	// movement fold (maximum = playerSpeed, brain speed A, then the command;
	// collided = airborne ticks > 30), the direction latch, the high-rev edge,
	// the lights edge, the claimant start/stop edge (slot 30 on the occupant
	// eye above water, the all-zero fold plus slot 31 on the hull +0x18000)
	// and the part spin, then the rev timer.
	// [IDB: Entity_ProcessInfantryPhysics @0x46E100 (a misnomer: the
	//  selector-zero ground mover): gate dword_24E0E80 @0x46F7C8, fold
	//  @0x46F7D4..0x46F825, direction latch @0x46F828..0x46F888, high-rev
	//  @0x46F88B..0x46F8C0, lights @0x46F8C3..0x46F8FC, claimant edge
	//  @0x46F8FC..0x46F99C, part spin @0x46F99E, timer @0x46F9A6]
	update_ground_sound(e, t, e.health <= 0, m.plat_airborne_ticks > 30);
	// The high-rev edge sits inside the same last-tick gate as the fold.
	// [orig: Entity_ProcessInfantryPhysics @0x46F7C8..0x46F7CE]
	if (world_.rules.last_tick_of_batch && controller != nullptr && m.rev_sound_ticks > 124 &&
			m.plat_airborne_ticks > 30) {
		play_contact_sound(e, t, 33);
		m.rev_sound_ticks = 0;
	}
	update_engine_sound(e, t);
	if (t.player_control)
		part_anim_tick(e, t);
	++m.rev_sound_ticks;
}
} // namespace opennova::world
