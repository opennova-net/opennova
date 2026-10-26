// The watercraft mover: the platform solve (VehicleSystem::watercraft_platform_solve), the
// predicting client's tick (watercraft_client_tick) and the motor (tick_watercraft_motor).
// Split out of vehicle_motor.cpp by responsibility (the oversize-TU ratchet, A2-43); the
// shared platform-solve helpers are vehicle_motor_detail.h's.

#include <runtime/world/vehicle_system.h>
#include <runtime/world/vehicle_motor.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/io/bam.h>

#include "vehicle_motor_detail.h"

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/collision_detail.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/geom.h>
#include <runtime/world/local_player.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

namespace opennova::world {

using namespace detail; // the shared platform-solve helpers, unqualified as in vehicle_motor.cpp

void VehicleSystem::watercraft_platform_solve(Entity &veh, const VehicleTraits &traits) {
    World &world = world_;
    Entity::VehicleMotorState &m = veh.veh;
    // A hull with no box extents runs no platform solve: its afloat latch,
    // contact byte and attitude stay as they stand. A model-less hull's
    // null-model branch writes the contact byte (+0x2F2) from the stack word
    // esp+0x2C (the row-2 Z the wake leg's extraction fills; stale on the paths
    // past both extractions) and, on the authority, Flags 0x10 from +0x2F0:
    // Q7-12 / Q7-33 (a), PERMANENT, the whole branch, a deliberate fix of
    // retail's unchecked read, leaves both; a model with no boxes keeps the level
    // hull and chase-owned Z.
    // [orig: Entity_ProcessPlatformPhysics @0x481870 (@0x481A85..0x481AC1 ->
    //  @0x481A06..0x481A22; the extractions @0x4819D4 / @0x481A54)]
    if (traits.box_z_hi == traits.box_z_lo || traits.box_y_hi == traits.box_y_lo)
        return;

    const int32_t W = world.env.water_z;
    int32_t px = veh.position.x.q16;
    int32_t py = veh.position.y.q16;
    int32_t pz = veh.position.z.q16;

	// [orig: Entity_ProcessPlatformPhysics @0x4818B9..0x481A5C]
	if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 && m.wheel_rate_bam == 0 &&
			m.air_pitch_rate == 0 && m.air_roll_rate == 0 &&
			((veh.flags | veh.engine_flags) & (kEntityFlagInAir | 0x40u)) == 0 &&
			m.slide_z > -500 && m.slide_z < 0 && veh.saved_live_valid &&
			px == veh.saved_live_pos[0] && py == veh.saved_live_pos[1] &&
			io::bam_abs(io::bam_sub(pz, veh.saved_live_pos[2])) < 500 &&
			std::all_of(
					std::begin(m.plat_acc), std::end(m.plat_acc),
					[](int32_t value) { return value == 0; })) {
		pz = io::bam_sub(pz, m.slide_z);
		m.slide_z >>= 1;
		vehicle_rest_state(world, veh, VehicleFamily::Watercraft);
		veh.position.z.q16 = pz;
		return;
	}
	vehicle_expire_contact_wake(world, veh);

	// The every-call tuning clamps [orig: @0x481ACC..0x481BA3]:
	const int32_t t_pitch = std::clamp(traits.pitch_lift, 0, 10);
	const int32_t t_pitch_vel = std::clamp(traits.pitch_lift_vel, 0, 10);
    const int32_t t_bob = std::clamp(traits.bob, 0, 10);

    // ---- §3 probe geometry. Probe springs +0x2D4.. are provably zero for
    // pure boats (blockers §4). q = beam/4.
    const int32_t q = (traits.box_y_hi - traits.box_y_lo) >> 2;
    if (q <= 0) return;
    m.plat_solve_valid = true;
    const int32_t zb = traits.box_z_lo + q;
    // 4 corner probes + 3 midline probes (model space) [orig: @0x481CB4..].
    int32_t rm = std::min(((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000,
                          ((traits.box_y_hi - traits.box_y_lo) >> 1) - 0x1000);
    if (rm < 0x2000) rm = 0x2000;
    const int32_t lx = traits.box_x_hi - traits.box_x_lo;
    const int32_t ymid = traits.box_y_lo + ((traits.box_y_hi - traits.box_y_lo) >> 1);
    const int32_t zt = traits.box_z_hi - rm;
    const int32_t probes_model[7][3] = {
        {traits.foot_x_hi - q, traits.foot_y_hi - q, zb}, // p0 bow-A
        {traits.foot_x_hi - q, traits.foot_y_lo + q, zb}, // p1 bow-B
        {traits.foot_x_lo + q, traits.foot_y_lo + q, zb}, // p2 stern-B
        {traits.foot_x_lo + q, traits.foot_y_hi - q, zb}, // p3 stern-A
        {traits.box_x_lo + (lx >> 2), ymid, zt},          // p4
        {traits.box_x_lo + ((3 * lx) >> 2), ymid, zt},    // p5 [(3*lx)>>2 verbatim]
        {traits.box_x_lo + (lx >> 1), ymid, zt},          // p6
    };
    const int32_t radii[7] = {q, q, q, q, rm, rm, rm};
    const int32_t v210 = -(traits.box_z_lo + q); // corner-0 model-Z, negated (§3)
    // Half-extents of the lever rectangle (§3 tail).
    const int32_t hb = (traits.foot_y_hi - q) - (traits.foot_y_lo + q);
    const int32_t hl = (traits.foot_x_hi - q) - (traits.foot_x_lo + q);

    // World transform: the pose matrix from {Pos, Yaw, Pitch, Roll} — the
    // boat's live attitude rides air_pitch_bam/air_roll_bam (the shared
    // attitude fields the sim mirrors to the row).
	const VehicleEulerBasis basis =
			vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
    // The probes ride the head matrix, the hull's effective scale included.
    // [orig: @0x481BEE..0x481C45]
	const CollisionMatrix head = vehicle_solve_matrix(veh);
    int32_t probes[7][3];
	place_probes(head, probes_model, px, py, pz, probes);
	// The occupant matrix (isUnderwater in the IDB): the head's rows extracted and set back, not
	// normalized. The fits and the euler extraction after them read it.
	// [orig: @0x481C48..0x481CAF; the readers @0x483BBE / @0x483D00 / @0x483F2B, @0x483BD0 /
	//  @0x483D1C / @0x483F3D]
	CollisionMatrix occupant = vehicle_matrix_rows(head);

    // ---- §4/§6 first force pass + severity response (plat_probe_pass).
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
	EntityHandle hit_entity;
	const int32_t sev =
			plat_probe_pass(world, veh, ProbeForcePass::ComputeCollisionForces, probes, radii, soft,
					hard, forces, px, py, pz, &hit_entity);
	vehicle_contact_impact(world, veh, traits, sev, hit_entity, px, py, pz);
	// The decay per severity [orig: @0x4821E7 (1), @0x4822A4 (2), @0x4822C9 (3)]. The shared
	// contact fold applies authority damage, sound and momentum. The quarter-speed cut requires
	// a terrain-only hit beyond the hull [orig: flt_7C333C @0x4826EB]. The original yaw-kick arm
	// is unreachable.
	// Witness sites: [orig: @0x482546, @0x4825DD, @0x4825E3, @0x48262D, @0x4826EB]
	contact_speed_response(m, traits, sev, hit_entity, forces, probes, 7, px, py);

	// ---- §7 position push + second pass (severity >= 1 only). zc[] mirrors
	// the SHARED force buffer the grounded leg reads [orig: §10-A
	// @0x483D5C..0x483F04]: pass 1 fills it; the sev>=1 second pass re-zeroes
	// and refills it; when §7 is skipped (sev 0, climbable contact) it still
	// holds the PASS-1 forces -- the saved zf[] carries the averaged values.
	int32_t zf[7], zc[7];
    for (int i = 0; i < 7; ++i) { zf[i] = forces[i].fz; zc[i] = forces[i].fz; }
    if (sev >= 1) {
        int64_t dX = 0, dY = 0;
        for (int i = 0; i < 7; ++i) { dX += forces[i].fx; dY += forces[i].fy; }
        for (int i = 0; i < 7; ++i) { probes[i][0] += int32_t(dX); probes[i][1] += int32_t(dY); }
        PlatProbeForce forces2[7];
		const int32_t sev2 =
				plat_probe_pass(world, veh, ProbeForcePass::ComputeCollisionForces, probes, radii,
						soft, hard, forces2, px, py, pz);
		for (int i = 0; i < 7; ++i)
			zc[i] = forces2[i].fz;
		if (sev2 != 0) {
			int64_t dX2 = 0, dY2 = 0;
            for (int i = 0; i < 7; ++i) { dX2 += forces2[i].fx; dY2 += forces2[i].fy; }
            for (int i = 0; i < 7; ++i) zf[i] = (forces2[i].fz + zf[i]) >> 1;
            dX = (dX2 + dX) >> 1;
            dY = (dY2 + dY) >> 1;
		}
		int32_t dx = int32_t(dX), dy = int32_t(dY);
		vehicle_contact_mass_share(world, veh, hit_entity, dx, dy, zf, 6);
		px += dx;
		py += dy; // Z-sum always 0 [orig: @0x482A86..0x482A8C]
	}

	// ---- §5 bob/lift parameters.
	const double F = double(q);
    const int32_t amp = std::min(int32_t(F * 0.0625), 352);
    int32_t floatH, liftHi, liftLo, planeSpd, pitchThr;
    if (traits.mass <= 1) { // LIGHT boat [orig: @0x482105]
        floatH = int32_t(0.65 * F);
        liftHi = t_pitch_vel * 100;
        liftLo = 250;
        planeSpd = 10000;
        pitchThr = int32_t(double(t_pitch) * 0.1 * 4096.0);
    } else { // HEAVY [orig: @0x48221B]
        floatH = q;
        liftHi = int32_t(double(t_pitch_vel) * F * 0.004);
        liftLo = liftHi >> 1;
        planeSpd = 20000;
        // Heavily rolled: the head's row 1 z, its extract's word.
        // [orig: `mov eax, [esp+..+var_244]` (row 1 z of the @0x481C55 extract) @0x482259]
        if (io::bam_abs(head.m[9] >> 6) > 0x2000) {
            pitchThr = int32_t(double(t_pitch) * 0.1 * 409.6);
            liftHi = 250;
        } else {
            pitchThr = int32_t(double(t_pitch) * 0.1 * 4096.0);
        }
    }
    const int32_t dipExit = int32_t(double(pitchThr) * (1.0 - 0.1 * double(t_bob)));

    // ---- §8 water leg: per-corner submersion, draft, the afloat flag.
    int32_t sub_k[4], cz_k[4];
    for (int k = 0; k < 4; ++k) {
        cz_k[k] = probes[k][2];
        sub_k[k] = W + q - cz_k[k];
    }
    const int32_t avg = (cz_k[0] + cz_k[1] + cz_k[2] + cz_k[3]) >> 2;
    int32_t draft = avg;
	// [orig: Entity_ProcessPlatformPhysics @ 0x481870, callback test @ 0x482B6E]
	if (m.plat_afloat)
		draft = traits.amphibian ? avg - (q >> 1) : int32_t(double(avg) - 0.9 * F);
	// Zero water height is the embedder no-water sentinel. Amphibians and boats retain their
	// separate draft hysteresis.
	if (W == 0 || draft + v210 >= W) {
		m.plat_afloat = false; // [orig: @0x482DB7]
	} else {
		// The first submerged pad emits the local splash/wake and queues the positioned sound at
		// the water plane. Flags 0x8000 latches this edge.
		// Witness sites: [orig: @0x482BB9, @0x482C9D, @0x50a1b0]
		m.plat_afloat = true; // [orig: @0x482CA5]
	}
	const bool trail_was_water = (veh.flags & 0x8000u) != 0;
	veh.flags = m.plat_afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);
	detail::vehicle_trail_water_transition(veh, traits, trail_was_water);
	int water_pad = 0;
	for (int k = 1; k < 4; ++k)
		if (cz_k[k] < cz_k[water_pad])
			water_pad = k;
	detail::vehicle_water_entry(
			world, veh, probes[water_pad][0], probes[water_pad][1], trail_was_water);

	// ---- §9 lever corners around the CURRENT pose + machines, off the rows of the matrix the
	// solve rebuilds unscaled from the euler words (vehicle_platform_corners). The crash state's
	// righting below replaces the rows but not these corners.
	// [orig: Math_BuildFixedPointMatrixFromEulerAngles (the call @0x482DCA) and its extracts
	//  @0x482DDC..0x482E2C; no corner build after the righting @0x4835EF / @0x483642]
	const CollisionMatrix &raw = basis.q22;
	int32_t c[4][3];
	vehicle_platform_corners(raw, hb, hl, px, py, pz, c);
	// The crash state reads that matrix's row 2 z; a righting replaces the occupant, and the rows
	// read from here on are then its extracts.
	// [orig: `mov ecx, [esp+..+var_25C]` @0x483467; Entity_BuildOrientationFromVectors @0x458DF0
	//  (the call @0x4835EF) / Entity_RebuildOrientationMatrixFromAxes @0x4632E0 (the call
	//  @0x483642) into isUnderwater, the re-extracts @0x483601..0x483625 / @0x483654..0x483678]
	const VehicleEulerBasis rows =
			vehicle_crash_state(world, veh, traits, nullptr, raw.m[10] >> 6, &occupant)
			? vehicle_basis_from_matrix(occupant)
			: basis;

	// Accumulator ramp [orig: @0x483680..0x4836E6].
	for (int k = 0; k < 4; ++k)
		if (zf[k] <= 0 && sub_k[k] <= floatH) m.plat_acc[k] += 250;
    // Bow lift / planing / porpoise [orig: @0x4836EC..0x483891]. The command
    // register is brain[136] (brain+0x220) — the COMMANDED speed. On a remote
    // boat the mirror makes it equal the received register, but the local
    // driver's [136] is the §1.8 current/received average, and the solve reads
    // that averaged command, not the raw wire value.
    const int32_t cmd = m.cmd_speed;
	// The forward z word: the rows' (the righting's, else the rebuild's), re-extracted from the
	// rebuild's matrix when the bow arm runs, whose whole body afloat gates.
	// [orig: Math_ExtractRow0FromFixedPoint22 (the call @0x483723) from `matrix` under the speed
	//  and lift gates @0x4836EC..0x483710, the afloat test @0x48372B, the reads of var_250
	//  @0x4837BF / @0x4839D7 / @0x483B04, the cmd test @0x483741 -> @0x483881]
	int32_t fwd_z_now = rows.q22.m[8] >> 6;
    if (m.speed > 0 && liftLo > 0 && liftHi > 0) {
		fwd_z_now = raw.m[8] >> 6;
		if (m.plat_afloat && cmd > 0) {
            m.plat_acc[0] = m.plat_acc[1] = 0;
            m.plat_planing = true;
            if (!m.plat_porpoise) {
                if (fwd_z_now > pitchThr) {
                    // The hull's XY travel since the mover's entry stamp, not its speed
                    // register: the integration and the push above moved it by its
                    // velocity words and the contact share, so the length of
                    // savedLivePose - Position (capped at flt_7C19E0, truncated) / 293
                    // against (cmd / 293) * 0.5 truncated latches the dip
                    // (D-NET-NEW-L6-41).
                    // [orig: the travel @0x48376B..0x4837A1, the cap flt_7C19E0
                    //  (2147418112.0f) @0x4837A3..0x4837B6, the command half (flt_7C3B94
                    //  0.5f) @0x4837D2..0x48380B, the travel's ftol and /293 @0x48380D..0x483825,
                    //  `jle` @0x483829, the latch @0x48382B; Entity_UpdateWatercraftPhysics
                    //  @0x48D480 - the entry stamp @0x48D4AC..0x48D4C5, the integration
                    //  @0x48ECA8..0x48ECC6]. A row no mover stamped reads no travel.
                    const double tx = veh.saved_live_valid
                            ? double(io::bam_sub(veh.saved_live_pos[0], px)) : 0.0;
                    const double ty = veh.saved_live_valid
                            ? double(io::bam_sub(veh.saved_live_pos[1], py)) : 0.0;
                    const int32_t travel =
                            int32_t(std::min(std::sqrt(ty * ty + tx * tx), 2147418112.0));
                    if (travel / 293 > int32_t(double(cmd / 293) * 0.5)) m.plat_porpoise = true;
                } else {
                    const int32_t lift = (m.speed >= planeSpd) ? liftHi : liftLo;
                    c[0][2] += lift;
                    c[1][2] += lift; // bow rises
                }
            } else {
                if (fwd_z_now < dipExit) m.plat_porpoise = false;
                else { c[0][2] -= liftHi; c[1][2] -= liftHi; }
            }
        } else if (m.plat_afloat) {
            m.plat_planing = false;
        }
    }
	// Post-machine adjustments [orig: @0x483893..0x48398C].
	if (m.plat_planing) {
		// The side z word: the rows' row 1 z. [orig: `mov edi, [esp+..+var_244]` @0x48389A]
		if (m.plat_afloat && m.speed > 0x2000) {
			vehicle_boat_lean(veh, traits, rows.q22.m[9] >> 6);
			m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
		} else {
			const int32_t error = rows.q22.m[9] >> 6;
			m.byte_2ef = 0;
			m.air_roll_rate = io::bam_abs(error) > 240 ? (error < 0 ? -2386239 : 2386092) : 0;
			m.part_spin.angle = 0;
			m.gear_phase = 0;
		}
	} else {
		if (!m.plat_at_rest) {
			for (int k = 0; k < 4; ++k)
				c[k][2] -= 500;
			m.byte_2ef = 0;
			m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
		}
		m.part_spin.angle = 0;
		m.gear_phase = 0;
	}

	// ---- §10 solve select.
	bool any_ground = false;
	for (int k = 0; k < 4; ++k) any_ground |= zf[k] > 0;
    PlatFit fit;
    if (any_ground) {
        // A. Grounded: airborne cleared HERE and only here.
        veh.flags &= ~kEntityFlagInAir; // [orig: @0x483D55]
        for (int k = 0; k < 4; ++k) {
            int32_t lift;
            if (sub_k[k] > floatH) {
                // Deep + grounded: the raw call-#2 force when the probe force
                // exceeds the submersion, else raise to the waterline
                // [orig: @0x483D5C..0x483F04 reads zc_k].
                lift = (zf[k] > sub_k[k]) ? zc[k] : std::abs(sub_k[k] - floatH);
                m.plat_acc[k] = 0;
            } else if (zf[k] != 0) {
                lift = zc[k];
                m.plat_acc[k] = 0;
            } else {
                lift = 250 - m.plat_acc[k];
            }
            c[k][2] += lift;
        }
        m.slide_z = 0; // [orig: @0x483F25]
		vehicle_boat_suspension_fit(world, veh, traits, occupant, c, fit, px, py, pz);
		pz = fit.z_avg;
		m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
	} else {
        // At-rest bob arm [orig: @0x4839C5..0x483A29].
        int32_t floatH_eff = floatH;
        // [orig: |var_250| < 5 @0x4839D7..0x4839E3, |edi| (var_244) < 5 @0x4839E5..0x4839EF]
        if (m.speed < 100 && m.plat_afloat && io::bam_abs(fwd_z_now) < 5 &&
                io::bam_abs(rows.q22.m[9] >> 6) < 5) {
            if (!m.plat_at_rest) {
                m.plat_at_rest = true;
                m.plat_bob_phase = 4.71f; // 3pi/2 [orig: flt_7C6F84]
            }
            m.plat_bob_phase += 0.03488888964056969f; // [orig: flt_7C6EB4]
            const double f =
                std::min(1.0, (std::sin(double(m.plat_bob_phase)) + 1.0) * 0.5);
            const int32_t inc = int32_t(double(amp) * f);
            for (int k = 0; k < 4; ++k) c[k][2] += inc;
            floatH_eff = floatH - inc;
        } else {
            m.plat_at_rest = false;
        }
        bool any_deep = false;
        for (int k = 0; k < 4; ++k) any_deep |= sub_k[k] > floatH_eff;
        if (any_deep) {
            // B. Buoyancy push-up.
            if (m.slide_z < 0) m.slide_z = 0; // [orig: @0x483C15]
            for (int k = 0; k < 4; ++k) {
                int32_t lift;
                if (sub_k[k] >= floatH_eff) {
                    lift = std::abs(sub_k[k] - floatH_eff);
                    m.plat_acc[k] = 0;
                } else {
                    lift = std::min(0, 250 - m.plat_acc[k]);
                }
                c[k][2] += lift;
            }
			vehicle_boat_suspension_fit(world, veh, traits, occupant, c, fit, px, py, pz);
			pz = fit.z_avg;
		} else {
			// C. Settled / airborne.
            if (m.plat_afloat) {
                int lowest = 0, second = 1;
                for (int k = 1; k < 4; ++k)
                    if (c[k][2] < c[lowest][2]) lowest = k;
                second = lowest == 0 ? 1 : 0;
                for (int k = 0; k < 4; ++k)
                    if (k != lowest && c[k][2] < c[second][2]) second = k;
                if (cmd <= 0) {
                    m.plat_acc[lowest] = m.plat_acc[second] = 0;
                    if (fwd_z_now < 100)
                        m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] =
                                m.plat_acc[3] = 0;
                }
                for (int k = 0; k < 4; ++k)
                    c[k][2] += std::min(0, 250 - m.plat_acc[k]);
            } else {
                veh.flags |= kEntityFlagInAir; // [orig: @0x483B98]
            }
            // The wheeled solver's settled/airborne fit: same 4-normal fit;
            // Z = the average of the ABOVE-ground corners, rise-clamped
            // +0x2000/tick (blockers §3).
			vehicle_suspension_fit(world, veh, occupant, c, nullptr, fit, px, py, pz);
			if (m.plat_afloat) {
				// The wheeled solver's settled-fit Z: the average of the
				// ABOVE-ZERO corners only, rise-clamped +0x2000/tick
				// (blockers §3).
				int32_t new_z = fit.positive_z_avg;
                if (new_z > pz + 0x2000) new_z = pz + 0x2000;
                pz = new_z;
			}
		}
	}
	// [orig: @0x4839EF..0x483FA9] crashed hulls also adopt solved yaw.
	if (m.crashed)
		m.yaw_bam = fit.yaw_bam;
	m.air_pitch_bam = fit.pitch_bam;
	m.air_roll_bam = fit.roll_bam;
	// Airborne tick counter [orig: @0x483FAF/0x483FC8].
    m.plat_airborne_ticks =
            (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;

    veh.position.x.q16 = px;
    veh.position.y.q16 = py;
    veh.position.z.q16 = pz;
}

namespace {

bool watercraft_has_platform_geometry(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           ((traits.box_y_hi - traits.box_y_lo) >> 2) > 0;
}

// Establish only the platform water latch for a newly promoted client hull.
// The full solve's water decision depends on the four transformed corner
// heights but not on its later collision/bob mutations, so this gives the first
// mover tick the state a preceding retail platform frame would have supplied
// without double-running gravity, accumulators, or the attitude fit.
void watercraft_seed_platform_latch(World &world, Entity &veh,
                                    const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    const int32_t q = (traits.box_y_hi - traits.box_y_lo) >> 2;
    if (q <= 0) return;
    const int32_t zb = traits.box_z_lo + q;
    const int32_t corners[4][3] = {
        {traits.foot_x_hi - q, traits.foot_y_hi - q, zb},
        {traits.foot_x_hi - q, traits.foot_y_lo + q, zb},
        {traits.foot_x_lo + q, traits.foot_y_lo + q, zb},
        {traits.foot_x_lo + q, traits.foot_y_hi - q, zb},
    };
    const int32_t pos[3] = {veh.position.x.q16,
                            veh.position.y.q16,
                            veh.position.z.q16};
    const int32_t origin[3] = {};
    const CollisionMatrix basis = collision_matrix_from_euler(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam, origin);
    int32_t corner_z[4];
    for (int i = 0; i < 4; ++i) {
        int32_t rotated[3];
        basis.rotate_point(corners[i], rotated);
        corner_z[i] = pos[2] + rotated[2];
    }
    const int32_t avg =
            (corner_z[0] + corner_z[1] + corner_z[2] + corner_z[3]) >> 2;
    const int32_t v210 = -(traits.box_z_lo + q);
    // The platform solve's afloat decision is hysteretic: once afloat, the
    // boat-form draft is 0.9*beam-quarter below the corner average. A promoted
    // client row has lost that prior latch, so reconstruct the basin it would
    // already occupy instead of evaluating the dry-entry arm at the waterline.
    const int32_t afloat_draft =
            static_cast<int32_t>(double(avg) - 0.9 * double(q));
    // The solve's compare reads the water word raw (plat_water_flag).
    m.plat_afloat = afloat_draft + v210 < world.env.water_z;
    m.plat_solve_valid = true;
	const bool trail_was_water = (veh.flags & 0x8000u) != 0;
	veh.flags = m.plat_afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);
	detail::vehicle_trail_water_transition(veh, traits, trail_was_water);
}

} // namespace

static void watercraft_motor_core(World &world, Entity &veh,
                                  const VehicleTraits &traits);

void VehicleSystem::watercraft_client_tick(Entity &veh, const VehicleTraits &traits) {
	if (traits.physics == 0) {
		tick_simple_motor(veh, traits, true);
		return;
	}
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	if (!m.net_predicted) return;
	if (!m.yaw_seeded) {
		// [orig: Entity_UpdateWatercraftPhysics @0x48D4B2 reads the row's Yaw,
		//  unmoved = the spawn form, Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
		m.yaw_bam = veh.heading;
		m.yaw_seeded = true;
	}
	// A client row the class init never brained jumps from the stamp (the pass
	// stamped it ahead of this call) to the matrix-build tail: no claimant edge,
	// chase, motion or sound. [orig: Entity_UpdateWatercraftPhysics `mov ebp,
	//  [esi+64h]` @0x48D499, the stamp @0x48D4AC..0x48D4E9, `jz` @0x48D4EF -> the
	//  tail @0x48EF50..0x48EF63]
	if (world.ai.for_handle(veh.handle) == nullptr) {
		veh.engine_flags |= kEntityFlagMatrixBuilt;
		return;
	}
	update_engine_sound(veh, traits);

	vehicle_refresh_ground_link(world, veh, traits);
	vehicle_follow_carrier(world, veh);

	// ---- 1. Per-record chase (the §5.38e vehicle template) on the world pose.
	vehicle_client_chase(veh, VehicleChaseFamily::Plain, 0);
	// A hull with no platform geometry runs no platform solve, so its afloat
	// latch stays as it stands (see watercraft_platform_solve).
	if (watercraft_has_platform_geometry(traits) && !m.plat_solve_valid) {
		// An exact-handle wire-materialized joiner hull has no earlier local frame, but the mover
		// consumes the PREVIOUS platform solve's afloat/attitude state. Seed
		// that state before its first thrust/drag pass instead of treating a
		// resolved floating hull as landed for one frame.
		watercraft_seed_platform_latch(world, veh, traits);
	}

	// Remote prediction mirrors the received command registers; local-driver reconciliation
	// retains the authored steering input.
	// Witness sites: [orig: @0x48DDD4, @0x48DDF4, @0x490C9E]
	if (Entity *local_driver = resolve_local_vehicle_controller(world, veh, traits)) {
		// The boat parks on the dead bit alone [orig: @0x48DFB6..0x48DFBA].
		if (((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0) {
			vehicle_park_inputs(veh);
		} else {
			stage_player_vehicle_input(world_, veh, *local_driver, traits);
			// Retail reconciles only longitudinal command on the controlling
			// client; its steer target stays owned by current local LOOK/input.
			m.cmd_speed = io::bam_sar(
                    io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
		}
	} else {
		m.cmd_speed = m.net_recv_speed;
		m.steer_target_bam = m.net_recv_steer_bam;
	}

	watercraft_motor_core(world, veh, traits);
	// The client runs the same mover to the same matrix-build tail, which sets Flags
	// 0x20000 (kEntityFlagMatrixBuilt, homed on engine_flags) as the authority's does.
	// [orig: Entity_UpdateWatercraftPhysics @0x48EF50..0x48EF63]
	veh.engine_flags |= kEntityFlagMatrixBuilt;
}

// Blocks 12..19 of the cbot mover — the steer integrator through the yaw apply.
// ONE sequence in the original, executed by every machine that runs the function
// past the input gate: the authority (with the occupant/AI/parked staging done),
// the driver's client (local prediction), and remote clients (register mirror).
// Extracted verbatim from the 2026-07-31 client-subset port; the authority tick
// below stages its inputs and runs the same core.
// [orig: Entity_UpdateWatercraftPhysics @0x48D480, blocks @0x48E82C..0x48ED76]
static void watercraft_motor_core(World &world, Entity &veh,
                                  const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    int32_t px = veh.position.x.q16;
    int32_t py = veh.position.y.q16;
    int32_t pz = veh.position.z.q16;

    // ---- 3. Steer/rudder integrator [orig: @0x48E82C..0x48E926].
    {
        const int32_t turn_rate = traits.turn_rate;
        // Retail reads ONLY itemDef waterSpeed here; a zero pins the speed
        // fraction at 0 so the effective turn rate stays at min_rate — no
        // player_speed fallback exists [orig: the jz to the f=0 arm @0x48E844].
        const int32_t water_spd = traits.water_speed;
        int32_t min_rate = io::bam_sar(turn_rate, 2);
        if (traits.turn_rate2 != 0) min_rate = traits.turn_rate2;
        int32_t f = 0;
        if (water_spd != 0) {
            f = 0x10000 - static_cast<int32_t>(
                    (static_cast<int64_t>(m.speed) * 0x10000) / water_spd);
        }
        if (f < 0) f = 0;
        // No upper clamp — a reversing hull over-rotates, witnessed absent.
        const int32_t eff = io::bam_add(min_rate, static_cast<int32_t>(
                (static_cast<int64_t>(io::bam_sub(turn_rate, min_rate)) * f +
                        0x8000) >> 16));
        int32_t delta = io::bam_sar(io::bam_add(
                io::bam_sub(m.steer_target_bam, m.yaw_bam), 32), 6);
        if (delta > eff) delta = eff;
        const int32_t neg_eff = io::bam_sub(0, eff);
        if (delta < neg_eff) delta = neg_eff;
        const int32_t steer_error = io::bam_sub(
                io::bam_sub(4, bam_shl_wrap(delta, 5)), m.steer_state);
        m.steer_state = io::bam_add(
                m.steer_state, io::bam_sar(steer_error, 3));
        // Airborne, non-afloat hulls retain their existing yaw rate; grounded
        // or floating hulls recompute from the rudder state.
        // [orig: @0x48E8E3..0x48E920]
        if ((veh.flags & kEntityFlagInAir) == 0 || m.plat_afloat) {
            const int32_t neg_speed = io::bam_sub(0, m.speed);
            m.wheel_rate_bam = static_cast<int32_t>(
                    (static_cast<int64_t>(neg_speed) *
                             io::bam_sar(m.steer_state, 2) + 0x8000) >> 16);
        }
    }

    // ---- 4. Thrust [orig: @0x48E926..0x48EA12].
    int32_t vertical_thrust = 0;
    {
        const int32_t cmd = m.cmd_speed;
        if (cmd == 0) {
            if (io::bam_abs(m.vel_x) < 384) m.vel_x = 0;
            if (io::bam_abs(m.vel_y) < 384) m.vel_y = 0;
        } else {
            const int32_t a = io::bam_abs(cmd);
            int32_t acc = io::bam_add(
                    traits.acceleration,
                    io::bam_add(io::bam_sar(a, 8), io::bam_sar(a, 7)));
            const int32_t accel = cmd >= 0 ? std::min(acc, cmd)
                                           : std::max(io::bam_sub(0, acc), cmd);
            // Retail consumes the full Euler matrix from the PREVIOUS platform
            // solve. Roll contributes the capsize up[2] gate; pitch tilts the
            // forward thrust into X/Y/Z. The new solve runs later in this tick,
            // after position integration [orig: @0x48E972..0x48E9FF].
            const VehicleEulerBasisQ16 basis = vehicle_euler_basis_q16(
                    m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
            if (basis.up_z > 0) {
                m.vel_x += static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_x +
                                0x8000) >> 16);
                m.vel_y += static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_y +
                                0x8000) >> 16);
                vertical_thrust = static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_z +
                                0x8000) >> 16);
                // The wheel phase is this block's inline tail — a capsized
                // (up.z <= 0) or unpowered hull advances no phase [orig:
                // `mov edx,[ebp+220h]; shl edx,0Dh; add [esi+2B8h],edx`
                // @0x48E9F0..0x48E9F9, inside the `cmp var_98(up.z), 0; jle`
                // gate @0x48E9A1..0x48E9A6 and the `cmd == 0` skip @0x48E92E].
                m.wheel_phase = watercraft_wheel_phase_step(m.wheel_phase, cmd);
            }
        }
    }

    // ---- 5. Drag / slip / keel [orig: @0x48EA14..0x48EBB3, FPU-reconstructed].
    {
        m.vel_x -= m.vel_x >> 6;
        m.vel_y -= m.vel_y >> 6;
        const int32_t vx = m.vel_x, vy = m.vel_y;
        const int32_t vel_heading = bam_of_atan2(double(vy), double(vx));
        const int32_t slip = io::bam_sub(m.yaw_bam, vel_heading);
        const int32_t s22 = sin22_of_bam_x87(slip);
        const int32_t c22 = cos22_of_bam_x87(slip);
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
        const int32_t beam = io::bam_add(m.yaw_bam, 0x3FFFFFC0);
        const int32_t bs22 = sin22_of_bam_x87(beam);
        const int32_t bc22 = cos22_of_bam_x87(beam);
        m.vel_x += static_cast<int32_t>(
                (static_cast<int64_t>(lateral >> 5) * bc22) >> 22);
        m.vel_y += static_cast<int32_t>(
                (static_cast<int64_t>(lateral >> 5) * bs22) >> 22);
        if (along > m.cmd_speed) { // overspeed shed
            m.vel_x -= m.vel_x >> 6;
            m.vel_y -= m.vel_y >> 6;
        }
    }

    // ---- 6. Contact drags [orig: @0x48EBB5..0x48ECA6]. This tick consumes
    // the prior platform solve's authoritative afloat latch [orig: test
    // Flags 0x8000 @0x48EBB5]. NOT afloat (landed AND airborne): planar
    // sheds + yaw-rate shed + gravity slideDecay -= 167 [orig:
    // @0x48EBD3..0x48EC13, the -167 @0x48EBD9]; afloat + at-rest: the heave
    // counterweight slideDecay -= 8350 [orig: @0x48EBBE..0x48EBC7]; afloat +
    // moving: no vertical write. The latch is the last platform solve's: a hull
    // no solve runs for keeps it as it stands. A model with no box extents keeps
    // its chase-owned Z with no gravity (the stand-in for a model retail's solve
    // would fault on).
    const bool retail_row = !veh.has_graphic_model || watercraft_has_platform_geometry(traits);
    {
        const bool afloat = m.plat_afloat;
        if (!afloat) {
            m.vel_x -= (m.vel_x + 4) >> 3;
            m.vel_y -= (m.vel_y + 4) >> 3;
            m.wheel_rate_bam = io::bam_sub(
                    m.wheel_rate_bam,
                    io::bam_sar(io::bam_add(m.wheel_rate_bam, 2), 2));
            if (retail_row) m.slide_z -= 167; // [orig: @0x48EBD9]
        } else if (retail_row && m.plat_at_rest) {
            m.slide_z -= 8350; // [orig: @0x48EBC7]
        }
        // Shore look-ahead: the witnessed single bilinear sample at the NEXT
        // position [orig: Terrain_SampleHeightBilinear(pos + vel)
        // @0x48EC19..0x48EC2D] — radius 0 with default clearance collapses
        // calc_average_ground_height to exactly that center-only column. The
        // sample is compared with the water word raw, no dry gate [orig: `cmp eax,
        // g_EnvWaterHeightFixed; jl` @0x48EC35..0x48EC3B].
        if (world.tables.terrain != nullptr) {
            const int32_t ahead3[3] = {px + m.vel_x, py + m.vel_y, pz};
            const GroundClearance clearance{};
            const int32_t ground =
                    calc_average_ground_height(*world.tables.terrain, ahead3, 0, clearance);
            if (ground != INT32_MIN && ground >= world.env.water_z) {
                m.vel_x -= (m.vel_x + 4) >> 3;
                m.vel_y -= (m.vel_y + 4) >> 3;
                m.wheel_rate_bam = io::bam_sub(
                        m.wheel_rate_bam,
                        io::bam_sar(io::bam_add(m.wheel_rate_bam, 2), 2));
                if (vertical_thrust > 0 && ground - world.env.water_z > 30583 &&
                        !veh.ground_target.valid()) {
                    m.vel_x = 0;
                    m.vel_y = 0;
                    m.wheel_rate_bam = 0;
                }
            }
        }
    }

    // ---- 7. Integration -> solve -> yaw, the witnessed order
    // [orig: Position += velocity (X/Y/Z all UNCONDITIONAL)
    // @0x48ECA8..0x48ECC6; the wheel-rate self-decay @0x48ECC9..0x48ECE1;
    // the platform call @0x48ECE7; Yaw += modelPtr0 AFTER it @0x48ECF2].
    px += m.vel_x;
    py += m.vel_y;
    if (retail_row) pz += m.slide_z; // [orig: add [esi+0Ch] @0x48ECC6]
    {
        const int32_t r = m.wheel_rate_bam;
        m.wheel_rate_bam = io::bam_sub(
                io::bam_sub(r, io::bam_sar(io::bam_add(r, 16), 5)),
                io::bam_sar(r, 31)); // decay toward zero
    }

    veh.position.x.q16 = px;
    veh.position.y.q16 = py;
    veh.position.z.q16 = pz;

    // The platform solve runs every tick after integration, exactly where the
    // retail caller sits [orig: call @0x48ECE7 — after Position += velocity,
    // before the yaw apply]. It owns Z + Pitch/Roll + the afloat/airborne
    // flags from here (the chase-staged Z above is its seed, matching the
    // template's airborne-only Z-step gate). Yaw applies only AFTER this call.
    world.vehicles.watercraft_platform_solve(veh, traits);
    m.yaw_bam = io::bam_add(m.yaw_bam, m.wheel_rate_bam);
    veh.heading = m.yaw_bam;
	detail::vehicle_sample_trails(world, veh, traits, m.cmd_speed);
	// No rotor machine here: Entity_UpdateWatercraftPhysics @0x48D480..0x48EF74
	// calls neither @0x4928B0 nor @0x48FA70, and its wheel phase is the thrust
	// block's inline add, ported above inside the up.z gate.
}

// Authority watercraft entry: stage occupant, AI or parked commands, advance the shared boat
// motor, then apply the authority capsize drain.
// Witness sites: [orig: @0x48DF7F, @0x48DFA2, @0x48D51F, @0x48D6DA, @0x48DACD, @0x48DE04,
// @0x48DE7B, @0x48DFD3, @0x48DFDF, @0x48ECF5, @0x48D480]
void VehicleSystem::tick_watercraft_motor(Entity &veh, const VehicleTraits &traits) {
    World &world = world_;
	if (traits.physics == 0) {
		tick_simple_motor(veh, traits, false);
		return;
	}
	Entity::VehicleMotorState &m = veh.veh;
	if (!m.yaw_seeded) {
		// The unmoved hull's placement angles, as the entry copy reads them.
		// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6;
		//  Entity_UpdateWatercraftPhysics @0x48D4B2/@0x48D4BB/@0x48D4CB]
		m.yaw_bam = veh.heading;
        m.air_pitch_bam = veh.pitch_bam;
        m.air_roll_bam = veh.roll_bam;
        m.yaw_seeded = true;
	}
	// A hull with no brain (+0x64) jumps from the stamp (the pass stamped it ahead
	// of this call) to the matrix-build tail: no ground probe, carrier ride, health
	// cadence, claimant edge, input, motion or sound fold (@0x48EEB4).
	// [orig: Entity_UpdateWatercraftPhysics `mov ebp, [esi+64h]` @0x48D499, the
	//  stamp @0x48D4AC..0x48D4E9, `jz` @0x48D4EF -> the tail @0x48EF50..0x48EF63]
	if (world.ai.for_handle(veh.handle) == nullptr) {
		veh.engine_flags |= kEntityFlagMatrixBuilt;
		return;
	}
	vehicle_refresh_ground_link(world, veh, traits);
	vehicle_follow_carrier(world, veh);
	// The platform solve owns Z/attitude/afloat from the first tick; an authored
	// hull starts with no prior solve frame, so seed the latch exactly like the
	// client path does before its first thrust pass.
	if (watercraft_has_platform_geometry(traits) && !m.plat_solve_valid)
		watercraft_seed_platform_latch(world, veh, traits);

	tick_health(veh, traits);
	update_engine_sound(veh, traits);

	// A DEAD hull skips everything to the matrix-build tail — no input, no
	// integration [orig: `test Flags, 2 -> jnz 0x48EF4B` @0x48DDFA]. Retail has
	// ONE flags word; our death chain latches the dead bit on engine_flags
	// (destruction.cpp), so read the established combined view. The tail still
	// sets Flags 0x20000 [orig: @0x48EF63].
	if (((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0) {
		veh.engine_flags |= kEntityFlagMatrixBuilt;
		return;
	}

	// Capsize damage, authority-only: past ~100 deg of roll OR pitch the hull
	// drains 200 health per tick to zero [orig: @0x48DE84..0x48DECD —
	// |Roll|/|Pitch| > 0x471C7180, Health -= 200, floor 0 and clear attacker].
	if (world.ai.is_authority && veh.health > 0 &&
			(io::bam_abs(m.air_roll_bam) > 0x471C7180 ||
					io::bam_abs(m.air_pitch_bam) > 0x471C7180)) {
		veh.health -= 200;
		if (veh.health <= 0) {
			veh.health = 0;
			veh.last_attacker = {};
		}
	}

	// Sound-lane classification only. The boat ENTRY split is occupant-NULL or
	// the dead flag — the witness records no health term (a 0-hp hull that never
	// took the kill edge, e.g. the capsize drain floor, keeps driving); the
	// ground family's mode-21 health check is its own witness and stays there.
	const bool wrecked = veh.health <= 0;

	// ------------------------------------------------------------------ input block
    // [orig: the gate @0x48DF7F..0x48DFA2; occupant resolve + class split
    // @0x48DFA8..0x48DFCD — no attrib 0x40 means the whole block is skipped and
    // the core runs on the persisted registers]
    if (traits.player_control) {
        // The entry split reads the seat and the hull's dead bit alone: no
        // occupant life (a body that dies seated has left at its own death edge,
        // ahead of this pool-1 update) and no disable bit.
        // [orig: @0x48DFA8..0x48DFBA; Entity_UpdateInfantryAI @0x4B9910
        //  (@0x4B9C57..0x4B9C60)]
        Entity *occ = world.vehicles.resolve_controller(veh);
        const bool player_occupant =
                occ != nullptr && occ->handle.pool() == 0 && occ->player_class != 0;
        // Every arm runs here, after the mover's head (the carrier ride, the health
        // cadence, the capsize drain).
        AiEntity *brain = world.ai.for_handle(veh.handle);
        VehicleDriveCmd cmd;
        if (occ == nullptr || ((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0) {
            // Parked/no controller: hold heading, zero command and ramp, lights
            // off; watercraft_ai_drive's parked arm stamps state 22 and counts the
            // stuck escalation. [orig: @0x48E7EE..0x48E81E]
            vehicle_park_inputs(veh);
            world.ai.watercraft_ai_drive(world, veh, occ, traits, cmd);
        } else {
            // Occupied and not dead: the stuck count rests whatever the leg
            // [orig: `moveTimer = 0` @0x48DFC0].
            m.stuck_ticks = 0;
            if (player_occupant && !watercraft_driver_submerged(world, *occ)) {
                // The human-driver leg: state 22, then 8-way keys + analog through
                // waterSpeed, the boat bit6/bit7 overrides, the 45-deg ramp cap,
                // walk/creep halving, lights [orig: @0x48DFE5..0x48E20F, state 22
                // @0x48DFF5]. A player whose head is under the water plane routes to
                // the AI leg instead [orig: the submerged-driver cut
                // @0x48DFD3..0x48DFDF].
                if (brain != nullptr) brain->brain.f[AiBrain::kCurState] = 22;
                stage_player_vehicle_input(world_, veh, *occ, traits);
            } else {
                // The AI-driver leg writes the command registers in place, the ramp
                // only in its boarders' hold [orig: @0x48E247..0x48E7EC].
                world.ai.watercraft_ai_drive(world, veh, occ, traits, cmd);
                if (cmd.ai_drive) {
                    m.steer_target_bam = cmd.steer_target_bam;
                    m.cmd_speed = cmd.cmd_speed;
                }
            }
        }
    }

    watercraft_motor_core(world, veh, traits);

    // Movement-sound presentation, same per-tick site as the ground core's tail
    // [orig: the cbot movement-sound call in step 18 @0x48ED76..].
    world.vehicles.update_ground_sound(veh, traits, wrecked, /*collided=*/false);
    // The part-animation tick is the core's (@0x48E9F0..0x48E9F9 runs once
    // per mover pass); a second call here would double the wheel phase.
    // The matrix-build tail sets Flags 0x20000 (kEntityFlagMatrixBuilt, homed on
    // engine_flags) [orig: Entity_UpdateWatercraftPhysics @0x48EF5B, OR @0x48EF63].
    veh.engine_flags |= kEntityFlagMatrixBuilt;
}

} // namespace opennova::world
