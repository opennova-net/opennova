#include "vehicle_motor_detail.h"

#include <runtime/world/vehicle_system.h>
#include <runtime/world/world.h>

namespace opennova::world::detail {

// Shared seven-point suspension for both physics=0 callbacks. Its pad order,
// second-pass weighting and spring equations differ from the physics=1 solves.
// [orig: Entity_ProcessVehicleSuspension @0x463C60]
void vehicle_simple_contact(
		World &world, Entity &e, const VehicleTraits &t, int32_t &x, int32_t &y, int32_t &z) {
	auto &m = e.veh;
	const int32_t radius = io::bam_sub(t.box_y_hi, t.box_y_lo) >> 2;
	if (radius <= 0 || t.box_z_hi == t.box_z_lo) {
		// A missing graphic makes the retail callback return immediately.
		return;
	}
	if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 && m.wheel_rate_bam == 0 &&
			m.air_roll_rate == 0 && m.air_pitch_rate == 0 &&
			((e.flags | e.engine_flags) & (kEntityFlagInAir | 0x40u)) == 0 && m.slide_z > -500 &&
			m.slide_z < 0) {
		z = io::bam_sub(z, m.slide_z);
		m.slide_z >>= 1;
		return;
	}
	e.flags &= ~0x40u;
	e.engine_flags &= ~0x40u;
	const int32_t pad_z = io::bam_add(t.box_z_lo, radius);
	const int32_t front = io::bam_sub(t.foot_x_hi, radius);
	const int32_t rear = io::bam_add(t.foot_x_lo, radius);
	const int32_t left = io::bam_sub(t.foot_y_hi, radius);
	const int32_t right = io::bam_add(t.foot_y_lo, radius);
	const int32_t length = io::bam_sub(t.box_x_hi, t.box_x_lo);
	const int32_t mid_y = io::bam_add(t.box_y_lo, io::bam_sub(t.box_y_hi, t.box_y_lo) >> 1);
	const int32_t spine_radius = std::max(8192,
			std::min((io::bam_sub(t.box_z_hi, t.box_z_lo) >> 1) - 16384,
					(io::bam_sub(t.box_y_hi, t.box_y_lo) >> 1) - 4096));
	const int32_t spine_z = io::bam_sub(t.box_z_hi, spine_radius);
	const int32_t model[7][3] = {
		{ front, left, pad_z },
		{ front, right, pad_z },
		{ rear, left, pad_z },
		{ rear, right, pad_z },
		{ io::bam_add(t.box_x_lo, length >> 2), mid_y, spine_z },
		{ io::bam_add(t.box_x_lo, bam_mul_wrap(length, 3) >> 2), mid_y, spine_z },
		{ io::bam_add(t.box_x_lo, length >> 1), mid_y, spine_z },
	};
	const int32_t radii[7] = { radius, radius, radius, radius, spine_radius, spine_radius,
		spine_radius };
	int32_t points[7][3];
	const auto basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	place_probes(basis, model, x, y, z, points);
	for (int i = 0; i < 4; ++i)
		points[i][2] = io::bam_add(points[i][2], m.wheel_comp[i]);
	PlatProbeForce first[7], second[7];
	const int32_t soft = cos22_of_bam_x87(t.max_slope), hard = cos22_of_bam_x87(t.slip_slope);
	EntityHandle hit;
	const int severity = plat_probe_pass(world, e, points, radii, soft, hard, first, x, y, z, &hit);
	vehicle_contact_impact(world, e, t, severity, hit, x, y, z);
	if (severity != 0)
		m.speed = io::bam_sub(m.speed, m.speed >> ((t.torque + (severity == 2 ? 1 : 2)) & 31));
	if (severity == 3 && strongest_probe_beyond_hull(first, points, 7, x, y)) {
		int best = 0;
		double magnitude = -1;
		for (int i = 0; i < 7; ++i) {
			const double squared =
					double(first[i].fx) * first[i].fx + double(first[i].fy) * first[i].fy;
			if (squared > magnitude) {
				magnitude = squared;
				best = i;
			}
		}
		const int32_t dx = io::bam_sub(points[best][0], x), dy = io::bam_sub(points[best][1], y);
		// This contact uses the NEGATIVE atan scale dbl_7C57B8.
		int32_t impulse = io::bam_sub(
				bam_of_atan2(io::bam_add(dy, first[best].fy), io::bam_add(dx, first[best].fx)),
				bam_of_atan2(dy, dx));
		const Entity *other = world.registry.get(hit);
		const VehicleTraits *ot =
				other != nullptr ? world.vehicles.traits.get(other->item_id) : nullptr;
		if (ot != nullptr && ot->player_control && ot->mass < int64_t(t.mass) * 2 &&
				other->bound_radius < e.bound_radius * 2 && int64_t(ot->mass) + t.mass != 0)
			impulse = int32_t(int64_t(impulse) * ot->mass / (int64_t(ot->mass) + t.mass));
		m.wheel_rate_bam = io::bam_add(m.wheel_rate_bam, impulse >> 2);
	}
	int32_t depth[7], push_x = 0, push_y = 0;
	for (int i = 0; i < 7; ++i) {
		depth[i] = first[i].fz;
		push_x = io::bam_add(push_x, first[i].fx);
		push_y = io::bam_add(push_y, first[i].fy);
	}
	for (auto &point : points) {
		point[0] = io::bam_add(point[0], push_x);
		point[1] = io::bam_add(point[1], push_y);
	}
	if (plat_probe_pass(world, e, points, radii, soft, hard, second, x, y, z) != 0) {
		int32_t second_x = 0, second_y = 0;
		for (int i = 0; i < 7; ++i) {
			second_x = io::bam_add(second_x, second[i].fx);
			second_y = io::bam_add(second_y, second[i].fy);
			// Keep the old depth plus HALF of old+new, not their average.
			depth[i] = io::bam_add(depth[i], io::bam_add(depth[i], second[i].fz) >> 1);
		}
		push_x = io::bam_add(push_x, second_x >> 1);
		push_y = io::bam_add(push_y, second_y >> 1);
	}
	vehicle_contact_mass_share(world, e, hit, push_x, push_y, depth, 7);
	x = io::bam_add(x, push_x);
	y = io::bam_add(y, push_y);
	// The dispatcher enables water support even when the plane is at zero.
	// [orig: Entity_ProcessVehicleSuspension @0x463C60]
	if (t.amphibian || t.family == VehicleFamily::Watercraft)
		for (int i = 0; i < 4; ++i)
			depth[i] = std::max(
					depth[i], io::bam_add(io::bam_sub(world.env.water_z, points[i][2]), pad_z));
	// This callback releases only W2/W3, including its boat caller.
	VehicleTraits transition = t;
	transition.family = VehicleFamily::Ground;
	plat_water_flag(world, e, transition, points, radius, io::bam_sub(0, pad_z), world.env.water_z);
	m.plat_afloat = (e.flags & 0x8000u) != 0;
	m.plat_solve_valid = true;
	for (int i = 0; i < 4; ++i) {
		m.plat_acc[i] = bam_mul_wrap(std::max(m.plat_acc[i], m.wheel_comp[i]), t.shock) >> 3;
		m.wheel_comp[i] = std::max(0, io::bam_sub(m.wheel_comp[i], m.plat_acc[i]));
	}
	if (depth[0] == 0 && depth[1] == 0 && depth[2] == 0 && depth[3] == 0) {
		e.flags |= kEntityFlagInAir;
		m.grounded = false;
		++m.plat_airborne_ticks;
		return;
	}
	e.flags &= ~kEntityFlagInAir;
	m.grounded = true;
	m.plat_airborne_ticks = 0;
	const bool diagonal = (depth[2] != 0 && depth[1] != 0) || (depth[3] != 0 && depth[0] != 0);
	if (diagonal) {
		m.air_roll_rate >>= 1;
		m.air_pitch_rate >>= 1;
		if (m.slide_z < 0)
			m.slide_z = io::bam_sub(m.slide_z, m.slide_z >> 2);
	}
	const int32_t travel = radius >> 2;
	for (int i = 0; i < 4; ++i)
		if (travel != 0 && depth[i] > (travel >> 6)) {
			const int32_t limit = std::min(depth[i], io::bam_sub(travel, m.wheel_comp[i]));
			const int32_t force = bam_mul_wrap(limit, t.spring) >> 3;
			const int32_t fraction =
					io::bam_sub(256, bam_shl_wrap(io::bam_add(force, m.wheel_comp[i]), 6) / travel);
			const int32_t transfer = bam_mul_wrap(force, fraction) >> 8;
			depth[i] = io::bam_sub(depth[i], transfer);
			m.wheel_comp[i] = io::bam_add(m.wheel_comp[i], transfer);
		}
	const bool supported = (depth[2] != 0 && depth[1] != 0) || (depth[3] != 0 && depth[0] != 0);
	int32_t minimum = 655360, damping = 0;
	for (int i = 0; i < 4; ++i) {
		minimum = std::min(minimum, depth[i]);
		if (supported && depth[i] != 0)
			damping = io::bam_add(damping, m.plat_acc[i]);
	}
	damping = t.spring != 0 ? damping >> 4 : 0;
	for (int i = 0; i < 4; ++i) {
		depth[i] = io::bam_sub(depth[i], minimum);
		if (depth[i] < 50)
			depth[i] = 0;
	}
	const int32_t roll_span = io::bam_sub(left, right), pitch_span = io::bam_sub(front, rear);
	auto fit = [&](int32_t angle, int32_t &rate, int32_t a, int32_t b, int32_t low, int32_t high,
					   int32_t span, bool pitch, int32_t &correction) {
		if (io::bam_abs(angle) >= 954437120u) {
			if (io::bam_abs(rate) < 17318340u)
				rate = io::bam_sub(rate, io::bam_sar(io::bam_add(angle, 16), 5));
			correction = 0;
			return int32_t(0);
		}
		rate = io::bam_sub(io::bam_sub(rate, io::bam_sar(io::bam_add(rate, 8), 4)), rate >> 31);
		correction = bam_of_atan2(io::bam_sub(a, b), a <= b ? io::bam_sub(0, low) : high);
		if (span == 0)
			return minimum;
		const int32_t distance = int32_t(io::bam_abs(a <= b ? low : high));
		// The <= pitch arm uses front depth, not the front/back difference.
		const int32_t delta = a <= b ? (pitch ? a : io::bam_sub(b, a)) : io::bam_sub(a, b);
		return io::bam_add(
				io::bam_add(minimum, std::min(a, b)), int32_t(int64_t(distance) * delta / span));
	};
	int32_t roll_correction = 0, pitch_correction = 0;
	const int32_t roll_lift = fit(m.air_roll_bam, m.air_roll_rate,
			io::bam_add(io::bam_add(depth[0], depth[2]), 1) >> 1,
			io::bam_add(io::bam_add(depth[1], depth[3]), 1) >> 1, right, left, roll_span, false,
			roll_correction);
	const int32_t pitch_lift = fit(m.air_pitch_bam, m.air_pitch_rate,
			io::bam_add(io::bam_add(depth[0], depth[1]), 1) >> 1,
			io::bam_add(io::bam_add(depth[2], depth[3]), 1) >> 1, rear, front, pitch_span, true,
			pitch_correction);
	int32_t lift =
			int32_t(std::min(2147418112.0, std::hypot(double(roll_lift), double(pitch_lift))));
	if (roll_lift <= pitch_lift)
		roll_correction >>= 1;
	else
		pitch_correction >>= 1;
	for (int i = 4; i < 7; ++i)
		lift = std::max(lift, depth[i]);
	m.air_roll_rate = io::bam_add(m.air_roll_rate, roll_correction >> 1);
	m.air_roll_bam = io::bam_add(m.air_roll_bam, roll_correction >> 1);
	m.air_pitch_rate = io::bam_add(m.air_pitch_rate, pitch_correction >> 1);
	m.air_pitch_bam = io::bam_add(m.air_pitch_bam, pitch_correction >> 1);
	if (damping > 0)
		m.slide_z = io::bam_add(m.slide_z, damping);
	z = io::bam_add(z, lift);
}
} // namespace opennova::world::detail
