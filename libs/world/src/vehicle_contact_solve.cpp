#include "world/vehicle_motor.h"

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

#include "world/angle.h"
#include "world/world.h"

namespace opennova::world {
namespace detail {

// The aircraft ground/water contact + suspension solve — the client-executed
// subset of Entity_ProcessAircraftContactPhysics [orig: @0x47EF10 (defined
// 2026-07-31); sole caller @0x49254E — AFTER Position += velocity/slideDecay,
// BEFORE the attitude-rate integration; ungated on authority]. Witness:
// docs/world/vehicle-client-movers-re.md §6; port contract §6.16. Cited
// deferrals: entity-entity bone collision + momentum exchange (no proximity
// seam — the terrain leg of Entity_ComputeCollisionForces is live via
// plat_terrain_probe), the spring/oscillator cosmetic machinery (the sinks
// are zeroed at every entry and bounded at 187/tick; their §6.7 damage gates
// are witnessed-DEAD in this variant), the parked flow (rides replicated
// Flags 0x10 — unreplicated to rows; the sim's wire-frozen gate owns dead
// hulls), splash/scrape FX + the caller's touchdown thud (sound seam), and
// every authority Health write.
// The activity predicate shared by the solve and its caller: the full solve
// runs only when the model boxes exist AND the pad radius is positive —
// anything else takes the terrain-clamp stand-in, so the two sides can never
// disagree about who owns Z and the flags.
bool air_contact_solve_active(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           traits.box_y_hi != traits.box_y_lo &&
           ((traits.box_y_hi - traits.box_y_lo) >> 2) > 0;
}

void aircraft_contact_solve(World &world, Entity &veh,
                            const VehicleTraits &traits,
                            Entity::VehicleMotorState &m,
                            int32_t start_x, int32_t start_y,
                            int32_t &px, int32_t &py, int32_t &pz) {
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

    // ---- §6.15 sleep fast-path: an at-rest hull undoes the mover's vertical
    // dribble and skips the whole solve [orig: @0x47EF20..0x47F189]. Live
    // gates: velocities/speed/rates zero, not airborne, not carried
    // (Flags 0x40), the slide window, the planar pose unchanged since mover
    // entry (Transform_ComparePartial — Z necessarily moved by the slide
    // dribble the path exists to undo, so the compare is planar), and no
    // occupant. The energy (+0x300), +0x5C timer, and four-sink gates ride
    // the deferred spring/oscillator machinery (the sinks are identically
    // zero in this subset).
    if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 &&
        m.wheel_rate_bam == 0 && m.air_pitch_rate == 0 &&
        m.air_roll_rate == 0 && (veh.flags & kEntityFlagInAir) == 0 &&
        (veh.flags & 0x40u) == 0 && m.slide_z > -350 && m.slide_z < 0 &&
        px == start_x && py == start_y && !veh.primary_occupant.valid()) {
        pz -= m.slide_z;      // [orig: @0x47F059]
        m.slide_z >>= 1;      // [orig: @0x47F067]
        return;
    }

    // ---- §6.3 probe points: 4 gear pads (the footprint corners inset by the
    // pad radius r = beam span >> 2 — the Y pair B[14]/B[15], the same pair
    // the boat solve reads as beam) + 3 upper-spine points [orig: r
    // @0x47F4D1 region; pads @0x47F514..0x47F683; spine @0x47F6F5..0x47F807].
    // The per-pad spring compressions (+0x2D4 sinks) ride the deferred
    // oscillator machinery and contribute 0 here.
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

    const VehicleEulerBasis basis = vehicle_euler_basis(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
    int32_t probes[7][3];
    for (int i = 0; i < 7; ++i) {
        int32_t rotated[3];
        basis.q22.rotate_point(probes_model[i], rotated);
        probes[i][0] = px + rotated[0];
        probes[i][1] = py + rotated[1];
        probes[i][2] = pz + rotated[2];
    }

    // ---- §6.4 the two force passes + severity (terrain leg; the shared
    // sub-contract with the platform solve) [orig: @0x47F855..0x480150].
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
    int32_t sev = 0;
    for (int i = 0; i < 7; ++i)
        sev = std::max(sev, plat_terrain_probe(world, probes[i][0],
                                               probes[i][1], probes[i][2],
                                               radii[i], soft, hard,
                                               forces[i]));
    if (sev == 1) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x47F905..]
    } else if (sev == 2) {
        m.speed -= m.speed >> ((traits.torque + 1) & 31);
    } else if (sev == 3) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31);
        // Authority damage / unitType-3 kill / scrape sound / momentum
        // exchange = cited deferrals (§6.4). The 0.25 cut keeps its witnessed
        // strongest-point distance gate; the no-hit-entity condition is
        // vacuously true (entity collision deferred).
        int strongest = 0;
        int64_t best = -1;
        for (int i = 0; i < 7; ++i) {
            const int64_t sfx = forces[i].fx, sfy = forces[i].fy;
            const int64_t mag2 = sfx * sfx + sfy * sfy;
            if (mag2 > best) { best = mag2; strongest = i; }
        }
        const int64_t ddx = int64_t(probes[strongest][0]) - px;
        const int64_t ddy = int64_t(probes[strongest][1]) - py;
        if (ddx * ddx + ddy * ddy > int64_t(0x8000) * 0x8000)
            m.speed = int32_t(m.speed * 0.25); // [orig: @0x47FDF8 region]
    }
    int32_t d[7];
    for (int i = 0; i < 7; ++i) d[i] = forces[i].fz;
    if (sev >= 1) {
        int64_t dX = 0, dY = 0;
        for (int i = 0; i < 7; ++i) { dX += forces[i].fx; dY += forces[i].fy; }
        for (int i = 0; i < 7; ++i) {
            probes[i][0] += int32_t(dX);
            probes[i][1] += int32_t(dY);
        }
        PlatProbeForce forces2[7];
        int32_t sev2 = 0;
        for (int i = 0; i < 7; ++i)
            sev2 = std::max(sev2, plat_terrain_probe(world, probes[i][0],
                                                     probes[i][1],
                                                     probes[i][2], radii[i],
                                                     soft, hard, forces2[i]));
        if (sev2 != 0) {
            int64_t dX2 = 0, dY2 = 0;
            for (int i = 0; i < 7; ++i) {
                dX2 += forces2[i].fx;
                dY2 += forces2[i].fy;
            }
            for (int i = 0; i < 7; ++i) d[i] = (forces2[i].fz + d[i]) >> 1;
            dX = (dX2 + dX) >> 1;
            dY = (dY2 + dY) >> 1;
        }
        px += int32_t(dX); // the planar separation that keeps a remote
        py += int32_t(dY); // aircraft out of hillsides [orig: @0x480143..]
    }

    // ---- §6.5 the in-water flag with the r/2 hysteresis
    // [orig: @0x48021A..0x48033D; splash FX = cited deferral].
    if (world.env.water_z != 0) {
        int32_t avg = (probes[0][2] + probes[1][2] + probes[2][2] +
                       probes[3][2]) >> 2;
        if ((veh.flags & 0x8000u) != 0u) avg -= r >> 1;
        const int32_t test_z = avg + hull_bottom_neg;
        if (test_z >= world.env.water_z) veh.flags &= ~0x8000u;
        else veh.flags |= 0x8000u;
    }

    // ---- §6.10 branch select: any PAD depth = grounded.
    const bool pad_contact = d[0] > 0 || d[1] > 0 || d[2] > 0 || d[3] > 0;
    if (!pad_contact) {
        // §6.11 AIRBORNE: the flag sets here [orig: @0x480ED5]; the
        // parked-frozen/wreck Z-hold rides the deferred park/wreck latches.
        // Pitch/Roll are an identity round-trip through the solver for a
        // clean flying hull — the mover integrates the rates on top.
        veh.flags |= kEntityFlagInAir;
        return;
    }
    // §6.12 GROUNDED: the flag clears unconditionally [orig: @0x4810EB].
    veh.flags &= ~kEntityFlagInAir;
    // The pad-rectangle bounding quad — the boat solve's witnessed corner
    // table ((-s,+f),(+s,+f),(-s,-f),(+s,-f)) around the same probe winding
    // ((+f,+s),(+f,-s),(-f,-s),(-f,+s)) — with corner targets lifted by the
    // resolved penetrations IDENTITY k-k, the load-bearing pairing shared
    // with the boat port [orig: quad @0x4805B4..; corner_z[i] += d_i
    // @0x481425..0x481427]. (The spring resolution is deferred, so d_i
    // lifts raw.)
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
    plat_fit_corners(c, fit);
    // Pitch/Roll ALWAYS overwritten from the conform [orig: @0x48148A..];
    // Yaw only when parked (deferred). The mover adds the rates after return.
    m.air_pitch_bam = fit.pitch_bam;
    m.air_roll_bam = fit.roll_bam;
    if (basis.up[2] > 0.0) {
        // Upright non-parked: slideDecay clamped toward the ground, then
        // Z = the solver chassis Z [orig: @0x481834..0x481849] — the wheeled
        // solver's form: the average of the ABOVE-ZERO corners only,
        // rise-clamped +0x2000/tick (blockers §3.3 @0x46C822..0x46C894).
        if (m.slide_z > 0) m.slide_z = 0;
        int32_t new_z = fit.positive_z_avg;
        if (new_z > pz + 0x2000) new_z = pz + 0x2000;
        pz = new_z;
    } else {
        // Inverted: lift by the max penetration [orig: @0x4814C0..0x4814C4].
        int32_t maxd = 0;
        for (int i = 0; i < 7; ++i) maxd = std::max(maxd, d[i]);
        pz += maxd;
    }
}

// The ground/tracked contact + suspension solve — the client-executed subset
// of Entity_ProcessTrackedVehiclePhysics [orig: @0x47C1C0; sole caller
// Entity_UpdateVehiclePhysics @0x48d0b1 — AFTER Position += velocity/slideDecay,
// BEFORE the grounded yaw apply @0x48d0d4; ungated on authority, so a joiner
// runs it for every predicted cveh/ctrn/catv row]. Witness:
// docs/world/vehicle-client-movers-re.md §7. This is what rests a parked hull
// at wheel height above terrain: the pad probes sit at box_z_lo + r, so the
// solved Z holds the origin at ground - box_z_lo (the per-model wheel
// clearance) instead of clamping the origin onto the terrain.
// Cited deferrals (same seams as the air solve §6): every authority Health
// write (object-impact damage @0x47CD00..0x47CDFB, crush @0x47D96B..0x47DA2B,
// inverted-crush @0x47DBFF..0x47DC50, burn drain @0x47DDF4), the
// spring/oscillator machinery (the +0x2C4 sinks, free-fall +187/tick
// @0x47DB59..0x47DBE4, the spring-energy resolution loop @0x47E960..0x47EC1F —
// its state is identically zero here, so the corner lifts consume the raw d_i
// exactly like the zero-state original; the +0x2D4 brake-dive probe offsets are
// written only by the wheeled-solve brake machinery @0x45CEB0/@0x4790C7 and
// stay zero for tracked rows), entity-entity collision + momentum exchange
// (@0x47CE7C../@0x47D097../@0x47D336..; plat_terrain_probe carries the terrain
// leg only), the crash/park/wreck latch machine (+0x2EC/+0x2ED/+0x2EE/+0x2EF/
// +0x2F0/+0x2FC bytes, the flip threshold @0x47D727, the client landing-grace
// timer @0x47E7A3, flip-restore @0x47ED73/@0x47EEAC — parked flow rides
// replicated Flags 0x10; the sim's wire-frozen gate owns dead hulls), the
// contact-direction store for the mover's slope-velocity re-derive
// (@0x47E65D..0x47E78F, read back @0x48cf97..0x48d003 — the mover keeps its
// level-frame re-derive, D-NET-161), scrape/landing sounds and splash/dust FX,
// and the airborne tick counter (entity[1] bookkeeping @0x47EEC7).
// The activity predicate mirrors the air solve's: retail bails before any Z
// logic when the row has no graphic model [orig: @0x47C49F `return 0`]; our
// boxless/terrain-less rows keep the terrain-clamp stand-in at the call site
// so lib embedders stay driveable (the same documented substitute).
bool ground_contact_solve_active(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           traits.box_y_hi != traits.box_y_lo &&
           ((traits.box_y_hi - traits.box_y_lo) >> 2) > 0;
}

void ground_contact_solve(World &world, Entity &veh, const VehicleTraits &traits,
                          Entity::VehicleMotorState &m,
                          int32_t start_x, int32_t start_y,
                          int32_t &px, int32_t &py, int32_t &pz) {
    // ---- sleep fast-path: an at-rest hull undoes the mover's vertical dribble
    // and skips the whole solve [orig: @0x47C244..0x47C44B]. Live gates:
    // velocities/speed/yaw-rate zero, not airborne, the slide window, the
    // planar pose unchanged since mover entry (Transform_ComparePartial — Z
    // necessarily moved by the slide dribble this path exists to undo), and no
    // occupant [orig: occupantEntity +0x170 == 0 @0x47C302]. The carried flag
    // (0x40), the +0x5C timer, the four spring sinks and the energy word ride
    // the deferred machinery and are identically zero here.
    if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 &&
        m.wheel_rate_bam == 0 && (veh.flags & kEntityFlagInAir) == 0 &&
        m.slide_z > -350 && m.slide_z < 0 &&
        px == start_x && py == start_y && !veh.primary_occupant.valid()) {
        pz -= m.slide_z; // [orig: @0x47C349]
        m.slide_z >>= 1; // [orig: @0x47C357]
        // Contact-byte refresh from the current pose: up.z above the capsize
        // floor [orig: @0x47C35D..0x47C395 `BYTE2(aiRef0) = upZ > 4096 &&
        // !crashed`; the crashed byte rides the deferred wreck machine]. The
        // authority park upkeep + at-rest flip restore are cited deferrals
        // [orig: @0x47C3A3..0x47C443].
        const VehicleEulerBasis rest_basis = vehicle_euler_basis(
                m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
        m.grounded = rest_basis.up[2] * 65536.0 > 4096.0;
        return;
    }

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
    const int32_t pad_z = traits.box_z_lo + r;
    const int32_t probes_model[7][3] = {
        {traits.foot_x_hi - r, traits.foot_y_hi - r, pad_z}, // pad0 (+fwd,+side)
        {traits.foot_x_hi - r, traits.foot_y_lo + r, pad_z}, // pad1 (+fwd,-side)
        {traits.foot_x_lo + r, traits.foot_y_lo + r, pad_z}, // pad2 (-fwd,-side)
        {traits.foot_x_lo + r, traits.foot_y_hi - r, pad_z}, // pad3 (-fwd,+side)
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

    const VehicleEulerBasis basis = vehicle_euler_basis(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
    int32_t probes[7][3];
    for (int i = 0; i < 7; ++i) {
        int32_t rotated[3];
        basis.q22.rotate_point(probes_model[i], rotated);
        probes[i][0] = px + rotated[0];
        probes[i][1] = py + rotated[1];
        probes[i][2] = pz + rotated[2];
    }

    // ---- the two force passes + severity (the shared sub-contract with the
    // platform/air solves; terrain leg only) [orig: Entity_CheckCollisionState
    // calls @0x47CB8C/@0x47D213].
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
    int32_t sev = 0;
    for (int i = 0; i < 7; ++i)
        sev = std::max(sev, plat_terrain_probe(world, probes[i][0],
                                               probes[i][1], probes[i][2],
                                               radii[i], soft, hard,
                                               forces[i]));
    if (sev == 1) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x47CC31]
    } else if (sev == 2) {
        m.speed -= m.speed >> ((traits.torque + 1) & 31); // [orig: @0x47CC71]
    } else if (sev == 3) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x47CCA1]
        // Authority damage/kill + scrape sound + momentum exchange = cited
        // deferrals. The 0.25 cut keeps its witnessed strongest-point distance
        // gate and fires only with no hit entity (vacuously true — entity
        // collision is deferred); the deflection heading pair feeds a
        // witnessed-dead yaw kick [orig: scan @0x47CF5E..0x47D038; cut
        // @0x47D0DE..0x47D0EF].
        int strongest = 0;
        int64_t best = -1;
        for (int i = 0; i < 7; ++i) {
            const int64_t sfx = forces[i].fx, sfy = forces[i].fy;
            const int64_t mag2 = sfx * sfx + sfy * sfy;
            if (mag2 > best) { best = mag2; strongest = i; }
        }
        const int64_t ddx = int64_t(probes[strongest][0]) - px;
        const int64_t ddy = int64_t(probes[strongest][1]) - py;
        if (ddx * ddx + ddy * ddy > int64_t(0x8000) * 0x8000)
            m.speed = int32_t(m.speed * 0.25); // [orig: flt_7C333C @0x47D0DE]
    }
    int32_t d[7];
    for (int i = 0; i < 7; ++i) d[i] = forces[i].fz;
    if (sev >= 1) {
        // Second pass over the planar-shifted probes, averaged in when it still
        // collides; the position push is X/Y only (the Z sum rides the deferred
        // entity-mass leg and is zero) [orig: @0x47D0F5..0x47D330; push
        // @0x47D452..0x47D458].
        int64_t dX = 0, dY = 0;
        for (int i = 0; i < 7; ++i) { dX += forces[i].fx; dY += forces[i].fy; }
        for (int i = 0; i < 7; ++i) {
            probes[i][0] += int32_t(dX);
            probes[i][1] += int32_t(dY);
        }
        PlatProbeForce forces2[7];
        int32_t sev2 = 0;
        for (int i = 0; i < 7; ++i)
            sev2 = std::max(sev2, plat_terrain_probe(world, probes[i][0],
                                                     probes[i][1],
                                                     probes[i][2], radii[i],
                                                     soft, hard, forces2[i]));
        if (sev2 != 0) {
            int64_t dX2 = 0, dY2 = 0;
            for (int i = 0; i < 7; ++i) {
                dX2 += forces2[i].fx;
                dY2 += forces2[i].fy;
            }
            for (int i = 0; i < 7; ++i) d[i] = (forces2[i].fz + d[i]) >> 1;
            dX = (dX2 + dX) >> 1;
            dY = (dY2 + dY) >> 1;
        }
        px += int32_t(dX);
        py += int32_t(dY);
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
    // The in-water flag with the r/2 hysteresis [orig: @0x47D516..0x47D53A;
    // clear + emitter release @0x47D637..0x47D6BB; set + splash FX/overlay
    // @0x47D542..0x47D629 — FX/overlay are cited deferrals]. W == 0 = our
    // no-water-world sentinel (retail worlds always carry a plane).
    if (world.env.water_z != 0) {
        int32_t avg = (probes[0][2] + probes[1][2] + probes[2][2] +
                       probes[3][2]) >> 2;
        if ((veh.flags & 0x8000u) != 0u) avg -= r >> 1;
        if (hull_bottom_neg + avg >= world.env.water_z)
            veh.flags &= ~0x8000u;
        else
            veh.flags |= 0x8000u;
    }

    // ---- the contact byte the mover's yaw apply and velocity re-derive read
    // [orig: BYTE2(aiRef0) @0x47D7F4..0x47D8AF]: an upright hull grounds on a
    // same-side or diagonal pad pair (the axle pairs 0/1 and 2/3 do not
    // count), and a LIGHT hull (mass <= 10) grounds on any single pad while
    // not steeply pitched (fwd.z < 24576). The crash/wreck/settle byte gates
    // ride the deferred latch machine.
    const int32_t up_z16 = static_cast<int32_t>(basis.up[2] * 65536.0);
    const int32_t fwd_z16 = static_cast<int32_t>(basis.fwd[2] * 65536.0);
    const bool pair_contact =
            (d[0] != 0 && d[3] != 0) || (d[1] != 0 && d[2] != 0) ||
            (d[0] != 0 && d[2] != 0) || (d[1] != 0 && d[3] != 0);
    const bool any_pad = d[0] != 0 || d[1] != 0 || d[2] != 0 || d[3] != 0;
    m.grounded = (up_z16 > 4096 && pair_contact) ||
                 (any_pad && fwd_z16 < 24576 && traits.mass <= 10 &&
                  up_z16 > 4096);

    // ---- solve select: the no-pad branch [orig: the all-zero pad test
    // @0x47E1A9].
    if (!any_pad) {
        if (up_z16 < 0) {
            // Inverted with spine contact: lift by the max penetration over all
            // seven probes, grounded again; the crash-latch adoption riding
            // this leg is deferred with the wreck machine [orig: scan
            // @0x47E1E9..0x47E23E; `Position.Z += v136` @0x47E358; the
            // un-airborne store @0x47E4C6].
            int32_t maxd = 0;
            for (int i = 0; i < 7; ++i) maxd = std::max(maxd, d[i]);
            if (maxd > 0) {
                pz += maxd;
                veh.flags &= ~kEntityFlagInAir;
                return;
            }
        }
        // Airborne: the flag sets here; the fit runs mode-1 on the unlifted
        // corners — an identity round-trip for attitude, so the pose keeps its
        // last conform (the client parked-leg spring apply is deferred with
        // the park flow) [orig: Flags |= 0x2000 @0x47E57B; the NULL-contact
        // suspension call @0x47E5E6].
        veh.flags |= kEntityFlagInAir;
        return;
    }
    // ---- pad contact: airborne clears unconditionally [orig: @0x47E8EE].
    veh.flags &= ~kEntityFlagInAir;
    // The pad-rectangle bounding quad, in the witnessed winding — corner k
    // sits over pad k, so the identity d_k lift pairing is geometric here
    // [orig: Entity_ComputeBoundingQuad @0x45B6E0 non-square arm, called
    // @0x47DAE2/@0x47DB54: c0=(+f,+s), c1=(+f,-s), c2=(-f,-s), c3=(-f,+s);
    // corner_z[k] += d_k in the spring loop's zero-state arm @0x47EC05].
    // (The spring resolution is deferred, so d_k lifts raw — the air-port
    // contract §6.16.)
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
            c[k][2] = pz + rotated[2] + d[k];
        }
    }
    PlatFit fit;
    plat_fit_corners(c, fit);
    // Pitch/Roll ALWAYS overwritten from the conform; Yaw only when crashed
    // (deferred with the wreck machine) [orig: Math_FixedPointMatrixToEulerAngles
    // @0x47EC62; Pitch @0x47EC83, Roll @0x47EC75, crashed Yaw @0x47EC8F]. The
    // int16 degree mirrors feed presentation and mounted-pose consumers — one
    // entity Pitch/Roll in the original.
    m.air_pitch_bam = fit.pitch_bam;
    m.air_roll_bam = fit.roll_bam;
    veh.pitch = static_cast<int16_t>(std::lround(
            double(m.air_pitch_bam) * kDegreesPerBam));
    veh.roll = static_cast<int16_t>(std::lround(
            double(m.air_roll_bam) * kDegreesPerBam));
    if (up_z16 > 0) {
        // Upright: slideDecay clamped toward the ground, then Z = the solver
        // chassis Z — the wheeled solver's positive-corner average,
        // rise-clamped +0x2000/tick [orig: the Z select @0x47ECAC..0x47ECBB;
        // solver Z @0x46C822..0x46C894 in Entity_ProcessWheeledVehicleSuspension
        // @0x46B140, call @0x47EC4D].
        if (m.slide_z > 0) m.slide_z = 0;
        int32_t new_z = fit.positive_z_avg;
        if (new_z > pz + 0x2000) new_z = pz + 0x2000;
        pz = new_z;
    } else {
        // Inverted: lift by the max penetration over all seven probes
        // [orig: `Position.Z += maxGroundHeight` @0x47ECE2 with the
        // @0x47E09F..0x47E107 scan].
        int32_t maxd = 0;
        for (int i = 0; i < 7; ++i) maxd = std::max(maxd, d[i]);
        pz += maxd;
    }
}

} // namespace detail
} // namespace opennova::world
