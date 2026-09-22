#include "vehicle_motor_detail.h"

#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>

namespace opennova::world::detail {

bool vehicle_has_contact_direction(const Entity::VehicleMotorState &m) {
	return m.contact_direction[0] != 0 || m.contact_direction[1] != 0 ||
			m.contact_direction[2] != 0;
}

namespace {

void clear_direction(Entity::VehicleMotorState &m) {
	for (int32_t &component : m.contact_direction)
		component = 0;
	m.slip_started_tick = 0;
}

int32_t dot_q16(const int32_t a[3], const int32_t b[3]) {
	return io::bam_add(
			io::bam_add(q16_mul_rhu(a[0], b[0]), q16_mul_rhu(a[1], b[1])), q16_mul_rhu(a[2], b[2]));
}

void cross_q16(const int32_t a[3], const int32_t b[3], int32_t out[3]) {
	int64_t raw[3];
	q16_cross(a, b, raw);
	for (int i = 0; i < 3; ++i)
		out[i] = int32_t(raw[i]);
}

bool sharp_steering(const Entity::VehicleMotorState &m) {
	// This is the wheel-angle register +0x2B4, not chassis pitch: its folded
	// angle in degrees (x 360/2^32, +360 when negative, 360-x above 180) against
	// 4.0, once for the acceleration caps and once for the velocity arms.
	// [orig: Entity_UpdateTankVehiclePhysics @0x488AB0, @0x489D24..0x489D77 and
	//  @0x48A00B..0x48A03D; flt_7C3BA8/7C3BA4/7C69EC/7C44B8]
	return std::abs(double(m.steer_state) * 8.381903171539307e-8) > 4.0;
}

void velocity_from_direction(
		Entity::VehicleMotorState &m, const int32_t dir[3], int32_t speed, bool replace_z) {
	m.vel_x = q16_mul_rhu(speed, dir[0]);
	m.vel_y = q16_mul_rhu(speed, dir[1]);
	if (replace_z)
		m.slide_z = q16_mul_rhu(speed, dir[2]);
}

// Assemble the direction frame with unnormalized Q16 cross products, rotate
// locally, then extract column zero. Do not normalize the matrix: the original
// preserves the cross-product quantization and length.
// [orig: Math_FixedPointCrossProduct @0x6134A0; Math_SetRow0FromVec3Scaled
// @0x613820; Math_BuildFixedPointRotationMatrixYXZ @0x615400 ->
// Matrix_Multiply3x4_FixedPoint @0x613940; cveh @0x48CA61..0x48CB21]
void turn_contact_direction(
		Entity::VehicleMotorState &m, const int32_t forward[3], const int32_t up[3], int32_t step) {
	int32_t side[3], turn[3];
	cross_q16(up, m.contact_direction, side);
	cross_q16(m.contact_direction, forward, turn);
	if (turn[2] <= 0)
		step = io::bam_sub(0, step);
	const int32_t c = cos22_of_bam_x87(step), s = sin22_of_bam_x87(step);
	for (int i = 0; i < 3; ++i) {
		const int32_t d22 = bam_shl_wrap(m.contact_direction[i], 6);
		const int32_t side22 = bam_shl_wrap(side[i], 6);
		const int32_t rotated22 =
				int32_t((int64_t(d22) * c + int64_t(side22) * s + 0x200000) >> 22);
		m.contact_direction[i] = rotated22 >> 6;
	}
}

} // namespace

// True means the skid branch supplied the complete acceleration, so the
// ordinary acceleration/deceleration clamp tree is skipped.
// [orig: cveh @0x48C330..0x48C3D0; cbik @0x4853EB..0x485501;
// ctan @0x489D79..0x489F19]
bool vehicle_traction_acceleration(
		World &world, Entity &vehicle, const VehicleTraits &traits, int32_t target_speed) {
	auto &m = vehicle.veh;
	// Crashed bike: -/+ deceleration into speedAccel, no clamp tree, speed==0
	// keeps the servo value [orig: Entity_UpdateLightVehiclePhysics @0x483FE0
	//  -- the +2EC branch @0x4853F1..0x4853F9 -> @0x485687..0x4856C5 (`neg
	//  eax` @0x48569D, `mov [esi+2A0h], eax` @0x4856A5/@0x4856BF, `jmp
	//  loc_4854EF` @0x4856C5)].
	if (traits.family == VehicleFamily::Bike && m.crashed) {
		if (m.speed > 0) m.speed_accel = io::bam_sub(0, traits.deceleration);
		if (m.speed < 0) m.speed_accel = traits.deceleration;
		return true;
	}
	if (!vehicle_has_contact_direction(m))
		return false;
	if (traits.family == VehicleFamily::Tank) {
		const bool reversal = (target_speed > 0 && m.speed <= 0) ||
				(target_speed < 0 && m.speed >= 0) || (target_speed == 0 && m.speed == 0);
		if (reversal || !sharp_steering(m))
			return false;
		const int32_t amount = io::bam_abs(m.speed) <= 4096 && target_speed != 0
				? traits.deceleration >> 2
				: bam_shl_wrap(traits.deceleration, 1);
		if (m.speed > 0)
			m.speed_accel = io::bam_sub(0, amount);
		if (m.speed < 0)
			m.speed_accel = amount;
		return true;
	}
	if (!m.grounded || m.settle_2f0 != 0)
		return false;
	m.skid_effects_requested = true; // Entity_SpawnBoneEffectsAtMask @0x458750
	const bool recovering = m.slip_started_tick != 0 && m.handbrake_latched == 0 &&
			int32_t(world.logic_tick - m.slip_started_tick) > bam_mul_wrap(5, traits.tire_slip);
	const int32_t amount = recovering ? traits.acceleration >> 4 : traits.deceleration;
	if (m.speed > 0)
		m.speed_accel = recovering ? amount : io::bam_sub(0, amount);
	if (m.speed < 0)
		m.speed_accel = recovering ? io::bam_sub(0, amount) : amount;
	return true;
}

// Before the velocity/contact solve: the wheel phase uses the drive speed,
// not its post-collision value. The wheelspin carry decays AFTER the phase add.
// [orig: cveh @0x48C330..0x48C535; cbik @0x4855C1..0x48586D]
void vehicle_wheel_traction_tick(
		World &world, Entity &vehicle, const VehicleTraits &traits, int32_t target_speed) {
	if (traits.family == VehicleFamily::Tank)
		return;
	auto &m = vehicle.veh;
	const Entity *occupant = world.registry.get(vehicle.primary_occupant);
	if (occupant == nullptr)
		return;
	if (traits.family == VehicleFamily::Bike) {
		if ((occupant->net_move_input & Entity::kMoveOrderLeanRight) != 0)
			return;
	} else if (m.handbrake_latched != 0)
		return;
	if (vehicle_has_contact_direction(m)) {
		// The initialized dword is negative and the comparison uses its ABS,
		// then stores the signed value (the repeated lock is intentional).
		// [orig: dword_81518C = 0xE8480000; @0x48C49F..0x48C4B4]
		constexpr int32_t kSlipPhase = -397934592;
		if (m.wheel_slip_phase < io::bam_abs(kSlipPhase))
			m.wheel_slip_phase = kSlipPhase;
		m.wheel_phase = wheel_phase_step(m.wheel_phase, m.speed, io::bam_abs(m.wheel_slip_phase));
	} else {
		m.wheel_phase = wheel_phase_step(m.wheel_phase, m.speed, io::bam_abs(m.wheel_slip_phase));
		if (target_speed == 0)
			m.wheel_slip_phase = 0;
		else if (m.wheel_slip_phase != 0) {
			const double factor = std::min(1.0, 1.0 - double(m.speed) * 1.627604251552839e-5);
			m.wheel_slip_phase = int32_t(factor * m.wheel_slip_phase);
			m.skid_effects_requested = true;
		}
	}
}

// Retain the previous drive direction throughout a handbrake skid, then
// turn it back toward the chassis in one-degree steps after the authored
// recovery window. Tanks use their separate five-degree/low-speed ladder.
// Returns true when a contact arm ran; the airborne and off-contact arms
// return false (they also skip the movement trails).
// [orig: Entity_UpdateVehiclePhysics @0x48AF00, @0x48C55A..0x48CF97;
// Entity_UpdateLightVehiclePhysics @0x483FE0, @0x48586D..0x48659B;
// Entity_UpdateTankVehiclePhysics @0x488AB0, @0x489FA0..0x48A82C]
bool vehicle_traction_velocity(
		World &world, Entity &vehicle, const VehicleTraits &traits, int32_t target_speed) {
	auto &m = vehicle.veh;
	const bool tank = traits.family == VehicleFamily::Tank;
	const bool bike = traits.family == VehicleFamily::Bike;
	// The basis builds before every gate in all three movers [orig: cveh
	// @0x48C55E..0x48C582; ctan @0x489FB3..0x489FD7; cbik @0x485891..0x4858B5].
	const auto basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	const int32_t forward[3] = { basis.q22.m[0] >> 6, basis.q22.m[4] >> 6, basis.q22.m[8] >> 6 };
	const int32_t up[3] = { basis.q22.m[2] >> 6, basis.q22.m[6] >> 6, basis.q22.m[10] >> 6 };
	// Flags 0x2000 (airborne) holds every register in all three movers
	// [orig: @0x48CE02..0x48CE04 / @0x4861F6..0x4861F8 / @0x48A684..0x48A686].
	if ((vehicle.flags & kEntityFlagInAir) != 0)
		return false;
	// Off contact. The ground/bike movers reach this arm on `+0x2F2 == 0 &&
	// +0x3CD == 0` (no handbrake); the tank on `+0x2F2 == 0` alone [orig: cveh
	// @0x48C587..0x48C5A6; cbik @0x4858CC..0x4858EB; ctan @0x489FEE..0x48A005].
	if (!m.grounded && (tank || m.handbrake_latched == 0)) {
		// Before the first solve nothing moves [orig: `cmp +0x3CE, 0; jz`
		// @0x48CE17 / @0x48620B / @0x48A699].
		if (!m.contact_solved_once)
			return false;
		// The GROUND mover alone refreshes or clears the slip stamp off contact
		// [orig: @0x48CE24..0x48CE61]; the cbik arm @0x4861F6..0x48621F and the
		// ctan arm @0x48A684..0x48A6AD carry no +0x3F8 handling.
		if (!tank && !bike) {
			if (m.slip_started_tick != 0 &&
					int32_t(world.logic_tick - m.slip_started_tick) <=
							bam_mul_wrap(5, traits.tire_slip))
				m.slip_started_tick = world.logic_tick;
			else
				clear_direction(m);
		}
		if (m.crashed == 0) {
			// Not crashed: the ground mover drives forward*speed into X/Y, and
			// into Z only while the nose points down [orig: `jz loc_48CF97`
			// @0x48CE6E; the store @0x48CF97..0x48D003 with `cmp var_110, 0;
			// jge` @0x48CFD9]; the tank holds every register [orig: `jz
			// loc_48A828` @0x48A6AD]. The bike's off-contact arm writes X/Y only:
			// a wheelie normalizes the 3-D launch vector, else the PLANAR
			// forward row (two squares, the third ftol of a zero), so a pitched
			// bike keeps its full planar speed [orig:
			// Entity_UpdateLightVehiclePhysics +3DE gate @0x4863DA; 3-D
			// normalize of +3E0..+3E8 @0x4863E3..0x486455; planar normalize of
			// the forward row @0x48645D..0x4864AB; X/Y stores @0x486572..0x486578].
			if (bike) {
				const int32_t *source = m.wheelie_active ? m.bike_launch_direction : forward;
				const int64_t raw[3] = {source[0], source[1],
						m.wheelie_active ? int64_t(source[2]) : int64_t(0)};
				int32_t direction[3];
				q16_normalize(raw, direction);
				velocity_from_direction(m, direction, m.speed, false);
				if (m.slip_started_tick != 0 &&
						int32_t(world.logic_tick - m.slip_started_tick) <= bam_mul_wrap(5, traits.tire_slip))
					m.slip_started_tick = world.logic_tick;
				else clear_direction(m);
			} else if (!tank)
				velocity_from_direction(m, forward, m.speed, forward[2] < 0);
			return false;
		}
		// Crashed: each family's own arm.
		if (tank) {
			// 3-D normalize, then -/+ 2*deceleration, then all three components
			// [orig: Entity_UpdateTankVehiclePhysics @0x488AB0 (site
			//  @0x48A6B3..0x48A7D3: `add ecx, ecx; sub eax, ecx` @0x48A752 /
			//  `lea edx, [eax+ecx*2]` @0x48A75E)].
			const int64_t motion[3] = { m.vel_x, m.vel_y, m.slide_z };
			int32_t dir[3];
			q16_normalize(motion, dir);
			const int32_t stop = bam_shl_wrap(traits.deceleration, 1);
			m.speed = m.speed > 0 ? io::bam_sub(m.speed, stop) : io::bam_add(m.speed, stop);
			velocity_from_direction(m, dir, m.speed, true);
		} else if (bike) {
			// +/-0x6000 speed clamp, 3-D normalize, -/+ 1*deceleration, all
			// three components [orig: Entity_UpdateLightVehiclePhysics @0x483FE0
			//  (site @0x486225..0x486379: the clamp @0x486232..0x48624F, `sub
			//  eax, [ecx+8E4h]` @0x4862EE / `add ecx, eax` @0x486305)].
			if (io::bam_abs(m.speed) > 0x6000)
				m.speed = m.speed < 0 ? -0x6000 : 0x6000;
			const int64_t motion[3] = { m.vel_x, m.vel_y, m.slide_z };
			int32_t dir[3];
			q16_normalize(motion, dir);
			m.speed = m.speed > 0 ? io::bam_sub(m.speed, traits.deceleration)
								  : io::bam_add(m.speed, traits.deceleration);
			velocity_from_direction(m, dir, m.speed, true);
		} else {
			// Planar normalize of X/Y only, NO speed step, Z untouched [orig:
			// Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48CE74..0x48CF46:
			//  `fild vel_y; fild vel_x` and the 65536/len products with the
			//  third ftol of a zero; the +0x98/+0x9C stores @0x48CF0E/@0x48CF46)].
			const int64_t motion[3] = { m.vel_x, m.vel_y, 0 };
			int32_t dir[3];
			q16_normalize(motion, dir);
			velocity_from_direction(m, dir, m.speed, false);
		}
		// The shared tail [orig: cveh @0x48CF41..0x48CF95; cbik @0x486386..
		// 0x4863D5; ctan @0x48A7E0..0x48A81F]: a live crash keeps skidding, a
		// slow one halves X/Y/speed, zeroes the three rates and settles.
		if (io::bam_abs(m.speed) >= 4096)
			m.skid_effects_requested = true;
		else {
			m.vel_x >>= 1;
			m.vel_y >>= 1;
			m.speed >>= 1;
			m.wheel_rate_bam = m.air_roll_rate = m.air_pitch_rate = 0;
			m.settle_2f0 = 1;
		}
		return false;
	}
	if (tank) {
		if (target_speed == 0 && sharp_steering(m)) {
			if (!vehicle_has_contact_direction(m)) {
				for (int i = 0; i < 3; ++i)
					m.contact_direction[i] = m.speed > 0 ? forward[i] : io::bam_sub(0, forward[i]);
			}
			m.contact_direction[2] = std::min(0, m.contact_direction[2]);
			velocity_from_direction(m, m.contact_direction, io::bam_abs(m.speed), true);
			m.wheel_rate_bam = q16_mul_rhu(-16384, m.steer_state >> 2);
			return true;
		}
		// The straight arm leaves any retained skid frame alone, and only it
		// keeps a crashed hull's vertical velocity.
		// [orig: @0x48A1E6..0x48A2BE, crashed Z gate @0x48A28D..0x48A2AE]
		if (target_speed != 0 && io::bam_abs(m.speed) > 8192) {
			velocity_from_direction(m, forward, m.speed, m.crashed == 0);
			m.wheel_rate_bam = q16_mul_rhu(io::bam_sub(0, m.speed), m.steer_state >> 2);
			return true;
		}
		// The recovery arm advances the stored skid frame but drives along the
		// current forward row; only the sharp, zero-command arm uses the frame.
		// Its slideDecay store is unconditional, crashed or not.
		// [orig: @0x48A2C3..0x48A607, store @0x48A5D4]
		if (vehicle_has_contact_direction(m)) {
			if (dot_q16(forward, m.contact_direction) >= 61439 || m.speed <= 1280)
				clear_direction(m);
			else
				turn_contact_direction(m, forward, up, 59652323);
		}
		velocity_from_direction(m, forward, m.speed, true);
		m.wheel_rate_bam = q16_mul_rhu(io::bam_sub(0, m.speed), m.steer_state >> 2);
		return true;
	}
	if (m.handbrake_latched != 0 && m.speed != 0 && vehicle_has_contact_direction(m)) {
		velocity_from_direction(m, m.contact_direction, m.speed, m.crashed == 0);
        if (bike) m.wheelie_active = 0; // [orig: @ 0x4859E0]
		return true;
	}
    if (bike && m.handbrake_latched == 0 && m.wheelie_active) {
        // [orig: Entity_UpdateLightVehiclePhysics @ 0x486052..0x486149]
        const int64_t raw[3] = {m.bike_launch_direction[0],
                m.bike_launch_direction[1], m.bike_launch_direction[2]};
        int32_t direction[3];
        q16_normalize(raw, direction);
        velocity_from_direction(m, direction, m.speed, m.crashed == 0);
        return true;
    }
	if (!vehicle_has_contact_direction(m)) {
		clear_direction(m);
		velocity_from_direction(m, forward, m.speed, m.crashed == 0);
		return true;
	}
	if (m.slip_started_tick == 0) {
		m.slip_started_tick = world.logic_tick;
		velocity_from_direction(m, m.contact_direction, m.speed, m.crashed == 0);
		return true;
	}
	if (int32_t(world.logic_tick - m.slip_started_tick) <= bam_mul_wrap(5, traits.tire_slip)) {
		velocity_from_direction(m, m.contact_direction, m.speed, m.crashed == 0);
		return true;
	}
	if (dot_q16(forward, m.contact_direction) < 0) {
		if (io::bam_abs(m.speed) > 256) {
			velocity_from_direction(m, m.contact_direction, m.speed, m.crashed == 0);
			if (m.speed >= 0)
				m.speed = io::bam_sub(m.speed, traits.acceleration);
			if (m.speed < 0)
				m.speed = io::bam_add(m.speed, traits.acceleration);
			return true;
		}
	} else {
		int32_t a[3], b[3];
		const int64_t planar_a[3] = { forward[0], forward[1], 0 };
		const int64_t planar_b[3] = { m.contact_direction[0], m.contact_direction[1], 0 };
		q16_normalize(planar_a, a);
		q16_normalize(planar_b, b);
		if (dot_q16(a, b) < 61166 && m.speed > 0) {
			turn_contact_direction(m, forward, up, 11930464);
			velocity_from_direction(m, m.contact_direction, m.speed, m.crashed == 0);
			return true;
		}
	}
	clear_direction(m);
	velocity_from_direction(m, forward, m.speed, m.crashed == 0);
	return true;
}

// Handbrake contact latches forward with uphill motion removed, normalized
// after the Z clamp. Ground requires both rear pads; bike requires its rear
// wheel. [orig: @0x47E65D..0x47E78F; @0x47B71E..0x47B838]
void vehicle_capture_contact_direction(Entity &vehicle, const VehicleEulerBasis &basis) {
	auto &m = vehicle.veh;
	if (m.handbrake_latched == 0)
		return;
	if (m.cmd_speed == 0 && (m.speed == 0 || vehicle_has_contact_direction(m)))
		return;
	const int64_t raw[3] = { basis.q22.m[0] >> 6, basis.q22.m[4] >> 6,
		std::min(0, basis.q22.m[8] >> 6) };
	q16_normalize(raw, m.contact_direction);
}

// Tank/bike contact tails bias an existing skid vector downhill after an
// actual descent, then renormalize it. [orig: @0x47951E..0x4795B7;
// @0x47C0EB..0x47C182]
void vehicle_contact_downhill_tail(Entity &vehicle, int32_t pz) {
	auto &m = vehicle.veh;
	if (!vehicle.saved_live_valid || vehicle.saved_live_pos[2] <= pz ||
			!vehicle_has_contact_direction(m))
		return;
	const int64_t raw[3] = { m.contact_direction[0], m.contact_direction[1], -28672 };
	q16_normalize(raw, m.contact_direction);
}

// The fxs emitter is transient at every masked userpoint, with no origin
// fallback. The source entity is an input context, not a following attachment.
// The camera range is 600 units every fourth tick, 300 on the intervening ticks.
// [orig: Entity_SpawnBoneEffectsAtMask @0x458750, gates @0x45875E..0x458836,
// rigid point/direction transform @0x4588F9..0x458986]
void vehicle_emit_skid_effects(World &world, Entity &vehicle, const VehicleTraits &traits) {
	if (!vehicle.veh.skid_effects_requested || vehicle.veh.movement_effects_disabled ||
			traits.skid_effect.empty() || traits.skid_points.empty() ||
			!world.out.fire_sounds.listener_valid())
		return;
	const Vec3 &camera = world.out.fire_sounds.listener();
	const int64_t dx = int64_t(to_fixed(vehicle.position.x)) - to_fixed(camera.x);
	const int64_t dy = int64_t(to_fixed(vehicle.position.y)) - to_fixed(camera.y);
	const int32_t limit = ((world.logic_tick & 3u) == 0 ? 600 : 300) << 16;
	if (std::sqrt(double(dx) * dx + double(dy) * dy) > limit)
		return;
	const bool snow = terrain::surface_type_at_fixed(world.tables.surface_map,
							  to_fixed(vehicle.position.x), to_fixed(vehicle.position.y)) == 3;
	const std::string &effect =
			snow && !traits.skid_snow_effect.empty() ? traits.skid_snow_effect : traits.skid_effect;
	const CollisionMatrix matrix = entity_placement_matrix(vehicle);
	for (const auto &point : traits.skid_points) {
		int32_t pos[3], dir[3];
		matrix.transform_point(point.position, pos);
		matrix.rotate_point(point.direction, dir);
		world.out.vehicle_effects.push_back({ effect,
				{ float(from_fixed(pos[0])), float(from_fixed(pos[1])), float(from_fixed(pos[2])) },
				{ float(from_fixed(dir[0])), float(from_fixed(dir[1])), float(from_fixed(dir[2])) },
				world.logic_tick + 1 });
	}
}

} // namespace opennova::world::detail
