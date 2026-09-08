#include "vehicle_motor_detail.h"
#include "world.h"

namespace opennova::world {
namespace {

int32_t dot16(const int32_t a[3], const int32_t b[3]) {
	return io::bam_add(
			io::bam_add(detail::q16_mul_rhu(a[0], b[0]), detail::q16_mul_rhu(a[1], b[1])),
			detail::q16_mul_rhu(a[2], b[2]));
}
void normalize16(const int32_t in[3], int32_t out[3]) {
	const double length =
			std::sqrt(double(in[0]) * in[0] + double(in[1]) * in[1] + double(in[2]) * in[2]);
	if (length == 0.0) {
		std::fill_n(out, 3, 0);
		return;
	}
	const double scale = 65536.0 / length;
	for (int k = 0; k < 3; ++k)
		out[k] = int32_t(double(in[k]) * scale);
}

// This footprint uses the full CMDL bounds. Each axis product rounds before
// the two products are summed; negative odd extents truncate toward zero.
// [orig: Entity_ComputeBoundingQuad @0x45B6E0, non-square arm]
void impulse_quad(const Entity &e, const VehicleTraits &t, const CollisionMatrix &frame,
		int32_t corners[4][3], int32_t directions[4][3]) {
	const int32_t x = io::bam_abs(io::bam_sub(t.box_x_hi, t.box_x_lo)) / 2;
	const int32_t y = io::bam_abs(io::bam_sub(t.box_y_hi, t.box_y_lo)) / 2;
	const int32_t xs[4] = { x, x, -x, -x }, ys[4] = { y, -y, -y, y };
	const int32_t position[3] = { to_fixed(e.position.x), to_fixed(e.position.y),
		to_fixed(e.position.z) };
	for (int i = 0; i < 4; ++i) {
		int32_t delta[3];
		for (int k = 0; k < 3; ++k) {
			delta[k] = io::bam_add(detail::q16_mul_rhu(xs[i], frame.m[4 * k] >> 6),
					detail::q16_mul_rhu(ys[i], frame.m[4 * k + 1] >> 6));
			corners[i][k] = io::bam_add(position[k], delta[k]);
		}
		normalize16(delta, directions[i]);
	}
}
} // namespace

// The recoil force pair follows the strongest positive forward/right-facing
// dot. Longitudinal shots scale by the model's width/length ratio. The fourth
// retail argument is an unused fire-pose scratch buffer.
// [orig: Entity_InitVehicleSuspensionGeometry @0x474580]
void detail::vehicle_recoil_impulse(World &world, Entity &e, const VehicleTraits &t,
		int32_t amplitude, const int32_t direction[3]) {
	if (amplitude == 0)
		return;
	auto &m = e.veh;
	const auto frame = entity_placement_matrix(e);
	int32_t corners[4][3], radial[4][3];
	impulse_quad(e, t, frame, corners, radial);
	int32_t axes[4][3];
	for (int k = 0; k < 3; ++k) {
		axes[0][k] = frame.m[4 * k] >> 6;
		axes[1][k] = io::bam_sub(0, frame.m[4 * k + 1] >> 6);
		axes[2][k] = io::bam_sub(0, axes[0][k]);
		axes[3][k] = frame.m[4 * k + 1] >> 6;
	}
	int quadrant = -1;
	int32_t best = -1;
	for (int i = 0; i < 4; ++i) {
		const int32_t dot = dot16(direction, axes[i]);
		if (dot > 0 && dot > best) {
			best = dot;
			quadrant = i;
		}
	}
	m.chassis_impulse_amplitude = amplitude;
	if (quadrant >= 0) {
		const int32_t length = io::bam_abs(io::bam_sub(t.box_x_hi, t.box_x_lo));
		const int32_t width = io::bam_abs(io::bam_sub(t.box_y_hi, t.box_y_lo));
		const double ratio = (quadrant & 1) != 0 ? 1.0 : length != 0 ? double(width) / length : 0.0;
		const int32_t scale = bam_mul_wrap(300, amplitude);
		for (int i = 0; i < 2; ++i) {
			const int corner = (quadrant + i) & 3;
			auto &force = m.chassis_forces[corner];
			force.rate =
					int32_t((double(dot16(direction, radial[corner])) / 65536.0) * ratio * scale);
			for (int k = 0; k < 3; ++k)
				force.direction[k] = io::bam_sub(0, frame.m[4 * k + 2] >> 6);
		}
	}
	vehicle_clear_chassis(m);
	vehicle_clear_chassis_forces(e, corners, 0);
	m.chassis_blend_tick = world.logic_tick;
	m.chassis_blend_ticks = 40;
	m.chassis_contact_active = true;
	normalize16(direction, m.chassis_impulse_direction);
}

// A heavy projectile rocks a wheeled/tank hull at the hit side. The force
// records intentionally retain the original second-pass index asymmetry.
// [orig: Entity_SetupInfantryForcePoints @0x475220]
static void impact_impulse(World &world, Entity &e, const VehicleTraits &t,
		const int32_t direction[3], const int32_t hit[3]) {
	auto &m = e.veh;
	const auto frame = entity_placement_matrix(e);
	int32_t corners[4][3], radial[4][3];
	impulse_quad(e, t, frame, corners, radial);
	const int32_t offset[3] = { io::bam_sub(hit[0], to_fixed(e.position.x)),
		io::bam_sub(hit[1], to_fixed(e.position.y)), 0 };
	int32_t toward[3];
	normalize16(offset, toward);
	int quadrant = -1;
	int32_t best = -1;
	for (int i = 0; i < 4; ++i) {
		const int32_t dot = dot16(toward, radial[i]);
		if (dot > 0 && dot > best) {
			best = dot;
			quadrant = i;
		}
	}
	const int32_t side[3] = { frame.m[1] >> 6, frame.m[5] >> 6, frame.m[9] >> 6 };
	const int32_t threshold = dot16(side, radial[0]);
	int32_t side_dot = dot16(side, toward);
	int pair = -1;
	switch (quadrant) {
		case 0:
			pair = side_dot < threshold ? 0 : 3;
			break;
		case 1:
			side_dot = io::bam_sub(0, side_dot);
			pair = side_dot >= threshold ? 1 : 0;
			break;
		case 2:
			side_dot = io::bam_sub(0, side_dot);
			pair = side_dot < threshold ? 2 : 1;
			break;
		case 3:
			pair = side_dot >= threshold ? 3 : 2;
			break;
	}
	detail::vehicle_clear_chassis(m);
	constexpr int32_t amplitude = 17;
	constexpr int32_t force = 650 * amplitude;
	if (pair >= 0) {
		for (int i = 0; i < 2; ++i) {
			auto &f = m.chassis_forces[(pair + i) & 3];
			f.direction[2] = 65536;
			f.rate = force / 2;
		}
		detail::vehicle_clear_chassis_forces(e, corners, 0);
		if ((pair & 1) != 0) {
			const int32_t rate = int32_t(double(force) * (double(side_dot) / 65536.0) * 0.5);
			const bool first = pair == 1 ? quadrant == 1 : quadrant == 0;
			m.chassis_forces[first ? 0 : 2].rate = rate;
			m.chassis_forces[first ? 1 : 3].rate = rate;
			// Retail writes these two direction records for both side pairs.
			std::copy_n(direction, 3, m.chassis_forces[1].direction);
			std::copy_n(direction, 3, m.chassis_forces[2].direction);
			detail::vehicle_clear_chassis_forces(e, corners, 0);
		}
	}
	m.chassis_blend_tick = world.logic_tick;
	m.chassis_blend_ticks = 40;
	m.chassis_contact_active = true;
	m.chassis_impulse_amplitude = amplitude;
	normalize16(direction, m.chassis_impulse_direction);
}

// Projectile+680 is the wrapped ammo weight converted to Q16 / 250, not
// the health damage. This reaction runs on both authority and predicting peers.
// [orig: Projectile_HandleEntityImpact @0x4E9390 -> sub_43C0C0 @0x43C0C0;
// RoundData_SpawnRound @0x4EC69B..0x4EC6B7]
void VehicleSystem::projectile_impact(
		Entity &target, int32_t weight, const int32_t normal[3], const int32_t hit[3]) {
	const auto *t = traits.get(target.item_id);
	if (t == nullptr || !target.has_item_def || target.item_type != 1 ||
			((target.flags | target.engine_flags) & 4u) != 0 ||
			(t->unit_type != 1 && t->unit_type != 2))
		return;
	const int32_t strength = int32_t(uint32_t(weight) << 16) / 250;
	if (strength <= 0x500000)
		return;
	target.flags |= 0x40u;
	target.engine_flags |= 0x40u;
	target.veh.contact_wake_tick = world_.logic_tick;
	impact_impulse(world_, target, *t, normal, hit);
}

// A mounted gun's ground link identifies its carrier. Only CTANK executes
// recoil, and a zero action value leaves an existing impulse untouched.
// [orig: WeaponAction_Fire @0x542B10, tail @0x542D1D..0x542DA1;
// ActionSlot_ExecuteAction @0x4020A0, tail @0x402196..0x402233]
void VehicleSystem::weapon_recoil(
		const Entity &shooter, int32_t amplitude, int32_t yaw, int32_t pitch) {
	if (amplitude == 0 || !shooter.mounted || shooter.mount_type != SeatType::Gunner)
		return;
	const Entity *gun = world_.registry.get(shooter.mount_target);
	if (gun == nullptr)
		return;
	Entity *carrier = world_.registry.get(gun->ground_target);
	if (carrier == nullptr || !carrier->has_item_def || carrier->item_type != 1)
		return;
	const VehicleTraits *t = traits.get(carrier->item_id);
	if (t == nullptr || t->family != VehicleFamily::Tank)
		return;
	const auto frame = detail::vehicle_euler_basis(yaw, pitch, 0).q22;
	const int32_t direction[3] = { io::bam_sub(0, frame.m[0] >> 6), io::bam_sub(0, frame.m[4] >> 6),
		io::bam_sub(0, frame.m[8] >> 6) };
	detail::vehicle_recoil_impulse(world_, *carrier, *t, amplitude, direction);
}
} // namespace opennova::world
