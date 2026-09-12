#include <runtime/world/vehicle_system.h>
#include <runtime/world/vehicle_motor.h>
#include <base/io/fixed.h>

// Split out of vehicle_motor.cpp (the oversize-TU ratchet). Motion only — every
// body is unchanged, and each original-code citation moved with the code it
// annotates.
//
// The air + ground contact/suspension solves — the client-executed subsets of
// Entity_ProcessAircraftContactPhysics [orig: @0x47EF10] and
// Entity_ProcessTrackedVehiclePhysics [orig: @0x47C1C0] — consumed by the
// air/ground movers in vehicle_motor.cpp at their witnessed call sites.

#include <algorithm>
#include <cmath>

#include "vehicle_motor_detail.h"

#include <runtime/world/angle.h>
#include <runtime/world/vehicle_suspension.h>
#include <runtime/world/world.h>

namespace opennova::world {
namespace detail {

// Aircraft ground/water contact, suspension and wreck solve. Shared contact forces include terrain
// and posed models; authority damage and client prediction keep their original gates. The solve
// runs after position integration and before attitude-rate integration. Model-less rows use the
// caller terrain clamp. See vehicle-client-movers-re.md sections 6 and 12-32.
// Witness sites: [orig: @0x47EF10, @0x49254E]
bool air_contact_solve_active(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           traits.box_y_hi != traits.box_y_lo &&
           ((traits.box_y_hi - traits.box_y_lo) >> 2) > 0;
}

void aircraft_contact_solve(World &world, Entity &veh, const VehicleTraits &traits, Entity::VehicleMotorState &m, int32_t start_x, int32_t start_y, int32_t &px, int32_t &py, int32_t &pz) {
    // Boxless/degenerate rows keep the terrain-clamp stand-in (lib embedders /
    // unresolved graphics); the caller derives the branch picks locally for
    // exactly the same rows.
    if (!air_contact_solve_active(traits)) {
        if (m.ground_cache != INT32_MIN && pz < m.ground_cache) {
            pz = m.ground_cache;
            if (m.slide_z < 0) m.slide_z = 0;
        }
        return;
    }

	// An at-rest hull undoes the vertical dribble and halves slide_z. Occupancy, spring energy and
	// the parked/wreck latches preserve the original sleep gates.
	// Witness sites: [orig: @0x47EF20, @0x47F189]
	if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 && m.wheel_rate_bam == 0 &&
			m.air_pitch_rate == 0 && m.air_roll_rate == 0 && (veh.flags & kEntityFlagInAir) == 0 &&
			((veh.flags | veh.engine_flags) & 0x40u) == 0 && m.slide_z > -350 && m.slide_z < 0 &&
			px == start_x && py == start_y && !veh.primary_occupant.valid() &&
			m.spring_energy == 0 &&
			std::all_of(
					std::begin(m.plat_acc), std::end(m.plat_acc),
					[](int32_t value) { return value == 0; })) {
		pz -= m.slide_z; // [orig: @0x47F059]
		m.slide_z >>= 1;      // [orig: @0x47F067]
		vehicle_rest_state(world, veh, traits.family);
		return;
	}

	vehicle_expire_contact_wake(world, veh);

	// Four gear-pad probes use the footprint corners inset by their radius, followed by three
	// spine probes. Current spring compression offsets the pads.
	// Witness sites: [orig: @0x47F4D1, @0x47F514, @0x47F683, @0x47F6F5, @0x47F807]
	const int32_t r = (traits.box_y_hi - traits.box_y_lo) >> 2;
	int32_t rs = std::min(((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000,
                          ((traits.box_y_hi - traits.box_y_lo) >> 1) - 0x1000);
    if (rs < 0x2000) rs = 0x2000;
    const int32_t L = traits.box_x_hi - traits.box_x_lo;
    const int32_t ymid =
            traits.box_y_lo + ((traits.box_y_hi - traits.box_y_lo) >> 1);
    const int32_t spine_z = traits.box_z_hi - rs;
    const int32_t pad_z = traits.box_z_lo + r;
    const int32_t probes_model[7][3] = {
        {traits.foot_x_hi - r, traits.foot_y_hi - r, pad_z}, // pad0 (+fwd,+side)
        {traits.foot_x_hi - r, traits.foot_y_lo + r, pad_z}, // pad1 (+fwd,-side)
        {traits.foot_x_lo + r, traits.foot_y_lo + r, pad_z}, // pad2 (-fwd,-side)
        {traits.foot_x_lo + r, traits.foot_y_hi - r, pad_z}, // pad3 (-fwd,+side)
        {traits.box_x_lo + (L >> 2), ymid, spine_z},         // pt4
        {traits.box_x_lo + ((3 * L) >> 2), ymid, spine_z},   // pt5
        {traits.box_x_lo + (L >> 1), ymid, spine_z},         // pt6
    };
    const int32_t radii[7] = {r, r, r, r, rs, rs, rs};
    const int32_t hull_bottom_neg = -(traits.box_z_lo + r); // §6.3 'output_matrix'
    const int32_t half_w =
            (traits.foot_y_hi - r) - (traits.foot_y_lo + r);
    const int32_t half_h =
            (traits.foot_x_hi - r) - (traits.foot_x_lo + r);

	VehicleEulerBasis basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	int32_t probes[7][3];
	place_probes(basis, probes_model, px, py, pz, probes);

    // ---- §6.4 the two force passes + severity (terrain leg; the shared
    // sub-contract with the platform solve) [orig: @0x47F855..0x480150].
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
	EntityHandle hit_entity;
	const int32_t sev =
			plat_probe_pass(world, veh, probes, radii, soft, hard, forces, px, py, pz, &hit_entity);
	vehicle_contact_impact(world, veh, traits, sev, hit_entity, px, py, pz);
	if (sev == 1) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x47F905..]
	} else if (sev == 2) {
		m.speed -= m.speed >> ((traits.torque + 1) & 31);
	} else if (sev == 3) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31);
		// The shared impact fold applies authority damage, sound and momentum. A terrain-only
		// severe hit cuts speed to one quarter when its strongest planar probe lies beyond the
		// hull.
		if (!hit_entity.valid() && strongest_probe_beyond_hull(forces, probes, 7, px, py))
			m.speed = int32_t(m.speed * 0.25); // [orig: @0x47FDF8 region]
	}
	int32_t d[7];
    for (int i = 0; i < 7; ++i) d[i] = forces[i].fz;
    if (sev >= 1) {
        // plat_second_pass: the planar separation that keeps a remote
        // aircraft out of hillsides [orig: @0x480143..].
		plat_second_pass(world, veh, probes, radii, soft, hard, forces, d, px, py, pz, hit_entity);
	}

	// Water entry uses r/2 hysteresis, releases the dry trail bank and emits the local splash and
	// positioned sound edge.
	// Witness sites: [orig: @0x48021A, @0x48033D]
	plat_water_flag(world, veh, traits, probes, r, hull_bottom_neg, world.env.water_z);

	vehicle_landing_damage(world, veh, traits, d, 7, basis.q22.m[10] >> 6);
	vehicle_crush_damage(world, veh, traits, d + 4, basis.q22.m[10] >> 6);
	const bool contacts[7] = { d[0] != 0, d[1] != 0, d[2] != 0, d[3] != 0, d[4] != 0, d[5] != 0,
		d[6] != 0 };
	vehicle_crash_state(world, veh, traits, contacts, basis.q22.m[10] >> 6);
	basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);

	// Save the pre-spring penetration. Upright live hulls leave the -1 sentinel.
	// [orig: Entity_ProcessAircraftContactPhysics @ 0x47EF10]
	int32_t crash_depth = -1;
	if (m.crashed || basis.q22.m[10] < 0 || m.settle_2f0)
		for (int i = 0; i < 7; ++i) crash_depth = std::max(crash_depth, d[i]);

	// ---- §6.10 branch select: any PAD depth = grounded.
	const bool pad_contact = d[0] > 0 || d[1] > 0 || d[2] > 0 || d[3] > 0;
    if (!pad_contact) {
		// Airborne marks the flag; the park and wreck latches decide whether to retain the
		// vertical pose.
		// Witness sites: [orig: @0x480ED5]
		// [orig: Entity_ProcessAircraftContactPhysics @ 0x47EF10]
		if ((m.crashed && m.byte_2ef) || m.settle_2f0) {
			int32_t depth = -1;
			if (m.crashed || basis.q22.m[10] < 0)
				for (int i = 0; i < 7; ++i) depth = std::max(depth, d[i]);
			pz = io::bam_add(pz, depth);
			veh.flags &= ~kEntityFlagInAir;
		} else veh.flags |= kEntityFlagInAir;
		int32_t corners[4][3];
		const int32_t hx = half_h >> 1, hy = half_w >> 1;
		const int32_t local[4][3] = { { hx, -hy, 0 }, { hx, hy, 0 }, { -hx, -hy, 0 },
			{ -hx, hy, 0 } };
		for (int k = 0; k < 4; ++k) {
			int32_t rotated[3];
			basis.q22.rotate_point(local[k], rotated);
			corners[k][0] = io::bam_add(px, rotated[0]);
			corners[k][1] = io::bam_add(py, rotated[1]);
			corners[k][2] = io::bam_add(pz, rotated[2]);
		}
		PlatFit fit;
		vehicle_suspension_fit(world, veh, corners, nullptr, fit, px, py, pz);
		m.air_pitch_bam = fit.pitch_bam;
		m.air_roll_bam = fit.roll_bam;
		if (m.crashed)
			m.yaw_bam = fit.yaw_bam;
		return;
	}
    // §6.12 GROUNDED: the flag clears unconditionally [orig: @0x4810EB].
    veh.flags &= ~kEntityFlagInAir;
	// Fit the pad rectangle through the Q22 chassis basis and each resolved spring lift.
	// Witness sites: [orig: @0x4805B4, @0x481425, @0x481427]
	int32_t c[4][3];
	{
        const int32_t hw2 = half_w >> 1, hh2 = half_h >> 1;
        const int32_t corner_model[4][3] = {
            {+hh2, -hw2, 0}, {+hh2, +hw2, 0}, {-hh2, -hw2, 0}, {-hh2, +hw2, 0},
        };
        for (int k = 0; k < 4; ++k) {
            int32_t rotated[3];
            basis.q22.rotate_point(corner_model[k], rotated);
            c[k][0] = px + rotated[0];
            c[k][1] = py + rotated[1];
            c[k][2] = pz + rotated[2] + d[k];
        }
    }
    PlatFit fit;
	vehicle_suspension_fit(world, veh, c, nullptr, fit, px, py, pz);
	// Conform writes pitch and roll; the parked arm also adopts yaw. The mover integrates the
	// rates after return.
	// Witness sites: [orig: @0x48148A]
	m.air_pitch_bam = fit.pitch_bam;
	m.air_roll_bam = fit.roll_bam;
	if (m.crashed != 0)
		m.yaw_bam = fit.yaw_bam;
	if (m.crashed == 0 && m.settle_2f0 == 0 && basis.up[2] > 0.0) {
		// Upright non-parked: slideDecay clamped toward the ground, then
        // Z = the solver chassis Z [orig: @0x481834..0x481849] — the wheeled
        // solver's form: the average of the ABOVE-ZERO corners only,
        // rise-clamped +0x2000/tick (blockers §3.3 @0x46C822..0x46C894).
        if (!m.settle_2f0 && m.slide_z > 0) m.slide_z = 0;
        int32_t new_z = fit.positive_z_avg;
        if (new_z > pz + 0x2000) new_z = pz + 0x2000;
        pz = new_z;
	} else {
		// Inverted: lift by the max penetration [orig: @0x4814C0..0x4814C4].
		pz = io::bam_add(pz, crash_depth);
	}
	const int32_t up_z16 = basis.q22.m[10] >> 6;
	// [orig: @0x4814CE..0x4817EA] contact sink clear, then replicated
	// crash recovery. Aircraft use the opposite diagonal indexing.
	for (int k = 0; k < 4; ++k)
		if (d[k] != 0)
			m.plat_acc[k] = 0;
	if (((d[0] && d[3]) || (d[1] && d[2])) && up_z16 > 0 && !m.crashed) {
		m.landing_2ee = 0;
		m.byte_2ef = 0;
		std::fill_n(m.plat_acc, 4, 0);
	}
	// Replicated parked state seeds the small opposite-corner up forces.
	// [orig: Entity_ProcessAircraftContactPhysics @0x47EF10 (site @0x4816DD)]
	if (((veh.flags | veh.engine_flags) & 0x10u) != 0 && !m.crashed && !m.settle_2f0) {
		int32_t quad[4][3];
		const int32_t hx = (traits.box_x_hi - traits.box_x_lo) >> 1;
		const int32_t hy = (traits.box_y_hi - traits.box_y_lo) >> 1;
		const int32_t local[4][3] = { { hx, hy, 0 }, { hx, -hy, 0 }, { -hx, -hy, 0 },
			{ -hx, hy, 0 } };
		for (int k = 0; k < 4; ++k) {
			basis.q22.rotate_point(local[k], quad[k]);
			quad[k][0] = io::bam_add(quad[k][0], px);
			quad[k][1] = io::bam_add(quad[k][1], py);
			quad[k][2] = io::bam_add(quad[k][2], pz);
		}
		for (int k : { 0, 3 }) {
			for (int axis = 0; axis < 3; ++axis)
				m.chassis_forces[k].direction[axis] = basis.q22.m[8 + axis] >> 6;
			m.chassis_forces[k].rate = 10;
		}
		vehicle_clear_chassis_forces(veh, quad, 0);
		vehicle_apply_chassis(world, veh, basis.q22);
		m.crashed = 1;
	}
	if (((veh.flags | veh.engine_flags) & 0x10u) == 0 &&
			((m.crashed && up_z16 < 8192) || m.settle_2f0)) {
		vehicle_rebuild_rest_orientation(world, veh, false);
		m.crashed = 0;
		m.settle_2f0 = 0;
	}
	vehicle_suspension_tick_tail(veh, traits);
	m.contact_solved_once = true;
}

// Ground/tracked contact and suspension for cveh/ctrn/catv. The four pads plus three spine probes
// preserve wheel clearance, model contact, mass exchange, spring feedback, contact direction and
// crash/park state. The caller runs this after position integration and before grounded yaw. See
// vehicle-client-movers-re.md sections 7 and 12-32.
// Witness sites: [orig: @0x47C1C0, @0x48d0b1, @0x48d0d4, @0x47CD00, @0x47CDFB, @0x47D96B,
// @0x47DA2B, @0x47DBFF, @0x47DC50, @0x47DDF4, @0x47DB59, @0x47DBE4, @0x47E960, @0x47EC1F,
// @0x47CE7C, @0x47D097, @0x47D336, @0x47D727, @0x47E7A3, @0x47ED73, @0x47EEAC, @0x47E65D,
// @0x47E78F, @0x48cf97, @0x48d003, @0x47EEC7, @0x47C49F]
bool ground_contact_solve_active(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           traits.box_y_hi != traits.box_y_lo &&
           ((traits.box_y_hi - traits.box_y_lo) >> 2) > 0;
}

void ground_contact_solve(World &world, Entity &veh, const VehicleTraits &traits, Entity::VehicleMotorState &m, int32_t start_x, int32_t start_y, int32_t &px, int32_t &py, int32_t &pz) {
	// Sleep requires the family velocity, occupancy, spring-energy and wreck-latch gates, then
	// restores the vertical dribble.
	// Witness sites: [orig: @0x47C244, @0x47C44B, @0x47C302]
	if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 && m.wheel_rate_bam == 0 &&
			m.air_pitch_rate == 0 && m.air_roll_rate == 0 &&
			((veh.flags | veh.engine_flags) & (kEntityFlagInAir | 0x40u)) == 0 &&
			m.slide_z > -350 && m.slide_z < 0 && px == start_x && py == start_y &&
			!veh.primary_occupant.valid() && m.spring_energy == 0 &&
			std::all_of(
					std::begin(m.plat_acc), std::end(m.plat_acc),
					[](int32_t value) { return value == 0; })) {
		pz -= m.slide_z; // [orig: @0x47C349]
		m.slide_z >>= 1; // [orig: @0x47C357]
		// Refresh the stability contact byte from the current pose before parked upkeep and flip
		// restoration.
		// Witness sites: [orig: @0x47C35D, @0x47C395, @0x47C3A3, @0x47C443]
		vehicle_rest_state(world, veh, VehicleFamily::Ground);
		return;
	}

	vehicle_expire_contact_wake(world, veh);

	// ---- probe geometry [orig: @0x47C7D0..0x47CB48]: 4 pad probes (the
	// footprint corners inset by r = beam span >> 2, at Z = box_z_lo + r; the
	// per-pad +0x2D4 brake-dive offsets are zero here) + 3 upper-spine probes,
	// in the witnessed slot order 3L/4, L/2, L/4 [orig: @0x47CA6E/@0x47CAFD/
	// @0x47CA26 storing slots 4/5/6].
	const int32_t r = (traits.box_y_hi - traits.box_y_lo) >> 2;
    int32_t rs = std::min(((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000,
                          ((traits.box_y_hi - traits.box_y_lo) >> 1) - 0x1000);
    if (rs < 0x2000) rs = 0x2000; // [orig: @0x47C9E8]
    const int32_t L = traits.box_x_hi - traits.box_x_lo;
    const int32_t ymid =
            traits.box_y_lo + ((traits.box_y_hi - traits.box_y_lo) >> 1);
    const int32_t spine_z = traits.box_z_hi - rs;
    // Each pad rides its wheel's spring compression (+0x2D4 + 4k, the
    // vehicle_suspension.cpp leg) on top of the wheel-height Z
    // [orig: the per-pad `box_z_lo + r + comp[k]` stores @0x47F514/@0x47F56A/
    //  @0x47F5E3/@0x47F63F-pattern in this solve's probe build].
    const int32_t pad_z = traits.box_z_lo + r;
    const int32_t probes_model[7][3] = {
        {traits.foot_x_hi - r, traits.foot_y_hi - r, pad_z + m.wheel_comp[0]}, // pad0 (+fwd,+side)
        {traits.foot_x_hi - r, traits.foot_y_lo + r, pad_z + m.wheel_comp[1]}, // pad1 (+fwd,-side)
        {traits.foot_x_lo + r, traits.foot_y_lo + r, pad_z + m.wheel_comp[2]}, // pad2 (-fwd,-side)
        {traits.foot_x_lo + r, traits.foot_y_hi - r, pad_z + m.wheel_comp[3]}, // pad3 (-fwd,+side)
        {traits.box_x_lo + ((3 * L) >> 2), ymid, spine_z},   // pt4
        {traits.box_x_lo + (L >> 1), ymid, spine_z},         // pt5
        {traits.box_x_lo + (L >> 2), ymid, spine_z},         // pt6
    };
    const int32_t radii[7] = {r, r, r, r, rs, rs, rs};
    const int32_t hull_bottom_neg = -(traits.box_z_lo + r); // [orig: v211 @0x47C832]
    const int32_t half_w =
            (traits.foot_y_hi - r) - (traits.foot_y_lo + r); // [orig: @0x47CB48]
    const int32_t half_h =
            (traits.foot_x_hi - r) - (traits.foot_x_lo + r); // [orig: @0x47CB85]

	VehicleEulerBasis basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	int32_t probes[7][3];
	place_probes(basis, probes_model, px, py, pz, probes);

    // ---- the two force passes + severity (the shared sub-contract with the
    // platform/air solves; terrain leg only) [orig: Entity_CheckCollisionState
    // calls @0x47CB8C/@0x47D213].
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
	EntityHandle hit_entity;
	const int32_t sev =
			plat_probe_pass(world, veh, probes, radii, soft, hard, forces, px, py, pz, &hit_entity);
	vehicle_contact_impact(world, veh, traits, sev, hit_entity, px, py, pz);
	if (sev == 1) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x47CC31]
	} else if (sev == 2) {
		m.speed -= m.speed >> ((traits.torque + 1) & 31); // [orig: @0x47CC71]
	} else if (sev == 3) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x47CCA1]
		// The shared impact fold owns damage, scrape sound and momentum. The terrain-only quarter-
		// speed cut keeps its strongest-probe distance gate.
		// Witness sites: [orig: @0x47CF5E, @0x47D038, @0x47D0DE, @0x47D0EF]
		if (!hit_entity.valid() && strongest_probe_beyond_hull(forces, probes, 7, px, py))
			m.speed = int32_t(m.speed * 0.25); // [orig: flt_7C333C @0x47D0DE]
	}
	int32_t d[7];
    for (int i = 0; i < 7; ++i) d[i] = forces[i].fz;
    if (sev >= 1) {
		// Average the two planar force passes. Scale the resulting position push and vertical
		// depths by the contacted entity mass share.
		// Witness sites: [orig: @0x47D0F5, @0x47D330, @0x47D452, @0x47D458]
		plat_second_pass(world, veh, probes, radii, soft, hard, forces, d, px, py, pz, hit_entity);
	}

	// ---- water leg [orig: @0x47D45B..0x47D629]. Pad support forces raise the
    // hull toward the waterline only on the amphibian dispatch (the mover's
    // hasWaterLevel arg: 0 via the cveh/ctrn dispatchers @0x48efce/@0x48f06e,
    // 2 via the catv/generic router @0x48f010) [orig: the per-pad
    // `max(d, WaterZ - probeZ - v211)` legs @0x47D489..0x47D512].
    if (traits.amphibian && world.env.water_z != 0) {
        for (int k = 0; k < 4; ++k) {
            const int32_t water_d =
                    world.env.water_z - probes[k][2] - hull_bottom_neg;
            if (water_d > d[k]) d[k] = water_d;
        }
    }
	// Water hysteresis uses the pad radius and retains the no-water sentinel. Entry emits the
	// local effect and positioned sound; exit retires water trails.
	// Witness sites: [orig: @0x47D516, @0x47D53A, @0x47D637, @0x47D6BB, @0x47D542, @0x47D629]
	plat_water_flag(world, veh, traits, probes, r, hull_bottom_neg, world.env.water_z);

	// The stability contact byte feeds yaw integration and the slope-following velocity re-derive.
	// Witness sites: [orig: @0x47D7F4, @0x47D8AF]
	int32_t up_z16 = static_cast<int32_t>(basis.up[2] * io::kFp16OneD);
	const int32_t fwd_z16 = static_cast<int32_t>(basis.fwd[2] * io::kFp16OneD);
	for (int k = 0; k < 4; ++k) m.dbg_pad_depth[k] = d[k]; // diagnostic tap ("pd")
    const bool pair_contact =
            (d[0] != 0 && d[3] != 0) || (d[1] != 0 && d[2] != 0) ||
            (d[0] != 0 && d[2] != 0) || (d[1] != 0 && d[3] != 0);
    const bool any_pad = d[0] != 0 || d[1] != 0 || d[2] != 0 || d[3] != 0;
    m.grounded = (up_z16 > 4096 && pair_contact) ||
                 (any_pad && fwd_z16 < 24576 && traits.mass <= 10 &&
                  up_z16 > 4096);

    // ---- the suspension spring leg, in the witnessed order
    // (vehicle_suspension.h): the crash tests, then the extend loop's sink
    // growth over the four pads at ftol(dt * 250) with the pre-gate skip
    // [orig: the tests @0x47D745..0x47D7A8 + the client window @0x47E793..
    //  0x47E7EE; the extend loop @0x47DB70..0x47DBD1, its pre-gate
    //  @0x47DB76..0x47DBA8; the dt select on +0x2EC @0x47C1DE..0x47C222].
	const bool pad_contact[7] = { d[0] != 0, d[1] != 0, d[2] != 0, d[3] != 0, d[4] != 0, d[5] != 0,
		d[6] != 0 };
	vehicle_landing_damage(world, veh, traits, d, 7, up_z16);
	world.vehicles.suspension_crash_tests(veh, traits, up_z16, SuspensionFamily::Tracked);
	const int32_t sink_growth = static_cast<int32_t>(
            (m.crashed != 0 ? kSuspensionDtCrashed : kSuspensionDtNormal) *
            static_cast<float>(kSinkGrowthPerTick));
    const bool sinks_zero = m.plat_acc[0] == 0 && m.plat_acc[1] == 0 &&
                            m.plat_acc[2] == 0 && m.plat_acc[3] == 0;
    vehicle_suspension_grow_sinks(veh, pad_contact, 4, sink_growth,
                                  /*latch_gated=*/true,
                                  m.settled_2f2 == 0 && sinks_zero && up_z16 < 0);
	vehicle_crush_damage(world, veh, traits, d + 4, up_z16);
	vehicle_crash_state(world, veh, traits, pad_contact, up_z16);
	basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	up_z16 = basis.q22.m[10] >> 6;
	m.grounded = m.grounded && !m.crashed && !m.settle_2f0 && !m.wreck_2fc;
	// [orig: Entity_ProcessTrackedVehiclePhysics @ 0x47E09F..0x47E107]
	int32_t crash_depth = -1;
	if (m.crashed || up_z16 < 0 || m.settle_2f0)
		for (int i = 0; i < 7; ++i) crash_depth = std::max(crash_depth, d[i]);
	int32_t corner_adj[4] = { 0, 0, 0, 0 };

	// ---- solve select: the no-pad branch [orig: the all-zero pad test
    // @0x47E1A9].
    if (!any_pad) {
        if (up_z16 < 0) {
			// Inverted spine contact lifts the hull by the greatest penetration across all seven
			// probes and feeds crush/crash state.
			// Witness sites: [orig: @0x47E1E9, @0x47E23E, @0x47E358, @0x47E4C6]
			int32_t maxd = 0;
			for (int i = 0; i < 7; ++i) maxd = std::max(maxd, d[i]);
            if (maxd > 0) {
                pz += maxd;
                veh.flags &= ~kEntityFlagInAir;
                vehicle_suspension_tick_tail(veh, traits);
				m.contact_solved_once = true;
				m.plat_airborne_ticks =
						(veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;
				return;
			}
        }
		// Airborne sets the flag and fits the unlifted corners before parked spring application.
		// Witness sites: [orig: @0x47E57B, @0x47E5E6, @0x47E283]
		world.vehicles.suspension_airborne_loop(veh, traits, 4, sink_growth, corner_adj);
		veh.flags |= kEntityFlagInAir;
		int32_t corners[4][3];
		const int32_t hx = half_h >> 1, hy = half_w >> 1;
		const int32_t local[4][3] = { { hx, hy, 0 }, { hx, -hy, 0 }, { -hx, -hy, 0 },
			{ -hx, hy, 0 } };
		for (int k = 0; k < 4; ++k) {
			int32_t rotated[3];
			basis.q22.rotate_point(local[k], rotated);
			corners[k][0] = io::bam_add(px, rotated[0]);
			corners[k][1] = io::bam_add(py, rotated[1]);
			corners[k][2] = io::bam_add(pz, rotated[2]);
		}
		PlatFit fit;
		vehicle_suspension_fit(world, veh, corners, nullptr, fit, px, py, pz);
		m.air_pitch_bam = fit.pitch_bam;
		m.air_roll_bam = fit.roll_bam;
		if (m.crashed)
			m.yaw_bam = fit.yaw_bam;
		vehicle_suspension_tick_tail(veh, traits);
		m.contact_solved_once = true;
		m.plat_airborne_ticks = (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;
		return;
	}
	if (d[2] != 0 && d[3] != 0)
		vehicle_capture_contact_direction(veh, basis);
	// ---- pad contact: airborne clears unconditionally [orig: @0x47E8EE].
	veh.flags &= ~kEntityFlagInAir;
    // The grounded spring loop over this tick's pad depths: the catch-up, the
    // landing impulse, the settle term and the compress/oscillate arms. It
    // RESOLVES d_k (the wheel absorbs the compression delta) and the
    // compressions feed NEXT tick's pad points above
    // [orig: the loop @0x47E960..0x47EC1F; `d_k -= delta` @0x47EBC3].
    world.vehicles.suspension_grounded_loop(veh, traits, 4, d, pad_contact,
                                     sink_growth, corner_adj);
    // The pad-rectangle bounding quad, in the witnessed winding — corner k
    // sits over pad k, so the identity d_k lift pairing is geometric here
    // [orig: Entity_ComputeBoundingQuad @0x45B6E0 non-square arm, called
    // @0x47DAE2/@0x47DB54: c0=(+f,+s), c1=(+f,-s), c2=(-f,-s), c3=(-f,+s);
    // corner_z[k] += the resolved d_k @0x47EC05 and the catch-up term @0x47E9EF].
    int32_t c[4][3];
    {
        const int32_t hw2 = half_w >> 1, hh2 = half_h >> 1;
        const int32_t corner_model[4][3] = {
            {+hh2, +hw2, 0}, {+hh2, -hw2, 0}, {-hh2, -hw2, 0}, {-hh2, +hw2, 0},
        };
        for (int k = 0; k < 4; ++k) {
            int32_t rotated[3];
            basis.q22.rotate_point(corner_model[k], rotated);
            c[k][0] = px + rotated[0];
            c[k][1] = py + rotated[1];
            c[k][2] = pz + rotated[2] + d[k] + corner_adj[k];
        }
    }
    PlatFit fit;
	vehicle_suspension_fit(world, veh, c, pad_contact, fit, px, py, pz);
	// Conform always adopts pitch and roll; crash state also adopts yaw.
	// Witness sites: [orig: @0x47EC62, @0x47EC83, @0x47EC75, @0x47EC8F]
	m.air_pitch_bam = fit.pitch_bam;
	m.air_roll_bam = fit.roll_bam;
	if (m.crashed != 0)
		m.yaw_bam = fit.yaw_bam;
	veh.pitch = static_cast<int16_t>(std::lround(double(m.air_pitch_bam) * kDegreesPerBam));
	veh.roll = static_cast<int16_t>(std::lround(
            double(m.air_roll_bam) * kDegreesPerBam));
    if (m.crashed == 0 && up_z16 > 0) {
        // Upright: slideDecay clamped toward the ground, then Z = the solver
        // chassis Z — the wheeled solver's positive-corner average,
        // rise-clamped +0x2000/tick [orig: the Z select @0x47ECAC..0x47ECBB;
        // solver Z @0x46C822..0x46C894 in Entity_ProcessWheeledVehicleSuspension
        // @0x46B140, call @0x47EC4D].
        if (!m.settle_2f0 && m.slide_z > 0) m.slide_z = 0;
        int32_t new_z = fit.positive_z_avg;
        if (new_z > pz + 0x2000) new_z = pz + 0x2000;
        pz = new_z;
    } else {
        // Inverted: lift by the max penetration over all seven probes
        // [orig: `Position.Z += maxGroundHeight` @0x47ECE2 with the
        // @0x47E09F..0x47E107 scan].
		pz = io::bam_add(pz, crash_depth);
    }
    // The crash latch arms inside the wheel-solver call [orig: @0x47EC4D ->
    // @0x46B1A6..0x46B213]; the pads back in contact then reset the sinks
    // (per pad, then the diagonal-pair clear on an upright hull)
    // [orig: @0x47ECE9..0x47ED60] and the tail clears the per-tick request
    // [orig: @0x47EEEE].
    vehicle_suspension_post_contact(veh, pad_contact, 4, up_z16);
	// [orig: @0x47ED67..0x47EE49] the final contact pass may recover an
	// inverted hull after its replicated parked bit was cleared.
	if (((veh.flags | veh.engine_flags) & 0x10u) == 0 && up_z16 < 0) {
		vehicle_rebuild_rest_orientation(world, veh, false);
		m.crashed = 0;
		m.byte_2ef = 0;
		m.settle_2f0 = 0;
	}
	vehicle_suspension_tick_tail(veh, traits);
	m.contact_solved_once = true;
	m.plat_airborne_ticks = (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;
}

// Tank/wheeled contact uses thirteen probes: four pads, six belly stations and three spine probes.
// Its spring pair, stability flags, head-on wall stop and slide_z absorption differ from the
// tracked family. Authority damage, client prediction and wreck state share the original gates.
// See vehicle-client-movers-re.md sections 8 and 12-32.
// Witness sites: [orig: @0x475DE0, @0x488AB0, @0x48a9ef, @0x48f004, @0x477157, @0x47733f,
// @0x477e6a, @0x45CEB0, @0x45D240]
void wheeled_contact_solve(World &world, Entity &veh, const VehicleTraits &traits, Entity::VehicleMotorState &m, int32_t start_x, int32_t start_y, int32_t &px, int32_t &py, int32_t &pz) {
	// The wheeled sleep gate checks zero motion, the family flags, occupancy and spring energy.
	// Witness sites: [orig: @0x475E5C, @0x475FB2, @0x475F51, @0x475F5F, @0x475F8F, @0x475FA1,
	// @0x476040]
	if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 && m.wheel_rate_bam == 0 &&
			m.air_pitch_rate == 0 && m.air_roll_rate == 0 &&
			((veh.flags | veh.engine_flags) & (kEntityFlagInAir | 0x40u)) == 0 &&
			m.slide_z > -350 && m.slide_z < 0 && px == start_x && py == start_y &&
			!veh.primary_occupant.valid() && m.spring_energy == 0 &&
			std::all_of(
					std::begin(m.plat_acc), std::end(m.plat_acc),
					[](int32_t value) { return value == 0; })) {
		pz -= m.slide_z;
		m.slide_z >>= 1;
		vehicle_rest_state(world, veh, VehicleFamily::Tank);
		return;
	}

	vehicle_expire_contact_wake(world, veh);
	if (!m.contact_solved_once && (veh.flags & 0x40u) == 0)
		vehicle_clear_chassis(m); // [orig: @0x47606E..0x47607D]

	// Clamp the authored spring, compression and flip settings before advancing their state.
	// Witness sites: [orig: @0x476070, @0x4760E5, @0x4762B8, @0x476559, @0x4762F2, @0x47653A]
	const int32_t r = (traits.box_y_hi - traits.box_y_lo) >> 2;
	const int32_t Lbox = traits.box_x_hi - traits.box_x_lo;
    const int32_t beam = traits.box_y_hi - traits.box_y_lo;
    const int32_t ymid_box =
            traits.box_y_lo + (beam >> 1);
    const int32_t pad_z = traits.box_z_lo + r;
    const int32_t w = (beam >> 3) - 0x4000;
    const int32_t q = static_cast<int32_t>(double(Lbox) * 0.75) >> 2;
    const int32_t bx = traits.foot_x_lo + r; // belly-station base X
    const int32_t ys = traits.foot_y_hi - r; // starboard rail (+side)
    const int32_t yp = traits.foot_y_lo + r; // port rail (-side)
	// The middle channels are sampled from the pre-solve wheel compressions.
	// Belly stations ride their corresponding front/rear/middle compression.
	// [orig: @0x476432..0x476473; probes @0x47668E..0x47688B]
	m.wheel_comp[4] = io::bam_sar(io::bam_add(m.wheel_comp[0], m.wheel_comp[3]), 1);
	m.wheel_comp[5] = io::bam_sar(io::bam_add(m.wheel_comp[1], m.wheel_comp[2]), 1);
	const int32_t probes_model[13][3] = {
		{ traits.foot_x_hi - r, ys, pad_z + m.wheel_comp[0] }, // pad0 (+fwd,+side)
		{ traits.foot_x_hi - r, yp, pad_z + m.wheel_comp[1] }, // pad1 (+fwd,-side)
		{ traits.foot_x_lo + r, yp, pad_z + m.wheel_comp[2] }, // pad2 (-fwd,-side)
		{ traits.foot_x_lo + r, ys, pad_z + m.wheel_comp[3] }, // pad3 (-fwd,+side)
		{ bx + 3 * q, ys, pad_z + m.wheel_comp[0] }, // belly4 (fwd,+side)
		{ bx + 3 * q, yp, pad_z + m.wheel_comp[1] }, // belly5 (fwd,-side)
		{ bx + q, yp, pad_z + m.wheel_comp[2] }, // belly6 (rear,-side)
		{ bx + q, ys, pad_z + m.wheel_comp[3] }, // belly7 (rear,+side)
		{ bx + 2 * q, ys, pad_z + m.wheel_comp[4] }, // belly8 (mid,+side)
		{ bx + 2 * q, yp, pad_z + m.wheel_comp[5] }, // belly9 (mid,-side)
		{ traits.box_x_lo + ((3 * Lbox) >> 2), ymid_box, traits.box_z_hi - w },
		{ traits.box_x_lo + (Lbox >> 1), ymid_box, traits.box_z_hi + w },
		{ traits.box_x_lo + (Lbox >> 2), ymid_box, traits.box_z_hi - w },
	};
	const int32_t hull_bottom_neg = -(traits.box_z_lo + r); // [orig: -v41 @0x4761B4]
	const int32_t half_w =
            (traits.foot_y_hi - r) - (traits.foot_y_lo + r); // [orig: @0x4761E2]
    const int32_t half_h =
            (traits.foot_x_hi - r) - (traits.foot_x_lo + r);

	VehicleEulerBasis basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	int32_t probes[13][3];
	place_probes(basis, probes_model, px, py, pz, probes);
    int32_t radii[13]; // the 13 radius stores all copy suspensionOffset
    for (int i = 0; i < 13; ++i) radii[i] = r;

	// The two force passes retain per-probe severity. Shared impact response applies damage, sound
	// and momentum; a terrain-only hit uses the strongest-point quarter-speed cut.
	// Witness sites: [orig: @0x476897, @0x476F98, @0x476944, @0x476984, @0x4769B8, @0x476BC3,
	// @0x476C4D]
	const int32_t soft = cos22_of_bam_x87(traits.max_slope);
	const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[13];
	EntityHandle hit_entity;
	const int32_t sev =
			plat_probe_pass(world, veh, probes, radii, soft, hard, forces, px, py, pz, &hit_entity);
	vehicle_contact_impact(world, veh, traits, sev, hit_entity, px, py, pz);
	if (sev == 1) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31);
	} else if (sev == 2) {
		m.speed -= m.speed >> ((traits.torque + 1) & 31);
	} else if (sev == 3) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31);
        // strongest_probe_beyond_hull over the first SEVEN probes only.
		if (!hit_entity.valid() && strongest_probe_beyond_hull(forces, probes, 7, px, py))
			m.speed = int32_t(m.speed * 0.25);
	}
	int32_t d[13];
    for (int i = 0; i < 13; ++i) d[i] = forces[i].fz;
    if (sev >= 1) {
		// Average the second planar pass into the first and apply the entity mass share to
		// displacement and depths.
		// Witness sites: [orig: @0x476E19, @0x476FF4, @0x477149]
		plat_second_pass(world, veh, probes, radii, soft, hard, forces, d, px, py, pz, hit_entity);
	}

	// The wheeled water latch uses mean wheel height with r/2 hysteresis. The transition owns
	// splash, sound and trail retirement.
	// Witness sites: [orig: @0x477496, @0x48f004]
	plat_water_flag(world, veh, traits, probes, r, hull_bottom_neg, world.env.water_z);

	// ---- per-probe planar-contact + reverse flags [orig: the per-probe
	// walk @0x477BF4..0x477D0A]: a probe "contacts" when it produced a planar
	// force (flat terrain never does; walls and slope-hard faces do), and a
	// contacted probe is a REVERSE hit when its normalized force opposes the
	// Stored downhill/skid direction, with the forward column as fallback.
	// [orig: @0x477A48; per-probe reverse threshold -49152, head-on stop
	// @0x477D3E..0x477E30 threshold -57070]
	int32_t direction[3];
	for (int i = 0; i < 3; ++i)
		direction[i] =
				vehicle_has_contact_direction(m) ? m.contact_direction[i] : basis.q22.m[i * 4] >> 6;
	const auto force_dot = [&](const int64_t force[3]) {
		int32_t unit[3];
		q16_normalize(force, unit);
		return io::bam_add(
				io::bam_add(q16_mul_rhu(unit[0], direction[0]), q16_mul_rhu(unit[1], direction[1])),
				q16_mul_rhu(unit[2], direction[2]));
	};
	bool contact_flag[13];
	bool reverse_flag[13];
    bool any_contact = false;
	int64_t sum[3] = {};
	for (int i = 0; i < 13; ++i) {
		contact_flag[i] = forces[i].fx != 0 || forces[i].fy != 0;
        reverse_flag[i] = false;
        if (!contact_flag[i]) continue;
        any_contact = true;
		const int64_t force[3] = { forces[i].fx, forces[i].fy, forces[i].fz };
		for (int k = 0; k < 3; ++k)
			sum[k] += force[k];
		reverse_flag[i] = force_dot(force) < -49152;
	}
	if (any_contact && force_dot(sum) < -57070) {
		m.vel_x = m.vel_y = m.speed = 0;
	}

	// Rebuild stability from the contact flags and the current up vector, retaining the crash/park
	// gates.
	// Witness sites: [orig: @0x477F31, @0x477FA9, @0x477FB4]
	int32_t up_z16 = static_cast<int32_t>(basis.up[2] * io::kFp16OneD);
	{
		bool stable;
        if (m.speed >= 0) {
            if (reverse_flag[0] || reverse_flag[1]) stable = false;
            else stable = contact_flag[0] == contact_flag[1];
        } else {
            if (reverse_flag[2] || reverse_flag[3]) stable = false;
            else stable = contact_flag[2] == contact_flag[3];
        }
        m.grounded = up_z16 > 0x2000 && stable;
	}

	// ---- the 7-slot contact model [orig: the wheel/belly pair maxes
    // @0x4780E5..0x478131 — slot k = max(d_k, d_{k+4}) for the four wheels,
    // slots 4..6 = the spine d's] and the solve select [orig: the
    // `centerMax <= 0 && wheelBellyMax <= 0` split @0x478435].
    int32_t slot_max[7];
    for (int k = 0; k < 4; ++k) slot_max[k] = std::max(d[k], d[k + 4]);
    slot_max[4] = d[10];
    slot_max[5] = d[11];
    slot_max[6] = d[12];
    int32_t center_max = std::max(d[8], d[9]);
    int32_t wheel_belly_max = d[0];
    for (int i = 1; i < 8; ++i) wheel_belly_max = std::max(wheel_belly_max, d[i]);

    // ---- the suspension spring leg's tank legs (vehicle_suspension.h): the
    // crash tests [orig: @0x477760..0x4777BF + the client window @0x478B6C..
    //  0x478BD6] and the +250 sink growth over the four wheel pads
    // [orig: @0x478510..0x47852B]. The tank's own spring pair (the linear
    // compress / slow oscillator) is the header's named residual.
    const bool wheel_contact[4] = {d[0] != 0, d[1] != 0, d[2] != 0, d[3] != 0};
	vehicle_landing_damage(world, veh, traits, d, 7, up_z16);
	world.vehicles.suspension_crash_tests(veh, traits, up_z16, SuspensionFamily::Tank);
	vehicle_suspension_grow_sinks(veh, wheel_contact, 4, kSinkGrowthTank,
                                  /*latch_gated=*/true, /*pre_gate_skip=*/false);

	const bool crash_contacts[7] = { d[0] != 0, d[1] != 0, d[2] != 0, d[3] != 0, d[4] != 0,
		d[5] != 0, d[6] != 0 };
	vehicle_crush_damage(world, veh, traits, d + 10, up_z16);
	vehicle_crash_state(world, veh, traits, crash_contacts, up_z16);
	basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	up_z16 = basis.q22.m[10] >> 6;

	const auto finish_contact = [&] {
		// Release compression into available terrain clearance, including the
		// airborne tail [orig: @0x4790A1..0x47919A / @0x479205..0x4792FE].
		for (int k = 0; k < 4; ++k) {
			if (forces[k].terrain_gap <= 0)
				continue;
			m.wheel_comp[k] = std::max(0, io::bam_sub(m.wheel_comp[k], forces[k].terrain_gap));
			m.wheel_osc[k].extension = io::bam_sub(65535, m.wheel_comp[k]);
		}
		// [orig: @0x47931B..0x47943E] tank recovery does not require
		// a negative up axis: the crashed arm accepts up.z < 8192.
		for (int k = 0; k < 4; ++k)
			if (d[k] > 0)
				m.plat_acc[k] = 0;
		if (((veh.flags | veh.engine_flags) & 0x10u) == 0 &&
				((m.crashed && up_z16 < 8192) || m.settle_2f0)) {
			vehicle_rebuild_rest_orientation(world, veh, false);
			m.crashed = 0;
			m.settle_2f0 = 0;
		}
		vehicle_suspension_tick_tail(veh, traits);
		vehicle_contact_downhill_tail(veh, pz);
		m.contact_solved_once = true;
		m.plat_airborne_ticks = (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;
	};
	// Preserve depths before the wheel springs absorb penetration.
	// [orig: Entity_ProcessWheeledVehiclePhysics @ 0x477FF4..0x478014;
	// retained-depth selection @0x478D15]
	int32_t crash_depth = -1;
	if (m.crashed || up_z16 < 0 || m.settle_2f0)
		for (int i = 0; i < 13; ++i) crash_depth = std::max(crash_depth, d[i]);
	int32_t wheel_depth = -1;
	for (int i = 0; i < 4; ++i) wheel_depth = std::max(wheel_depth, d[i]);
	// Positive terrain gap arms the per-wheel landing impulse while the
	// current amplitude is still small. [orig: @0x478643..0x478706]
	for (int k = 0; k < 4; ++k) {
		const int32_t gap = forces[k].terrain_gap;
		if (gap > 0 && gap > m.wheel_osc[k].impulse && m.wheel_osc[k].amplitude < 500)
			m.wheel_osc[k].impulse = gap >> 1;
	}
	int32_t corner_adj[4] = {};
	if (center_max <= 0 && wheel_belly_max <= 0) {
		world.vehicles.suspension_tank_loop(veh, traits, false, d, wheel_contact, corner_adj);
		if (up_z16 < 0) {
			// Inverted spine contact lifts from the greatest penetration and applies the family
			// crush/crash gates.
			// Witness sites: [orig: @0x47843E, @0x4784DA, @0x478540]
			int32_t maxs = 0;
			for (int i = 0; i < 7; ++i) maxs = std::max(maxs, slot_max[i]);
            if (maxs > 0) {
                pz += maxs;
				finish_contact();
				return;
			}
		}
		// Airborne sets the flag and fits the unlifted quad; Flags 0x10 controls parked spring
		// application.
		// Witness sites: [orig: @0x478522, @0x478509]
		veh.flags |= kEntityFlagInAir;
		int32_t corners[4][3];
		const int32_t hx = half_h >> 1, hy = half_w >> 1;
		const int32_t local[4][3] = { { hx, hy, 0 }, { hx, -hy, 0 }, { -hx, -hy, 0 },
			{ -hx, hy, 0 } };
		for (int k = 0; k < 4; ++k) {
			int32_t rotated[3];
			basis.q22.rotate_point(local[k], rotated);
			corners[k][0] = io::bam_add(px, rotated[0]);
			corners[k][1] = io::bam_add(py, rotated[1]);
			corners[k][2] = io::bam_add(pz, rotated[2]);
		}
		PlatFit fit;
		vehicle_suspension_fit(world, veh, corners, crash_contacts, fit, px, py, pz, true);
		m.air_pitch_bam = fit.pitch_bam;
		m.air_roll_bam = fit.roll_bam;
		if (m.crashed)
			m.yaw_bam = fit.yaw_bam;
		finish_contact();
		return;
	}
	// ---- wheel/belly contact: airborne clears unconditionally
    // [orig: @0x478604 in the latch fall-through]. The corner quad lifts by
    // the four WHEEL d's only (belly/spine d's feed severity and the Z maxes)
    // in the spring loop [orig: `dest[corner].z += d_k`
    // @0x478A16/@0x478A72-region]. The wheeled family's spring pair is the
    // LINEAR compress (Suspension_CompressWheelLinear @0x45CEB0) with the SLOW
    // oscillator (Suspension_OscillateWheel @0x45D240) — the named residual in
    // vehicle_suspension.h; the crash latch arms at the tail as in the tracked
    // solve.
    veh.flags &= ~kEntityFlagInAir;
	world.vehicles.suspension_tank_loop(veh, traits, true, d, wheel_contact, corner_adj);
	int32_t c[4][3];
	{
        const int32_t hw2 = half_w >> 1, hh2 = half_h >> 1;
        const int32_t corner_model[4][3] = {
            {+hh2, +hw2, 0}, {+hh2, -hw2, 0}, {-hh2, -hw2, 0}, {-hh2, +hw2, 0},
        };
        for (int k = 0; k < 4; ++k) {
            int32_t rotated[3];
            basis.q22.rotate_point(corner_model[k], rotated);
            c[k][0] = px + rotated[0];
            c[k][1] = py + rotated[1];
			c[k][2] = pz + rotated[2] + d[k] + corner_adj[k];
		}
	}
    PlatFit fit;
	const bool fitted = vehicle_suspension_fit(world, veh, c, crash_contacts, fit, px, py, pz, true);
	// Conform writes pitch and roll, and adopts yaw in the crashed/latched arms.
	// Witness sites: [orig: @0x478AB1, @0x478AD8]
	m.air_pitch_bam = fit.pitch_bam;
	m.air_roll_bam = fit.roll_bam;
	if (m.crashed != 0)
		m.yaw_bam = fit.yaw_bam;
	veh.pitch = static_cast<int16_t>(std::lround(double(m.air_pitch_bam) * kDegreesPerBam));
	veh.roll = static_cast<int16_t>(std::lround(
            double(m.air_roll_bam) * kDegreesPerBam));
    if (m.crashed == 0 && up_z16 > 0) {
		// An upright hull adopts the spring-resolved chassis Z; parked application retains the
		// family crash and latch gates.
		// Witness sites: [orig: @0x4698A0, @0x46AF54, @0x46AFE1, @0x478BE3, @0x478C06]
		if (fitted) {
			const int32_t new_z = fit.positive_z_avg;
			m.slide_z = io::bam_add(m.slide_z, io::bam_sub(new_z, pz));
			if (m.slide_z > 0) m.slide_z = 0;
			pz = new_z;
		} else pz = io::bam_add(pz, wheel_depth);
    } else {
		// Crashed/inverted hulls use the saved depth, even when still upright.
		// [orig: Entity_ProcessWheeledVehiclePhysics @ 0x475DE0]
		pz = io::bam_add(pz, crash_depth);
    }
    // The crash latch arms inside the suspension call [orig: @0x4698A0's seed
    // @0x469933..0x46999E]; the tail clears the per-tick request [orig:
    // @0x4795DA].
	finish_contact();
	// Airborne tick counter [orig: @0x4795D4..0x4795F1].
}

// Bike/light contact uses two wheel probes, three spine probes and one mid-hull probe. The two-
// corner axle fit preserves roll continuity and mean wheel height. Rider ejection, wheelie, spring
// and tumble state retain the family gates. The bike lean helper is Entity_SmoothHeadingToTarget
// at 0x45B2C0 (sole caller @0x47A7D3; Vehicle_UpdateTurretRotation @0x45AEA0 is the boat's, sole
// caller @0x4838CF). See vehicle-client-movers-re.md sections 9 and 12-32.
// Witness sites: [orig: @0x479600, @0x483FE0, @0x486672, @0x48eff4, @0x468A50, @0x468D34,
// @0x47A343, @0x45CFB0, @0x45D110, @0x45B2C0, @0x47a7d3]
bool light_contact_solve_active(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           traits.box_x_hi != traits.box_x_lo &&
           (((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000) > 0;
}

namespace {

constexpr int32_t kBikeFallSeedBam = 0x016C16C1;
constexpr int32_t kBikeFallAccelerationBam = 298261;

int32_t q22_mul_trunc(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 22);
}

// Square footprint: the lower axle, then its copy lifted by half the model height.
// [orig: Entity_ComputeBoundingQuad @ 0x45B6E0]
static void light_bounding_quad(const VehicleTraits &traits, const VehicleEulerBasis &basis,
        int32_t x, int32_t y, int32_t z, int32_t out[4][3]) {
    const int32_t radius = (traits.box_z_hi - traits.box_z_lo) / 2 - 16384;
    const int32_t half_length = ((traits.foot_x_hi - radius) - (traits.foot_x_lo + radius)) / 2;
    const int32_t height = io::bam_abs(io::bam_sub(traits.box_z_hi, traits.box_z_lo)) / 2;
    const int32_t pos[3] = {x, y, z};
    for (int k = 0; k < 3; ++k) {
        const int32_t forward = q16_mul_rhu(half_length, basis.q22.m[4*k] >> 6);
        const int32_t lift = q16_mul_rhu(height, basis.q22.m[4*k+2] >> 6);
        out[0][k] = io::bam_add(pos[k], forward);
        out[1][k] = io::bam_sub(pos[k], forward);
        out[2][k] = io::bam_add(out[1][k], lift);
        out[3][k] = io::bam_add(out[0][k], lift);
    }
}

// Preserve SIDE, rebuild UP from axle x side, then forward from side x up.
// Column helpers and the cross-product operands, not Hex-Rays local names,
// determine the axes. [orig: Entity_UpdateVehicleChassisOrientation @ 0x468A50;
// Math_FixedPointCrossProduct @ 0x6134A0; CWnd_HitTest @ 0x6137D0]
static CollisionMatrix light_axle_frame(const int32_t corners[4][3], const CollisionMatrix &old) {
    const int64_t axle[3] = {int64_t(corners[0][0]) - corners[1][0],
            int64_t(corners[0][1]) - corners[1][1], int64_t(corners[0][2]) - corners[1][2]};
    const int32_t side[3] = {old.m[1] >> 6, old.m[5] >> 6, old.m[9] >> 6};
    int32_t forward[3], up[3];
    int64_t cross[3];
    q16_normalize(axle, forward);
    q16_cross(forward, side, cross);
    q16_normalize(cross, up);
    q16_cross(side, up, cross);
    q16_normalize(cross, forward);
    CollisionMatrix fitted = old;
    for (int k = 0; k < 3; ++k) {
        fitted.m[4*k] = bam_shl_wrap(forward[k], 6);
        fitted.m[4*k+1] = bam_shl_wrap(side[k], 6);
        fitted.m[4*k+2] = bam_shl_wrap(up[k], 6);
    }
    return fitted;
}

static void light_apply_frame(World &world, Entity &entity, CollisionMatrix matrix, bool write_yaw) {
    if (!entity.veh.crashed) vehicle_apply_lean(entity, matrix);
    vehicle_apply_chassis(world, entity, matrix);
    int32_t angles[3];
    collision_matrix_to_euler(matrix, angles);
    auto &m = entity.veh;
    m.air_pitch_bam = angles[1]; m.air_roll_bam = angles[2];
    if (write_yaw) m.yaw_bam = angles[0];
    entity.pitch = static_cast<int16_t>(std::lround(double(angles[1]) * kDegreesPerBam));
    entity.roll = static_cast<int16_t>(std::lround(double(angles[2]) * kDegreesPerBam));
}

// Normalize displacement, replace its X/Y with the original forward row, normalize again.
// [orig: Entity_ProcessLightVehiclePhysics @ 0x47BF01..0x47C03B]
static void light_capture_launch(Entity &entity, const VehicleEulerBasis &basis,
        int32_t x, int32_t y, int32_t z) {
    const int32_t fallback[3] = {to_fixed(entity.position.x), to_fixed(entity.position.y),
            to_fixed(entity.position.z)};
    const int32_t *previous = entity.saved_live_valid ? entity.saved_live_pos : fallback;
    const int64_t displacement[3] = {
        io::bam_sub(x, previous[0]), io::bam_sub(y, previous[1]), io::bam_sub(z, previous[2])};
    int32_t direction[3];
    q16_normalize(displacement, direction);
    const int64_t raw[3] = {basis.q22.m[0] >> 6, basis.q22.m[4] >> 6, direction[2]};
    q16_normalize(raw, entity.veh.bike_launch_direction);
}

// The fallen-bike arm inside Entity_UpdateVehicleChassisOrientation
// [orig: @0x468B62..0x468D83]. Once the crashed/override pair is active,
// simultaneous front-wheel, rear-wheel and center-spine contact at low speed
// latches +0x2FC. The shared +0x460 part-spin word becomes its angular rate:
// a moving bike starts at roughly two degrees/tick, the current frame is
// right-multiplied by a local-Y rotation, then the rate loses 298261 BAM.
// Entity_UpdatePartSpinAccumulator runs later in the mover and intentionally
// acts on that same word, exactly as it does in retail.
bool light_fall_over_tick(World &world, Entity &veh, const VehicleTraits &traits,
		const VehicleEulerBasis &basis, const int32_t d[6], int32_t px, int32_t py, int32_t pz,
		const int32_t corner_adjust[4]) {
	auto &m = veh.veh;
	if (!m.crashed)
		return false;
	CollisionMatrix matrix = basis.q22;
    int32_t corners[4][3], up[3], side[3];
    light_bounding_quad(traits, basis, px, py, pz, corners);
    for (int k = 0; k < 3; ++k) {
        up[k] = matrix.m[4*k+2] >> 6;
        side[k] = matrix.m[4*k+1] >> 6;
    }
	if ((veh.flags & kEntityFlagInAir) == 0) {
		corners[0][2] = io::bam_add(corners[0][2], io::bam_add(d[0], corner_adjust[0]));
		corners[1][2] = io::bam_add(corners[1][2], io::bam_add(d[1], corner_adjust[1]));
	}
	const auto force = [&](int corner, int32_t rate, const int32_t direction[3]) {
		std::copy_n(direction, 3, m.chassis_forces[corner].direction);
		m.chassis_forces[corner].rate = rate;
	};
	const auto apply = [&] {
		vehicle_clear_chassis_forces(veh, corners, 1);
		vehicle_apply_chassis(world, veh, matrix);
	};
	if (!m.byte_2ef) {
		if (m.wheelie_active) {
			force(0, 5000, up);
			force(3, 5000, up);
		}
		apply();
	} else {
		const int32_t speed = io::bam_abs(m.speed);
		const int32_t rate = speed <= 256 ? 4000 : 5000;
		const bool both = (d[0] > 250 && d[1] > 250) || d[4] > 0;
		if (both)
			m.wheelie_active = 0;
		// [orig: Entity_UpdateVehicleChassisOrientation contact/speed gate @0x468BC1]
		const bool low_contact = d[0] > 0 && d[1] > 0 && d[4] > 0 && speed < 8192;
		if (low_contact && !m.wreck_2fc) {
			vehicle_clear_chassis(m);
			m.wreck_2fc = 1;
			if (speed > 4096)
				m.part_spin.speed = kBikeFallSeedBam;
		}
		if (d[5] > 5000)
			m.wheelie_active = 0;
		if (m.wreck_2fc) {
			const auto rotation = vehicle_euler_basis(0, m.part_spin.speed, 0).q22;
			CollisionMatrix result;
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 3; ++col) {
					uint64_t sum = 0x200000;
					for (int k = 0; k < 3; ++k)
						sum += uint64_t(int64_t(matrix.m[4 * row + k]) * rotation.m[4 * k + col]);
					result.m[4 * row + col] = int32_t(sum >> 22);
				}
			matrix = result;
			m.part_spin.speed = io::bam_sub(m.part_spin.speed, kBikeFallAccelerationBam);
		} else {
			vehicle_clear_chassis(m);
			if (m.wheelie_active && !both) {
				force(0, rate, up);
				force(3, rate, up);
				apply();
			}
			if (!low_contact) {
				const int first = d[5] > 500 || up[2] < 0 ? 0 : 2;
				for (int i = first; i < first + 2; ++i)
					if (d[i] == 0)
						force(i, rate, side);
				apply();
			}
		}
	}
	int32_t angles[3];
	collision_matrix_to_euler(matrix, angles);
	m.yaw_bam = angles[0];
	m.air_pitch_bam = angles[1];
	m.air_roll_bam = angles[2];
	veh.pitch = int16_t(std::lround(double(angles[1]) * kDegreesPerBam));
	veh.roll = int16_t(std::lround(double(angles[2]) * kDegreesPerBam));
	return true;
}

} // namespace

void light_contact_solve(World &world, Entity &veh, const VehicleTraits &traits, Entity::VehicleMotorState &m, int32_t start_x, int32_t start_y, int32_t &px, int32_t &py, int32_t &pz) {
    // ---- sleep fast-path [orig: @0x47972C..0x479790]: velocities/speed/
    // rates zero, not airborne, not carried, slideDecay in (-300,-1] (the
    // unsigned `> 0xFFFFFED4` compare — the bike window is 300, not the
    // tracked/wheeled 350), planar compare, energy zero, not crashed. Action
    // gated on the mover's frameFlags=1: undo the dribble and return — the
    // bike sleep has NO contact-byte refresh [orig: Z -= slideDecay
    // @0x47976B; slideDecay >>= 1 @0x479777].
	if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 && m.wheel_rate_bam == 0 &&
			m.air_pitch_rate == 0 && m.air_roll_rate == 0 &&
			((veh.flags | veh.engine_flags) & (kEntityFlagInAir | 0x40u)) == 0 && m.crashed == 0 &&
			m.slide_z > -300 && m.slide_z < 0 && px == start_x && py == start_y &&
			m.spring_energy == 0) {
		pz -= m.slide_z;
		m.slide_z >>= 1;
		if (world.ai.for_handle(veh.handle) == nullptr || m.cmd_speed == 0)
			return;
	}

	vehicle_expire_contact_wake(world, veh);

	// ---- probe geometry [orig: @0x479960..0x479C20]: r = (box height >> 1)
	// - 0x4000 [orig: @0x47992A-region]; the two wheels on the FOOT
	// centerline at X = foot_x_hi - r / foot_x_lo + r, Z = box_z_lo + r
	// (per-wheel sinks zero); three spine probes on the BOX centerline at
	// X = box_x_lo + {L/4, 3L/4, L/2}, Z = box_z_lo + 2r; one mid-hull probe
	// at X = box_x_lo + L/2, Z = box_z_lo + 3r [orig: `box_z_lo -
	// ftol(4r * -0.75)` via flt_7C6F78 @0x479BC1]. Every radius is r
	// [orig: the plain fild/ftol round-trip @0x4799b7/@0x4799e7].
	const int32_t r = ((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000;
    const int32_t Lbox = traits.box_x_hi - traits.box_x_lo;
    const int32_t foot_ymid = (traits.foot_y_lo + traits.foot_y_hi) >> 1;
    const int32_t box_ymid =
            traits.box_y_lo + ((traits.box_y_hi - traits.box_y_lo) >> 1);
    // The two wheels ride their spring compression (+0x2D4 / +0x2D8, the
    // vehicle_suspension.cpp leg — the bike uses the quadratic/fast pair).
    const int32_t wheel_z = traits.box_z_lo + r;
    const int32_t probes_model[6][3] = {
        {traits.foot_x_hi - r, foot_ymid, wheel_z + m.wheel_comp[0]}, // front wheel
        {traits.foot_x_lo + r, foot_ymid, wheel_z + m.wheel_comp[1]}, // rear wheel
        {traits.box_x_lo + (Lbox >> 2), box_ymid, traits.box_z_lo + 2 * r},
        {traits.box_x_lo + ((3 * Lbox) >> 2), box_ymid, traits.box_z_lo + 2 * r},
        {traits.box_x_lo + (Lbox >> 1), box_ymid, traits.box_z_lo + 2 * r},
        {traits.box_x_lo + (Lbox >> 1), box_ymid, traits.box_z_lo + 3 * r},
    };

	VehicleEulerBasis basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	int32_t probes[6][3];
	for (int i = 0; i < 6; ++i) {
        int32_t rotated[3];
        basis.q22.rotate_point(probes_model[i], rotated);
        probes[i][0] = px + rotated[0];
        probes[i][1] = py + rotated[1];
        probes[i][2] = pz + rotated[2];
    }

	// The two contact passes retain the six-probe layout. Shared response applies sound and
	// momentum; the terrain-only quarter-speed cut scans probes 0 through 4, excluding the mid-
	// hull probe.
	// Witness sites: [orig: @0x479CB1, @0x47A2F6, @0x479C2B, @0x479E7C, @0x479C89, @0x479CD1,
	// @0x479D0F, @0x479DEB, @0x479E36]
	const int32_t soft = cos22_of_bam_x87(traits.max_slope);
	const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[6];
	const int32_t radii[6] = { r, r, r, r, r, r };
	EntityHandle hit_entity;
	const int32_t sev =
			plat_probe_pass(world, veh, probes, radii, soft, hard, forces, px, py, pz, &hit_entity);
	vehicle_contact_impact(world, veh, traits, sev, hit_entity, px, py, pz);
	if (sev == 1) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31);
	} else if (sev == 2) {
		m.speed -= m.speed >> ((traits.torque + 1) & 31);
	} else if (sev == 3) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31);
        int strongest = 0;
        int64_t best = -1;
        for (int i = 0; i < 5; ++i) {
            const int64_t sfx = forces[i].fx, sfy = forces[i].fy;
            const int64_t mag2 = sfx * sfx + sfy * sfy;
            if (mag2 > best) { best = mag2; strongest = i; }
        }
        const int64_t ddx = int64_t(probes[strongest][0]) - px;
        const int64_t ddy = int64_t(probes[strongest][1]) - py;
		if (!hit_entity.valid() && ddx * ddx + ddy * ddy > int64_t(0x8000) * 0x8000)
			m.speed = int32_t(m.speed * 0.25);
	}
	int32_t d[6];
    for (int i = 0; i < 6; ++i) d[i] = forces[i].fz;
    if (sev >= 1) {
        // Pass 2: the push sums probes 0..4 only — the mid-hull probe never
        // contributes to the planar separation [orig: the five-term fx/fy
        // sums @0x479ppp-region 5.., push @0x47A0AF..0x47A0BE].
        int64_t dX = 0, dY = 0;
        for (int i = 0; i < 5; ++i) { dX += forces[i].fx; dY += forces[i].fy; }
        for (int i = 0; i < 6; ++i) {
            probes[i][0] += int32_t(dX);
            probes[i][1] += int32_t(dY);
        }
        PlatProbeForce forces2[6];
		const int32_t sev2 =
				plat_probe_pass(world, veh, probes, radii, soft, hard, forces2, px, py, pz);
		if (sev2 != 0) {
			int64_t dX2 = 0, dY2 = 0;
            for (int i = 0; i < 5; ++i) {
                dX2 += forces2[i].fx;
                dY2 += forces2[i].fy;
            }
            for (int i = 0; i < 6; ++i) d[i] = (forces2[i].fz + d[i]) >> 1;
            dX = (dX2 + dX) >> 1;
            dY = (dY2 + dY) >> 1;
		}
		int32_t dx = int32_t(dX), dy = int32_t(dY);
		vehicle_contact_mass_share(world, veh, hit_entity, dx, dy, d, 6);
		px += dx;
		py += dy;
	}

	// Bike water entry uses the two-wheel mean without hysteresis and emits the local effect and
	// positioned sound once.
	// Witness sites: [orig: @0x479F7C, @0x479F90]
	const bool trail_was_water = (veh.flags & 0x8000u) != 0;
	if (world.env.water_z != 0) {
		if (((probes[0][2] + probes[1][2]) >> 1) >= world.env.water_z)
            veh.flags &= ~0x8000u;
        else
            veh.flags |= 0x8000u;
	}
	vehicle_trail_water_transition(veh, traits, trail_was_water);
	const int lowest_water_probe = probes[0][2] < probes[1][2] ? 0 : 1;
	vehicle_water_entry(world, veh, probes[lowest_water_probe][0], probes[lowest_water_probe][1],
			trail_was_water);

	vehicle_bike_lean(veh, traits, basis.q22.m[9] >> 6);

	// Wall contact sums only flagged probe forces before normalizing.
	// The upper hull probe has its own crew-kill/crash-request branch.
	// [orig: Entity_ProcessLightVehiclePhysics @0x479600]
	{
		bool any_contact = false;
		int64_t normal_sum[3] = {};
		for (const auto &force : forces) {
			if (!force.wall_contact)
				continue;
			any_contact = true;
			normal_sum[0] += force.fx;
			normal_sum[1] += force.fy;
			normal_sum[2] += force.fz;
		}
		const int64_t velocity[3] = { m.vel_x, m.vel_y, m.slide_z };
		int32_t normal[3], direction[3];
		q16_normalize(normal_sum, normal);
		q16_normalize(velocity, direction);
		const int32_t dot = io::bam_add(io::bam_add(q16_mul_rhu(normal[0], direction[0]),
												q16_mul_rhu(normal[1], direction[1])),
				q16_mul_rhu(normal[2], direction[2]));
		const int32_t speed = int32_t(std::min(2147418100.0,
				std::sqrt(double(velocity[0]) * velocity[0] + double(velocity[1]) * velocity[1] +
						double(velocity[2]) * velocity[2])));
		if (world.ai.is_authority && any_contact && dot < -57070) {
			if (forces[5].wall_contact && speed > 26370) {
				vehicle_kill_crash_occupants(world, veh);
				if (!m.crashed && (d[0] || d[1]))
					m.crash_request = 1;
			} else if (speed > 29300) {
				veh.health = 0;
				veh.last_attacker = {};
			} else if (speed > 21975) {
				vehicle_kill_crash_occupants(world, veh);
			} else if (speed > 19045) {
				veh.health = int16_t(veh.health - 20);
				if (veh.health < 0) {
					veh.health = 0;
					veh.last_attacker = {};
				}
			}
		}
		// Any planar contact above the threshold halves speed. [orig: @0x47A66E..0x47A683]
		if (any_contact && speed > 17580)
			m.speed >>= 1;
	}

	// ---- solve select [orig: the `!d_front && !d_rear` split @0x47A985].
	int32_t up_z16 = static_cast<int32_t>(basis.up[2] * io::kFp16OneD);
	const int32_t side_z16 = static_cast<int32_t>(basis.side[2] * io::kFp16OneD);
    // [orig: Entity_ProcessLightVehiclePhysics @ 0x479600]
    int32_t crash_depth = -1;
    if (m.crashed || up_z16 < 0 || m.settle_2f0)
        for (int i = 0; i < 5; ++i) crash_depth = std::max(crash_depth, d[i]);
    // The contact gate reads the previous rear-contact run; its tail increments later.
    // [orig: Entity_ProcessLightVehiclePhysics @ 0x47A90B..0x47A94E]
    m.grounded = up_z16 > 4096 && io::bam_abs(side_z16) < 40960 &&
            d[1] != 0 && m.crashed == 0 && io::bam_abs(m.light_rear_contact_ticks) > 1;
    if (m.grounded)
        m.bike_ground_contact_ticks = d[0] != 0 ? io::bam_add(m.bike_ground_contact_ticks, 1) : 0;
    if (!m.crashed) {
        if (m.wheelie_active && m.wheelie_request) {
            // Two upward force records on the front pair, rate 50, mode 3.
            // [orig: Entity_ProcessLightVehiclePhysics @ 0x47B1BC..0x47B25E]
            int32_t corners[4][3];
            light_bounding_quad(traits, basis, px, py, pz, corners);
            if (!m.chassis_contact_active) {
                for (int i : {0, 3}) {
                    auto &force = m.chassis_forces[i];
                    if (force.rate <= 0) {
                        for (int k = 0; k < 3; ++k) force.direction[k] = basis.q22.m[4*k+2] >> 6;
                        force.rate = 50; force.scratch = 3;
                    }
                }
            }
            vehicle_clear_chassis_forces(veh, corners, 1);
        } else vehicle_clear_chassis(m);
    }
	// ---- the suspension spring leg's bike legs (vehicle_suspension.h): the
    // live crash test over the spine probes [orig: @0x47B32D..0x47B375] and
    // the +100 front/rear sink growth with no latch terms [orig: @0x47AB36..
    //  0x47ABDC].
    const bool wheel_contact[4] = {d[0] != 0, d[1] != 0, true, true};
	vehicle_landing_damage(world, veh, traits, d, 5, up_z16);
	vehicle_suspension_bike_crash_test(
			veh, d[0] != 0, d[1] != 0, d[2] != 0 || d[3] != 0 || d[4] != 0);
	vehicle_suspension_grow_sinks(veh, wheel_contact, 2, kSinkGrowthBike,
                                  /*latch_gated=*/false, /*pre_gate_skip=*/false);
	vehicle_crush_damage(world, veh, traits, d + 2, up_z16);
	vehicle_crash_state(world, veh, traits, nullptr, up_z16);
	const bool both_wheels_off = d[0] == 0 && d[1] == 0;
	// The caller enables +0x2EF one tick after crash arming: immediately in
    // a wheel-contact branch, or on an off-wheel branch once a body probe
    // touches [orig: @0x47B37C..0x47B3FA / @0x47B838..0x47B84A].
    if (m.crashed != 0 && m.byte_2ef == 0) {
        bool body_contact = !both_wheels_off;
        for (int i = 0; i < 5 && !body_contact; ++i)
            body_contact = d[i] > 0;
        if (body_contact) m.byte_2ef = 1;
    }
    int32_t corner_adj[4] = {0, 0, 0, 0};
    if (both_wheels_off) {
		// When both wheels leave support, reset the rear-contact run, mark airborne and fit the
		// axle with the family spring/crash gates.
		// Witness sites: [orig: @0x47AA13, @0x47AC84]
		m.light_rear_contact_ticks = 0;
		m.grounded = false;
        world.vehicles.suspension_airborne_loop(veh, traits, 2, kSinkGrowthBike,
                                         corner_adj);
        if ((m.crashed && m.byte_2ef) || m.settle_2f0) {
            int32_t depth = -1;
            if (m.crashed || up_z16 < 0)
                for (int i = 0; i < 5; ++i) depth = std::max(depth, d[i]);
            pz = io::bam_add(pz, depth);
        }
        veh.flags |= kEntityFlagInAir;
        const bool just_armed =
                world.vehicles.suspension_arm(veh, /*eject_occupants=*/true);
        const bool falling = !just_armed &&
                light_fall_over_tick(world, veh, traits, basis, d, px, py, pz, corner_adj);
        if (!falling) {
            int32_t corners[4][3];
            light_bounding_quad(traits, basis, px, py, pz, corners);
            corners[0][2] = io::bam_add(corners[0][2], corner_adj[0]);
            corners[1][2] = io::bam_add(corners[1][2], corner_adj[1]);
            light_apply_frame(world, veh, just_armed ? basis.q22 : light_axle_frame(corners, basis.q22), true);
        }
        light_capture_launch(veh, basis, px, py, pz);
		vehicle_suspension_tick_tail(veh, traits);
		vehicle_contact_downhill_tail(veh, pz);
		m.contact_solved_once = true;
		m.plat_airborne_ticks = (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;

		return;
	}
    // Both wheels settle a released launch after the contact run reaches ten.
    // [orig: Entity_ProcessLightVehiclePhysics @ 0x47B5FD..0x47B63F]
    if (!m.crashed && d[0] && d[1] && m.bike_ground_contact_ticks >= 10 &&
            !m.wheelie_request && m.wheelie_active) {
        m.wheelie_active = 0;
        vehicle_clear_chassis(m);
    }
    if (m.wheelie_active && !m.crashed &&
            (up_z16 < 0 || (io::bam_abs(m.slide_z) > 20480 && (basis.q22.m[8] >> 6) > (io::kFp16OneInt / 2))))
        m.crash_request = 1;
    // A fast nose-down landing can also arm a crash outside wheelie mode.
    // [orig: Entity_ProcessLightVehiclePhysics @ 0x479600]
    if (!m.crashed && io::bam_abs(m.slide_z) > 16384 && (basis.q22.m[8] >> 6) < -(io::kFp16OneInt / 2))
        m.crash_request = 1;
	if (d[1] != 0)
		vehicle_capture_contact_direction(veh, basis);
	// ---- wheel contact: the 2-corner axle fit [orig: the grounded arm of
	// Entity_UpdateVehicleChassisOrientation @0x468B03..0x468D3B]. Corners
	// from the isSquare quad — ±half the inset foot length along the basis
	// forward row [orig: Entity_ComputeBoundingQuad @0x45B6E0 isSquare arm:
	// c0 = pos + 0.5*len*fwd, c1 = pos - 0.5*len*fwd] — lifted by the wheel
	// d's in the spring loop [orig: dest[2] += d0 / dest[5] += d1].
	veh.flags &= ~kEntityFlagInAir;
    // The bike's spring loop over its two wheel depths (the quadratic/fast
    // pair, the same kernels as the tracked solve; run with the tracked loop's
    // shape — vehicle_suspension.h names the bike's own loop a residual)
    // [orig: the light solve's Suspension_CompressWheelQuadratic @0x47B431 /
    //  @0x47BAD0 and OscillateWheelFast calls].
    world.vehicles.suspension_grounded_loop(veh, traits, 2, d, wheel_contact,
                                     kSinkGrowthBike, corner_adj);
    const int32_t half_len =
            ((traits.foot_x_hi - r) - (traits.foot_x_lo + r)) >> 1;
    int32_t cf[3], cr[3];
    for (int i = 0; i < 3; ++i) {
        const int32_t off = static_cast<int32_t>(
                (static_cast<int64_t>(half_len) *
                         static_cast<int32_t>(basis.fwd[i] * io::kFp16OneD) +
                 0x8000) >> 16);
        cf[i] = off;
        cr[i] = -off;
    }
    cf[0] += px; cf[1] += py; cf[2] += pz + d[0] + corner_adj[0];
    cr[0] += px; cr[1] += py; cr[2] += pz + d[1] + corner_adj[1];
    const bool just_armed =
            world.vehicles.suspension_arm(veh, /*eject_occupants=*/true);
	const bool falling = !just_armed &&
			light_fall_over_tick(world, veh, traits, basis, d, px, py, pz, corner_adj);
    if (!falling) {
        int32_t corners[4][3] = {};
        std::copy_n(cf, 3, corners[0]); std::copy_n(cr, 3, corners[1]);
        light_apply_frame(world, veh, just_armed ? basis.q22 : light_axle_frame(corners, basis.q22),
                m.crashed || m.wheelie_active || up_z16 < 0);
    }
    // Z select: crashed/no-up/parked ride the latch machine; the live leg
    // clamps slideDecay non-positive and adopts the fit Z — the mean of the
    // two lifted wheel corners [orig: `slideDecay = min(slideDecay, 0);
    // Position.Z = out.z` @0x47AF52..0x47AF60; out.z = (c0.z + c1.z) * 0.5
    // via flt_7C3B94].
    if (m.crashed == 0 && m.settle_2f0 == 0 && up_z16 > 0) {
        if (m.slide_z > 0) m.slide_z = 0;
        pz = static_cast<int32_t>(
                (int64_t(cf[2]) + cr[2]) / 2);
    } else {
        pz = io::bam_add(pz, crash_depth);
    }
    // Both-wheel landing absorbs half the fall [orig: `if (d0 && d1 &&
    // slideDecay < 0) slideDecay -= slideDecay >> 1` @0x47B0F1..0x47B103].
    if (d[0] != 0 && d[1] != 0 && m.slide_z < 0)
        m.slide_z -= m.slide_z >> 1;
    // ---- the contact byte [orig: @0x47A4CC..0x47A52B]: up.z > 4096, the
    // lean bound |right.z| < 40960, REAR wheel contact, and a rear-contact
    // run longer than one tick [orig: the +1 @0x47B739-region, reset in the
    // both-wheels-off branch; `|entity[1].pad_040[8]| <= 1` kills the byte].
    if (d[1] != 0) m.light_rear_contact_ticks += 1;
    if (m.crashed) m.grounded = false;
    light_capture_launch(veh, basis, px, py, pz);

	// The crash latch armed at the chassis-call site above (the bike seed
	// ejects its riders) [orig: @0x468B00..0x468B3B]. Wheels back in contact
	// reset the sinks (the tracked form, both wheels as the pair); the tail
	// clears the per-tick request [orig: @0x47C0B6].
	vehicle_suspension_post_contact(veh, wheel_contact, 2, up_z16);
    vehicle_suspension_tick_tail(veh, traits);
	vehicle_contact_downhill_tail(veh, pz);
	m.contact_solved_once = true;
	m.plat_airborne_ticks = (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;
}

} // namespace detail
} // namespace opennova::world
