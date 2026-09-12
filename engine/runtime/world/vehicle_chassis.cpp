#include "vehicle_motor_detail.h"
#include "world.h"

namespace opennova::world::detail {
namespace {
CollisionMatrix identity() {
	CollisionMatrix result;
	result.m[0] = result.m[5] = result.m[10] = result.m[15] = 4194304;
	return result;
}
// A complete dot product receives ONE rounding bias before SHRD22.
// [orig: Matrix_Multiply3x4_FixedPoint @0x613940]
CollisionMatrix multiply(const CollisionMatrix &a, const CollisionMatrix &b) {
	CollisionMatrix result = identity();
	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 4; ++col) {
			uint64_t sum = 0x200000;
			for (int k = 0; k < 3; ++k)
				sum += uint64_t(int64_t(a.m[row * 4 + k]) * b.m[k * 4 + col]);
			const int32_t product = int32_t(sum >> 22);
			result.m[row * 4 + col] = col == 3 ? io::bam_add(product, a.m[row * 4 + 3]) : product;
		}
	}
	return result;
}
void normalize(int32_t v[3]) {
	const int64_t wide[3] = { v[0], v[1], v[2] };
	q16_normalize(wide, v);
}
// [orig: Math_FixedPointMatrixToFloat3x3 @0x615FA0 -> @0x611080;
// Math_Matrix3x3ToQuaternion @0x615C70]
void matrix_quaternion(const CollisionMatrix &matrix, float q[4]) {
	const auto *m = matrix.m;
	const float f[9] = { float(m[5] / 4194304.0), float(-double(m[9]) / 4194304.0),
		float(-double(m[1]) / 4194304.0), float(-double(m[6]) / 4194304.0),
		float(m[10] / 4194304.0), float(m[2] / 4194304.0), float(-double(m[4]) / 4194304.0),
		float(m[8] / 4194304.0), float(m[0] / 4194304.0) };
	const double trace = double(f[4]) + f[0] + f[8];
	if (trace > 0) {
		const double root = std::sqrt(trace + 1.0), inv = 0.5 / root;
		q[3] = float(root * 0.5);
		q[0] = float((double(f[5]) - f[7]) * inv);
		q[1] = float((double(f[6]) - f[2]) * inv);
		q[2] = float((double(f[1]) - f[3]) * inv);
	} else {
		const int i = f[4] >= f[0] ? (f[4] >= f[8] ? 1 : 2) : (f[0] >= f[8] ? 0 : 2);
		const int j = (i + 1) % 3, k = (i + 2) % 3;
		const double root = std::sqrt(double(f[4 * i]) - f[4 * j] - f[4 * k] + 1.0),
					 inv = 0.5 / root;
		q[i] = float(root * 0.5);
		q[j] = float((double(f[3 * i + j]) + f[3 * j + i]) * inv);
		q[k] = float((double(f[3 * i + k]) + f[3 * k + i]) * inv);
		q[3] = float((double(f[3 * j + k]) - f[3 * k + j]) * inv);
	}
}
// The only target here is the identity quaternion; preserve retail's 0.01
// linear threshold and unnormalized result. [orig: Math_QuaternionSlerp @0x615E20]
void blend_identity(const float q[4], float t, float out[4]) {
	const double dot = std::abs(double(q[3]));
	double a = 1.0 - double(t), b = t;
	if (1.0 - dot > double(0.01f)) {
		const double omega = std::acos(dot), inv = 1.0 / std::sin(omega);
		a = std::sin((1.0 - double(t)) * omega) * inv;
		b = std::sin(double(t) * omega) * inv;
	}
	for (int i = 0; i < 3; ++i)
		out[i] = float(double(q[i]) * a);
	out[3] = float(double(q[3]) * a + (q[3] < 0 ? -b : b));
}
// [orig: Math_QuaternionToMatrix3x3 @0x615A70; @0x616010 -> @0x611140]
CollisionMatrix quaternion_matrix(const float q[4]) {
	const double tx = double(q[0]) * 2, ty = double(q[1]) * 2, tz = double(q[2]) * 2;
	const double xx = q[0] * tx, xy = q[0] * ty, xz = q[0] * tz, yy = q[1] * ty;
	const float yz = float(q[1] * tz), zz = float(q[2] * tz), wz = float(q[3] * tz);
	const double wx = q[3] * tx, wy = q[3] * ty;
	const float render[16] = { float(1.0 - (double(zz) + yy)), float(wz + xy), float(xz - wy), 0,
		float(xy - wz), float(1.0 - (double(zz) + xx)), float(wx + yz), 0, float(xz + wy),
		float(yz - wx), float(1.0 - (xx + yy)), 0, 0, 0, 0, 1 };
	CollisionMatrix result;
	collision_matrix_apply_render_pose(identity(), render, result);
	return result;
}

// [orig: Vehicle_ComputeOrientationFrom4Wheels @0x459310]
CollisionMatrix four_wheel_orientation(const int32_t c[4][3]) {
	int32_t edges[4][3];
	constexpr int first[4] = { 0, 3, 2, 3 }, second[4] = { 1, 2, 1, 0 };
	for (int e = 0; e < 4; ++e) {
		for (int k = 0; k < 3; ++k)
			edges[e][k] = io::bam_sub(c[first[e]][k], c[second[e]][k]);
		normalize(edges[e]);
	}
	int32_t a[3], b[3], side[3], up[3], forward[3];
	for (int k = 0; k < 3; ++k) {
		a[k] = int32_t(double(io::bam_add(edges[0][k], edges[1][k])) * 0.5);
		b[k] = int32_t(double(io::bam_add(edges[2][k], edges[3][k])) * 0.5);
	}
	normalize(a);
	normalize(b);
	int64_t cross[3];
	q16_cross(b, a, cross);
	q16_normalize(cross, side);
	q16_cross(a, side, cross);
	q16_normalize(cross, up);
	q16_cross(side, up, cross);
	q16_normalize(cross, forward);
	CollisionMatrix matrix;
	for (int k = 0; k < 3; ++k) {
		matrix.m[4 * k] = bam_shl_wrap(forward[k], 6);
		matrix.m[4 * k + 1] = bam_shl_wrap(side[k], 6);
		matrix.m[4 * k + 2] = bam_shl_wrap(up[k], 6);
	}
	return matrix;
}
} // namespace

// The land lean servo. Its only caller supplies turnRate=0 (authored
// lean_velocity*0.01) and scaleFactor=1 (unclamped error/8192).
// [orig: Entity_SmoothHeadingToTarget @0x45B2C0 (IDB name; the bike lean —
//  its sole caller is Entity_ProcessLightVehiclePhysics @0x479600, site
//  @0x47A7B1..0x47A7D3). The boat servo is Vehicle_UpdateTurretRotation
//  @0x45AEA0 below.]
void vehicle_bike_lean(Entity &e, const VehicleTraits &t, int32_t error) {
	auto &m = e.veh;
	// The caller's gate order: crashed skips everything; +0x2EF clears BEFORE
	// the settle and airborne tests [orig: `cmp +0x2EC` @0x47A78B, `mov byte
	// ptr [esi+2EFh], 0` @0x47A79F between `cmp +0x2F0` @0x47A798 and its jnz
	// @0x47A7A6, then `test Flags, 0x2000` @0x47A7A8].
	if (m.crashed)
		return;
	m.byte_2ef = 0;
	if (m.settle_2f0 || (e.flags & kEntityFlagInAir) != 0)
		return;
	const double rate = double(float(double(t.lean_velocity) * double(0.01f)));
	constexpr double bam_per_degree = 11930465.0;
	const auto degrees = [](int32_t a) {
		const double d = double(a) * double(8.381903171539307e-8f);
		return a < 0 ? 360.0 + d : d;
	};
	const int32_t magnitude = io::bam_abs(error);
	const auto slope_rate = [&] {
		const double step = double(magnitude) * 0.0001220703125 * rate * bam_per_degree;
		return int32_t(error < 0 ? step : -step);
	};
	if (m.speed > 4096) {
		m.byte_2ef = 1;
		const double angle = degrees(m.steer_state);
		const double bend = angle > 180.0 ? 360.0 - angle : angle;
		const double delta = angle - degrees(m.view_tilt_bam);
		if (bend > 2.0) {
			const int32_t limit = int32_t(double(t.lean) * double(0.1f) * 8192.0);
			if (angle > 180.0)
				m.air_roll_rate =
						magnitude > limit && error < 0 ? 0 : int32_t(-rate * bam_per_degree);
			else
				m.air_roll_rate =
						magnitude > limit && error > 0 ? 0 : int32_t(rate * bam_per_degree);
		} else {
			// Unlike the low-speed arm, the deadband keeps the previous rate.
			if (magnitude > 256 && (e.flags & kEntityFlagInAir) == 0)
				m.air_roll_rate = slope_rate();
			m.gear_phase = 0;
			m.part_spin.angle = 0;
		}
		m.part_spin.rate = int32_t(delta * bam_per_degree);
		m.view_tilt_bam = m.steer_state;
	} else {
		if (magnitude > 256 && (e.flags & kEntityFlagInAir) == 0) {
			m.byte_2ef = 1;
			m.air_roll_rate = slope_rate();
		} else
			m.air_roll_rate = 0;
		m.gear_phase = 0;
		m.part_spin.angle = 0;
	}
}

// Planing boats retain both the lean acceleration and the error interval
// used to decelerate it. The +470 interval is deliberately a 16-bit store.
// [orig: IDB: Vehicle_UpdateTurretRotation @0x45AEA0 (a misnomer — the boat
//  lean servo; its sole caller is Entity_ProcessPlatformPhysics @0x481870,
//  site @0x4838CF)]
// Platform capsize latch and righting feed the retained chassis state.
// [orig: @0x483474..0x48348B, @0x4835DA..0x48367D]
void vehicle_boat_lean(Entity &e, const VehicleTraits &traits, int32_t error) {
	auto &m = e.veh;
	m.byte_2ef = 1;
	const auto degrees = [](int32_t a) {
		const double d = double(a) * double(8.381903171539307e-8f);
		return a < 0 ? 360.0 + d : d;
	};
	const double angle = degrees(m.steer_state);
	const double bend = std::min(angle, 360.0 - angle);
	const double delta = angle - degrees(m.view_tilt_bam);
	const int32_t magnitude = io::bam_abs(error);
	const int32_t limit = int32_t(double(traits.lean) * double(0.1f) * 8192.0);
	const double rate = double(float(double(0.1f) * traits.lean_velocity * double(0.01f)));
	const auto clear_envelope = [&] {
		m.air_roll_rate = 0;
		m.gear_phase = 0;
		m.part_spin.angle = 0;
	};
	const auto acceleration = [&] {
		if (!m.gear_phase) {
			m.gear_phase = uint16_t(io::bam_abs(io::bam_sub(limit, magnitude)));
			m.part_spin.angle = magnitude;
		}
		const double q = std::min(1.0,
				double(io::bam_abs(io::bam_sub(magnitude, io::bam_abs(m.part_spin.angle)))) /
						m.gear_phase);
		return rate - 2.0 * rate * q;
	};
	if (bend <= 2.0) {
		clear_envelope();
		if (magnitude > 160)
			m.air_roll_rate = error > 0 ? -2386239 : 2386092;
	} else if (angle < 180.0) {
		if (std::abs(delta) > double(0.001f) && double(m.part_spin.rate) * delta < 0.0)
			clear_envelope();
		if (magnitude > limit)
			m.air_roll_rate = error > 0 ? 0 : 2386092;
		else if (error <= 0)
			m.air_roll_rate = 2386092;
		else {
			m.air_roll_rate = io::bam_add(m.air_roll_rate, int32_t(acceleration() * 11930465.0));
			if (m.air_roll_rate < 0)
				m.air_roll_rate = 0;
		}
	} else {
		if (delta > 180.0)
			clear_envelope();
		if (magnitude > limit)
			m.air_roll_rate = error < 0 ? 0 : -2386092;
		else if (error >= 0)
			m.air_roll_rate = -2386092;
		else {
			m.air_roll_rate = io::bam_add(m.air_roll_rate, int32_t(acceleration() * -11930465.0));
			const double r = degrees(m.air_roll_rate);
			if (r > 0.0 && r < 180.0)
				m.air_roll_rate = 0;
		}
	}
	m.part_spin.rate = int32_t(delta * 11930465.0);
	m.view_tilt_bam = m.steer_state;
}

void vehicle_apply_lean(Entity &e, CollisionMatrix &matrix) {
	// The builder postmultiplies the existing frame; it does not replace it.
	// [orig: Math_BuildFixedPointRotationMatrixYXZ @0x615400]
	if (e.veh.byte_2ef)
		matrix = multiply(
				matrix, vehicle_euler_basis(0, e.veh.air_pitch_rate, e.veh.air_roll_rate).q22);
}

// This reset does not clear wheel springs or free-fall sinks.
// [orig: Entity_ClearSuspensionState @0x4592B0]
void vehicle_clear_chassis(Entity::VehicleMotorState &m) {
	const auto unit = identity();
	std::copy_n(unit.m, 16, m.chassis_matrix);
	std::fill_n(m.chassis_quaternion, 4, 0.0f);
	m.chassis_active = false;
	m.chassis_blend_tick = 0;
	m.chassis_blend_ticks = 0;
	m.chassis_contact_active = false;
}

// Consume and clear all four force records, retaining their rotational delta.
// [orig: Entity_ClearSuspensionForces @0x468980;
// Entity_ComputeChassisOrientation @0x463940]
static void compute_chassis_forces(Entity &e, const int32_t corners[4][3], int mode) {
	auto &m = e.veh;
	{
		int32_t moved[4][3];
		for (int i = 0; i < 4; ++i) {
			auto &force = m.chassis_forces[i];
			if (force.rate != 0)
				normalize(force.direction);
			for (int k = 0; k < 3; ++k) {
				const int32_t delta = force.rate != 0
						? int32_t(double(force.direction[k]) * (double(force.rate) / 65536.0))
						: 0;
				moved[i][k] = io::bam_add(corners[i][k], delta);
			}
		}
		CollisionMatrix fitted;
		if (mode == 1)
			fitted = four_wheel_orientation(moved);
		else {
			PlatFit ignored;
			plat_fit_corners(moved, ignored, &fitted); // @0x459A50: same four-normal aggregation
		}
		auto current = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam).q22;
		CollisionMatrix inverse;
		current.invert_into(inverse);
		CollisionMatrix retained;
		std::copy_n(m.chassis_matrix, 16, retained.m);
		retained = multiply(retained, multiply(inverse, fitted));
		std::copy_n(retained.m, 16, m.chassis_matrix);
		matrix_quaternion(retained, m.chassis_quaternion);
		m.chassis_active = true;
	}
}
void vehicle_clear_chassis_forces(Entity &e, const int32_t corners[4][3], int mode) {
	bool any = false;
	for (const auto &force : e.veh.chassis_forces)
		any |= force.rate != 0;
	if (any)
		compute_chassis_forces(e, corners, mode);
	for (auto &force : e.veh.chassis_forces)
		force = {};
}

// Apply this tick's retained matrix BEFORE advancing its identity blend.
// [orig: Entity_ApplyBoneAttachmentTransform @0x45A750]
void vehicle_apply_chassis(World &world, Entity &e, CollisionMatrix &matrix) {
	auto &m = e.veh;
	if (!m.chassis_active)
		return;
	std::fill_n(m.plat_acc, 4, 0);
	CollisionMatrix retained;
	std::copy_n(m.chassis_matrix, 16, retained.m);
	matrix = multiply(matrix, retained);
	if (m.chassis_blend_ticks > 0) {
		const float t = float(
				double(io::bam_sub(int32_t(world.logic_tick), int32_t(m.chassis_blend_tick))) /
				m.chassis_blend_ticks);
		if (t < 1.0f) {
			float q[4];
			blend_identity(m.chassis_quaternion, t, q);
			retained = quaternion_matrix(q);
			std::copy_n(retained.m, 16, m.chassis_matrix);
		} else
			vehicle_clear_chassis(m);
	}
	for (int col = 0; col < 3; ++col) {
		int32_t v[3] = { matrix.m[col] >> 6, matrix.m[4 + col] >> 6, matrix.m[8 + col] >> 6 };
		normalize(v);
		for (int row = 0; row < 3; ++row)
			matrix.m[4 * row + col] = bam_shl_wrap(v[row], 6);
	}
}
// Shared settled/airborne suspension state machine. Contact records retain
// all seven probes: the slow inverted path also reads spine slots 4 and 6.
// [orig: Entity_ProcessWheeledVehicleSuspension @0x46B140;
// Entity_ComputeSuspensionAndOrientation @0x4698A0]
bool vehicle_suspension_fit(World &world, Entity &e, int32_t corners[4][3], const bool *contacts,
		PlatFit &out, int32_t px, int32_t py, int32_t pz, bool tank) {
	auto &m = e.veh;
	CollisionMatrix matrix = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam).q22;
	bool fit = true;
	out.z_avg = out.positive_z_avg = pz;
	const bool armed = world.vehicles.suspension_arm(e, false, corners, contacts);
	if (armed) {
		vehicle_apply_chassis(world, e, matrix);
		fit = (e.flags & kEntityFlagInAir) == 0;
	} else if (m.crashed && !m.byte_2ef) {
		vehicle_apply_chassis(world, e, matrix);
		fit = false;
	} else if (m.wreck_2fc) {
		vehicle_clear_chassis(m);
		fit = tank && matrix.m[10] >= 0;
	} else if (tank && m.chassis_contact_active && !(m.crashed && m.byte_2ef)) {
		vehicle_apply_chassis(world, e, matrix);
		fit = false;
	} else if (m.crashed && m.byte_2ef) {
		if (contacts != nullptr && ((contacts[0] && contacts[2]) || (contacts[1] && contacts[3])))
			vehicle_clear_chassis(m);
		const int32_t up[3] = { matrix.m[2] >> 6, matrix.m[6] >> 6, matrix.m[10] >> 6 };
		if (m.speed < 4096 && up[2] > 0) {
			vehicle_clear_chassis(m);
			m.settle_2f0 = 1;
			m.crashed = 0;
			std::fill_n(m.plat_acc, 4, 0);
		} else {
			// Rebuild the whole authored footprint at the current attitude.
			// [orig: Entity_ComputeBoundingQuad @0x45B6E0, call @0x46B50F/@0x46B6D6]
			if (const auto *traits = world.vehicles.traits.get(e.item_id)) {
				const int32_t hx = (traits->foot_x_hi - traits->foot_x_lo) >> 1;
				const int32_t hy = (traits->foot_y_hi - traits->foot_y_lo) >> 1;
				const int32_t local[4][3] = { { hx, hy, 0 }, { hx, -hy, 0 }, { -hx, -hy, 0 },
					{ -hx, hy, 0 } };
				for (int k = 0; k < 4; ++k) {
					int32_t rotated[3];
					matrix.rotate_point(local[k], rotated);
					corners[k][0] = io::bam_add(px, rotated[0]);
					corners[k][1] = io::bam_add(py, rotated[1]);
					corners[k][2] = io::bam_add(pz, rotated[2]);
				}
			}
			vehicle_clear_chassis(m);
			const auto force = [&](int k, int32_t rate, const int32_t direction[3]) {
				auto &f = m.chassis_forces[k];
				std::copy_n(direction, 3, f.direction);
				f.rate = rate;
			};
			const int32_t down[3] = { 0, 0, -65536 };
			if (m.speed < 4096) {
				float tilt = float(double(io::bam_abs(matrix.m[8] >> 6)) / 65536.0);
				if (tilt < 0.10000000149011612)
					tilt = 0;
				const int32_t pair_rate = tank ? 8000 - int32_t(double(tilt) * -3000.0) : 8000;
				const int32_t free_rate = tank ? 8000 - int32_t(double(tilt) * -6000.0) : 8000;
				if (contacts != nullptr && contacts[4]) {
					force(2, pair_rate, down);
					force(3, pair_rate, down);
				} else if (contacts != nullptr && contacts[6]) {
					force(0, tank ? pair_rate : 800, down);
					force(1, tank ? pair_rate : 800, down);
				} else {
					for (int k = 0; k < 4; ++k)
						if (corners[k][2] > pz)
							force(k, free_rate, down);
				}
			} else {
				if (contacts != nullptr) {
					const int32_t negative_up[3] = { -up[0], -up[1], -up[2] };
					for (int k = 0; k < 4; ++k)
						force(k, contacts[k] ? 8000 : 800, contacts[k] ? negative_up : up);
				}
				vehicle_clear_chassis_forces(e, corners, 0);
				int32_t velocity[3] = { m.vel_x, m.vel_y, 0 };
				normalize(velocity);
				if (contacts != nullptr && (m.vel_x != 0 || m.vel_y != 0))
					for (int k = 0; k < 4; ++k)
						if (contacts[k])
							force(k, 8000, velocity);
			}
			vehicle_clear_chassis_forces(e, corners, 0);
			vehicle_apply_chassis(world, e, matrix);
			vehicle_clear_chassis(m);
			fit = false;
		}
	}
	if (fit) {
		plat_fit_corners(corners, out, &matrix);
		if (m.byte_2ef) {
			// [orig: Math_BuildFixedPointRotationMatrixYXZ @0x46C897..0x46C8B8]
			// The yaw-zero pitch/roll builder consumes the retained lean rates.
			vehicle_apply_lean(e, matrix);
		}
		vehicle_apply_chassis(world, e, matrix);
	}
	int32_t angles[3];
	collision_matrix_to_euler(matrix, angles);
	out.yaw_bam = angles[0];
	out.pitch_bam = angles[1];
	out.roll_bam = angles[2];
	out.fwd_z = double(matrix.m[8]) / 4194304.0;
	return fit;
}
// The boat's two buoyancy/ground calls always pass waterContact=1 and a
// null contact array. Its capsize threshold and slow inverted force differ
// from the wheeled suspension callback used by the settled/airborne leg.
// [orig: Entity_ComputeSuspensionOrientation @0x46C8E0;
// calls @0x483D00/@0x483F2B]
void vehicle_boat_suspension_fit(World &world, Entity &e, const VehicleTraits &t,
		int32_t corners[4][3], PlatFit &out, int32_t px, int32_t py, int32_t pz) {
	auto &m = e.veh;
	CollisionMatrix matrix = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam).q22;
	out.z_avg = out.positive_z_avg = pz;
	int32_t a[3], b[3], normal[3];
	int64_t edge[3], cross[3];
	for (int k = 0; k < 3; ++k)
		edge[k] = io::bam_sub(corners[0][k], corners[3][k]);
	q16_normalize(edge, a);
	for (int k = 0; k < 3; ++k)
		edge[k] = io::bam_sub(corners[3][k], corners[2][k]);
	q16_normalize(edge, b);
	q16_cross(a, b, cross);
	q16_normalize(cross, normal);
	bool fit = true;
	if (!m.settle_2f0 && io::bam_abs(normal[2]) < 36864 && !m.crashed && m.landing_2ee) {
		m.byte_2ef = 0;
		if (world.ai.is_authority)
			e.flags |= 0x10u;
		if ((e.flags & 0x10u) != 0)
			m.crashed = 1;
		if (m.crashed) {
			m.landing_2ee = 0;
			if (std::all_of(std::begin(m.plat_acc), std::end(m.plat_acc),
						[](int32_t v) { return v >= 0; })) {
				vehicle_clear_chassis(m);
				compute_chassis_forces(e, corners, 0);
				if ((e.flags & kEntityFlagInAir) != 0)
					fit = false;
			}
		}
	} else if (m.crashed && !m.byte_2ef && (e.flags & kEntityFlagInAir) != 0) {
		fit = false;
		vehicle_apply_chassis(world, e, matrix);
	} else if (m.crashed && m.byte_2ef && !(m.settle_2f0 && (e.flags & kEntityFlagInAir) == 0)) {
		fit = false;
		// The crash leg rebuilds the full CMDL footprint at the original pose.
		const int32_t hx = io::bam_sub(t.box_x_hi, t.box_x_lo) / 2;
		const int32_t hy = io::bam_sub(t.box_y_hi, t.box_y_lo) / 2;
		const int32_t xs[4] = { hx, hx, -hx, -hx }, ys[4] = { hy, -hy, -hy, hy };
		const int32_t position[3] = { px, py, pz };
		for (int i = 0; i < 4; ++i)
			for (int k = 0; k < 3; ++k)
				corners[i][k] = io::bam_add(position[k],
						io::bam_add(q16_mul_rhu(xs[i], matrix.m[4 * k] >> 6),
								q16_mul_rhu(ys[i], matrix.m[4 * k + 1] >> 6)));
		if (m.speed < 14080) {
			if (!m.wreck_2fc) {
				vehicle_clear_chassis(m);
				if ((matrix.m[10] >> 6) <= 0)
					for (int i = 0; i < 4; ++i)
						if (corners[i][2] > pz) {
							m.chassis_forces[i].direction[2] = -65536;
							m.chassis_forces[i].rate = 16000;
						}
				vehicle_clear_chassis_forces(e, corners, 0);
				vehicle_apply_chassis(world, e, matrix);
			}
		} else if (!m.settle_2f0) {
			vehicle_clear_chassis_forces(e, corners, 0);
			vehicle_apply_chassis(world, e, matrix);
		}
	}
	if (fit) {
		plat_fit_corners(corners, out, &matrix);
		vehicle_apply_lean(e, matrix);
		vehicle_apply_chassis(world, e, matrix);
	}
	int32_t angles[3];
	collision_matrix_to_euler(matrix, angles);
	out.yaw_bam = angles[0];
	out.pitch_bam = angles[1];
	out.roll_bam = angles[2];
	out.fwd_z = double(matrix.m[8]) / 4194304.0;
}
} // namespace opennova::world::detail
