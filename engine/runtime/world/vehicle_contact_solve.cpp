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
#include "world/vehicle_suspension.h"
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
// inverted-crush @0x47DBFF..0x47DC50, burn drain @0x47DDF4), the remaining
// spring/oscillator machinery (the +0x2C4 sinks and their free-fall growth
// +187/tick @0x47DB59..0x47DBE4, and the corner-lift feedback of the stepped
// compression in the spring-energy loop @0x47E960..0x47EC1F — the per-wheel
// compress/oscillate step itself and the +0x2D4 pad offsets are LIVE through
// vehicle_suspension.cpp since 2026-08-21; the corner lifts still consume the
// raw d_i pending that feedback witness), entity-entity collision + momentum exchange
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
    // The spring leg over this tick's pad depths: the parked latch edge and
    // the per-wheel compress/oscillate step (vehicle_suspension.cpp) — the
    // compressions feed NEXT tick's pad points above [orig: the latch
    //  @0x46B1A6..0x46B213 inside the wheel-solver call @0x47EC4D; the spring
    //  loop @0x47E960..0x47EC1F with its compress/oscillate arms].
    vehicle_suspension_latch(world, veh);
    vehicle_suspension_step(world, veh, traits, d);
    // The pad-rectangle bounding quad, in the witnessed winding — corner k
    // sits over pad k, so the identity d_k lift pairing is geometric here
    // [orig: Entity_ComputeBoundingQuad @0x45B6E0 non-square arm, called
    // @0x47DAE2/@0x47DB54: c0=(+f,+s), c1=(+f,-s), c2=(-f,-s), c3=(-f,+s);
    // corner_z[k] += d_k in the spring loop's arm @0x47EC05].
    // WITNESS PENDING: the corner-lift feedback of the STEPPED compression
    // (the spring loop's non-zero-state arm) — d_k lifts raw until witnessed.
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

// The wheeled (ctan/tank) contact + suspension solve — the client-executed
// subset of Entity_ProcessWheeledVehiclePhysics [orig: @0x475DE0; live caller
// the tank mover Entity_UpdateTankVehiclePhysics @0x488AB0, call @0x48a9ef,
// water-support arg 0 via the ctank dispatcher's push @0x48f004]. Witness:
// docs/world/vehicle-client-movers-re.md §8. The skeleton is the tracked
// solve with the wheeled deltas: 13 probes (4 wheel pads + 6 belly stations
// along the side rails + 3 spine, ALL at the pad radius r), per-probe
// planar-contact/reverse flags feeding a stability contact byte and the
// head-on wall stop, and a Z select that absorbs the step into slideDecay
// instead of the tracked +0x2000 rise clamp.
// Cited deferrals (the same seams as the tracked solve §7): every authority
// Health write (park-move damage @0x477157.., spine-impact @0x47733f..,
// underside-crush @0x477e6a-region, burn drain/emitters), the wheeled
// family's own spring pair (sink growth +250/tick on airborne wheels, the
// Suspension_CompressWheelLinear @0x45CEB0 step, Suspension_OscillateWheel
// @0x45D240 — WITNESS PENDING for the linear/slow pair's wiring; the pads
// ride +0x2D4 and the latch edge runs through vehicle_suspension.cpp, the
// quadratic/fast pair the tracked and light solves call is live there),
// entity-entity collision + momentum exchange (mass-gated transfer at the
// sev-3 leg and the entity-mass delta scaling — plat_terrain_probe carries
// the terrain leg only), the crash/flip/park/wreck latch machine (the
// capsize-threshold flip byte, the falling-crash client latch, the settle
// state machine + Yaw adoption, Entity_ApplyWheelSuspensionForces' parked
// spring apply), the contact-direction downhill store (+0x3BC..+0x3C4 with
// the fixed -28672 Z renormalize — the mover keeps its level-frame re-derive,
// D-NET-161), and scrape/landing sounds, splash FX + the water
// enter/exit overlay sends.
void wheeled_contact_solve(World &world, Entity &veh,
                           const VehicleTraits &traits,
                           Entity::VehicleMotorState &m,
                           int32_t start_x, int32_t start_y,
                           int32_t &px, int32_t &py, int32_t &pz) {
    // ---- sleep fast-path [orig: @0x475E5C..0x475FB2]: the tracked gate set
    // (velocities/speed/rates zero, not airborne, not carried, slideDecay in
    // (-350,-1] via the unsigned `> 0xFFFFFEA2` compare, planar
    // Transform_ComparePartial, no occupant; the four spring sinks and the
    // energy word ride the deferred machinery and are identically zero).
    // Action: undo the vertical dribble, then refresh the contact byte from
    // the current pose [orig: Z -= slideDecay @0x475F51; slideDecay >>= 1
    // @0x475F5F; `BYTE2(aiRef0) = upZ > 4096 && !crashed` @0x475F8F]. The
    // authority park upkeep + the at-rest flip restore are latch-machine
    // deferrals [orig: @0x475FA1..0x476040].
    if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 &&
        m.wheel_rate_bam == 0 && (veh.flags & kEntityFlagInAir) == 0 &&
        m.slide_z > -350 && m.slide_z < 0 &&
        px == start_x && py == start_y && !veh.primary_occupant.valid()) {
        pz -= m.slide_z;
        m.slide_z >>= 1;
        const VehicleEulerBasis rest_basis = vehicle_euler_basis(
                m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
        m.grounded = rest_basis.up[2] * 65536.0 > 4096.0;
        return;
    }

    // ---- def clamps every call [orig: @0x476070..0x4760E5 — spring 0..10,
    // springComp 0..100, flip 0..55] feed the deferred spring/latch machinery.
    // ---- probe geometry [orig: @0x4762B8..0x476559]: 4 wheel pads at the
    // footprint corners inset by r = beam >> 2, Z = box_z_lo + r (each pad's
    // per-wheel +0x2D4 spring offset is zero here); 6 belly stations along
    // the two side rails at X = foot_x_lo + r + {3q, 3q, q, q, 2q, 2q} with
    // q = ftol(0.75 * box_x_span) >> 2 [orig: flt_7C3DC8 = 0.75 @0x4762F2];
    // 3 spine probes at X = box_x_lo + {3L/4, L/2, L/4}, Y = box ymid,
    // Z = box_z_hi -w/+w/-w with w = (beam >> 3) - 0x4000 [orig: the
    // @0x47653A +w middle-spine store]. EVERY radius is r — the wheeled solve
    // has no separate spine radius [orig: the 13 radius stores all copy
    // suspensionOffset].
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
    // The four wheel pads ride their spring compression (+0x2D4 + 4k, the
    // vehicle_suspension.cpp leg); the belly/spine stations do not.
    const int32_t probes_model[13][3] = {
        {traits.foot_x_hi - r, ys, pad_z + m.wheel_comp[0]}, // pad0 (+fwd,+side)
        {traits.foot_x_hi - r, yp, pad_z + m.wheel_comp[1]}, // pad1 (+fwd,-side)
        {traits.foot_x_lo + r, yp, pad_z + m.wheel_comp[2]}, // pad2 (-fwd,-side)
        {traits.foot_x_lo + r, ys, pad_z + m.wheel_comp[3]}, // pad3 (-fwd,+side)
        {bx + 3 * q, ys, pad_z},                        // belly4 (fwd,+side)
        {bx + 3 * q, yp, pad_z},                        // belly5 (fwd,-side)
        {bx + q, yp, pad_z},                            // belly6 (rear,-side)
        {bx + q, ys, pad_z},                            // belly7 (rear,+side)
        {bx + 2 * q, ys, pad_z},                        // belly8 (mid,+side)
        {bx + 2 * q, yp, pad_z},                        // belly9 (mid,-side)
        {traits.box_x_lo + ((3 * Lbox) >> 2), ymid_box, traits.box_z_hi - w},
        {traits.box_x_lo + (Lbox >> 1), ymid_box, traits.box_z_hi + w},
        {traits.box_x_lo + (Lbox >> 2), ymid_box, traits.box_z_hi - w},
    };
    const int32_t hull_bottom_neg = -(traits.box_z_lo + r); // [orig: -v41 @0x4761B4]
    const int32_t half_w =
            (traits.foot_y_hi - r) - (traits.foot_y_lo + r); // [orig: @0x4761E2]
    const int32_t half_h =
            (traits.foot_x_hi - r) - (traits.foot_x_lo + r);

    const VehicleEulerBasis basis = vehicle_euler_basis(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
    int32_t probes[13][3];
    for (int i = 0; i < 13; ++i) {
        int32_t rotated[3];
        basis.q22.rotate_point(probes_model[i], rotated);
        probes[i][0] = px + rotated[0];
        probes[i][1] = py + rotated[1];
        probes[i][2] = pz + rotated[2];
    }

    // ---- the two force passes + severity [orig: Entity_CheckCollisionState
    // calls @0x476897/@0x476F98; the shared terrain-leg sub-contract]. Same
    // torque sheds as tracked [orig: @0x476944/@0x476984/@0x4769B8]; the
    // sev-3 scrape sound + momentum exchange are deferred; the 0.25 cut scans
    // the STRONGEST of the first SEVEN probes only (4 pads + the three
    // forward/rear belly stations) with the > 0x8000 planar distance gate and
    // the no-hit-entity condition (vacuously true) [orig: init from probe 0
    // @0x476BC3, scan `while (v73 < 7)` @0x476C4D-region; cut via flt_7C333C].
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[13];
    int32_t sev = 0;
    for (int i = 0; i < 13; ++i)
        sev = std::max(sev, plat_terrain_probe(world, probes[i][0],
                                               probes[i][1], probes[i][2], r,
                                               soft, hard, forces[i]));
    if (sev == 1) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31);
    } else if (sev == 2) {
        m.speed -= m.speed >> ((traits.torque + 1) & 31);
    } else if (sev == 3) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31);
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
            m.speed = int32_t(m.speed * 0.25);
    }
    int32_t d[13];
    for (int i = 0; i < 13; ++i) d[i] = forces[i].fz;
    if (sev >= 1) {
        // Second pass over the planar-shifted probes, averaged in when it
        // still collides; the push is X/Y only (the Z sum rides the deferred
        // entity-mass leg) [orig: @0x476E19..0x476FF4; push @0x477149..].
        int64_t dX = 0, dY = 0;
        for (int i = 0; i < 13; ++i) { dX += forces[i].fx; dY += forces[i].fy; }
        for (int i = 0; i < 13; ++i) {
            probes[i][0] += int32_t(dX);
            probes[i][1] += int32_t(dY);
        }
        PlatProbeForce forces2[13];
        int32_t sev2 = 0;
        for (int i = 0; i < 13; ++i)
            sev2 = std::max(sev2, plat_terrain_probe(world, probes[i][0],
                                                     probes[i][1],
                                                     probes[i][2], r,
                                                     soft, hard, forces2[i]));
        if (sev2 != 0) {
            int64_t dX2 = 0, dY2 = 0;
            for (int i = 0; i < 13; ++i) {
                dX2 += forces2[i].fx;
                dY2 += forces2[i].fy;
            }
            for (int i = 0; i < 13; ++i) d[i] = (forces2[i].fz + d[i]) >> 1;
            dX = (dX2 + dX) >> 1;
            dY = (dY2 + dY) >> 1;
        }
        px += int32_t(dX);
        py += int32_t(dY);
    }

    // ---- the in-water flag with the r/2 hysteresis [orig: @0x477496..
    // 0x4774F3; wheel-average Z against the hull-bottom reference]. The
    // per-wheel water-support forces ride the dispatcher's hasWaterLevel arg,
    // which the ctank dispatcher pins to 0 [orig: push 0 @0x48f004] — tanks
    // never float; the flag itself is unconditional. Splash FX + the overlay
    // sends and the water-exit emitter release are cited deferrals.
    if (world.env.water_z != 0) {
        int32_t avg = (probes[0][2] + probes[1][2] + probes[2][2] +
                       probes[3][2]) >> 2;
        if ((veh.flags & 0x8000u) != 0u) avg -= r >> 1;
        if (hull_bottom_neg + avg >= world.env.water_z)
            veh.flags &= ~0x8000u;
        else
            veh.flags |= 0x8000u;
    }

    // ---- per-probe planar-contact + reverse flags [orig: the per-probe
    // walk @0x477BF4..0x477D0A]: a probe "contacts" when it produced a planar
    // force (flat terrain never does; walls and slope-hard faces do), and a
    // contacted probe is a REVERSE hit when its normalized force opposes the
    // contact direction (dot < -0.75 = -49152) — the direction is the stored
    // downhill contact vector, falling back to the basis forward row when the
    // store is empty [orig: the |dir|==0 fallback @0x477A48..; our subset
    // defers the store (D-NET-161), so the fallback IS the direction].
    bool contact_flag[13];
    bool reverse_flag[13];
    bool any_contact = false;
    int64_t sum_fx = 0, sum_fy = 0, sum_fz = 0;
    for (int i = 0; i < 13; ++i) {
        contact_flag[i] = forces[i].fx != 0 || forces[i].fy != 0;
        reverse_flag[i] = false;
        if (!contact_flag[i]) continue;
        any_contact = true;
        sum_fx += forces[i].fx;
        sum_fy += forces[i].fy;
        sum_fz += forces[i].fz;
        const double n = std::sqrt(double(forces[i].fx) * forces[i].fx +
                                   double(forces[i].fy) * forces[i].fy +
                                   double(forces[i].fz) * forces[i].fz);
        if (n <= 0.0) continue;
        const double dot = (double(forces[i].fx) * basis.fwd[0] +
                            double(forces[i].fy) * basis.fwd[1] +
                            double(forces[i].fz) * basis.fwd[2]) / n;
        reverse_flag[i] = dot * 65536.0 < -49152.0;
    }
    // The head-on wall stop [orig: @0x477D3E..0x477E30]: the summed contacted
    // force, normalized, against the same direction — past -0.871 (-57070)
    // the drive state zeroes. The authority park-move damage leg riding it is
    // deferred.
    if (any_contact) {
        const double n = std::sqrt(double(sum_fx) * double(sum_fx) +
                                   double(sum_fy) * double(sum_fy) +
                                   double(sum_fz) * double(sum_fz));
        if (n > 0.0) {
            const double dot = (double(sum_fx) * basis.fwd[0] +
                                double(sum_fy) * basis.fwd[1] +
                                double(sum_fz) * basis.fwd[2]) / n;
            if (dot * 65536.0 < -57070.0) {
                m.vel_x = 0;
                m.vel_y = 0;
                m.speed = 0;
            }
        }
    }

    // ---- the stability contact byte [orig: @0x477F31..0x477FA9 +
    // `BYTE2(aiRef0) = upZ > 0x2000 && stable && !crashed && !parked &&
    // !destroyed` @0x477FB4]: the leading axle for the current gear (front
    // pads driving forward, rear pads in reverse) must be SYMMETRIC — both
    // pads contacted or neither — and neither may be a reverse-direction hit.
    // The crash/park/destroyed bytes ride the deferred latch machine.
    const int32_t up_z16 = static_cast<int32_t>(basis.up[2] * 65536.0);
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

    if (center_max <= 0 && wheel_belly_max <= 0) {
        if (up_z16 < 0) {
            // Inverted with spine/slot contact: lift by the max over the
            // seven contact slots, stay grounded; the crash-latch adoption is
            // deferred with the wreck machine [orig: the gated slot scan
            // @0x47843E..0x4784DA; `Position.Z += bestSurfaceY` @0x478540].
            int32_t maxs = 0;
            for (int i = 0; i < 7; ++i) maxs = std::max(maxs, slot_max[i]);
            if (maxs > 0) {
                pz += maxs;
                return;
            }
        }
        // Airborne: the flag sets; the suspension orientation runs on the
        // unlifted corners — an attitude identity round-trip [orig: Flags |=
        // 0x2000 @0x478522; the client parked spring apply riding replicated
        // Flags 0x10 is deferred @0x478509..].
        veh.flags |= kEntityFlagInAir;
        return;
    }
    // ---- wheel/belly contact: airborne clears unconditionally
    // [orig: @0x478604 in the latch fall-through]. The corner quad lifts by
    // the four WHEEL d's only (belly/spine d's feed severity and the Z maxes)
    // in the spring loop [orig: `dest[corner].z += d_k`
    // @0x478A16/@0x478A72-region]. The wheeled family's spring pair is the
    // LINEAR compress (Suspension_CompressWheelLinear @0x45CEB0) with the SLOW
    // oscillator (Suspension_OscillateWheel @0x45D240) — WITNESS PENDING for
    // that pair's wiring; the latch edge runs here as in the tracked solve.
    veh.flags &= ~kEntityFlagInAir;
    vehicle_suspension_latch(world, veh);
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
    // Pitch/Roll always overwritten from the conform; Yaw only in the
    // crashed/latched states (deferred) [orig: the Euler stores
    // @0x478AB1..0x478AD8]. The int16 degree mirrors feed presentation.
    m.air_pitch_bam = fit.pitch_bam;
    m.air_roll_bam = fit.roll_bam;
    veh.pitch = static_cast<int16_t>(std::lround(
            double(m.air_pitch_bam) * kDegreesPerBam));
    veh.roll = static_cast<int16_t>(std::lround(
            double(m.air_roll_bam) * kDegreesPerBam));
    if (up_z16 > 0) {
        // Upright: Z = the solver chassis Z (the positive-corner average from
        // Entity_ComputeSuspensionAndOrientation [orig: @0x4698A0, the
        // positive-only corner scan + divide @0x46AF54..0x46AFE1]), with the
        // step ABSORBED into slideDecay, clamped non-positive — the wheeled
        // solve's form; there is no +0x2000 rise clamp here
        // [orig: `slideDecay += solvedZ - Position.Z; if (slideDecay > 0)
        // slideDecay = 0; Position.Z = solvedZ` @0x478BE3..0x478C06]. The
        // !orientationResult fallback (Z += max wheel d) rides the
        // crash/latch gates and is deferred with them.
        const int32_t new_z = fit.positive_z_avg;
        m.slide_z += new_z - pz;
        if (m.slide_z > 0) m.slide_z = 0;
        pz = new_z;
    } else {
        // Inverted: lift by the max penetration over all thirteen probes
        // [orig: the crashed/inverted `Position.Z += v300` @0x478D06 with the
        // gated all-13 scan @0x477FF4..0x478014].
        int32_t maxd = 0;
        for (int i = 0; i < 13; ++i) maxd = std::max(maxd, d[i]);
        pz += maxd;
    }
    // Airborne tick counter [orig: @0x4795D4..0x4795F1].
    m.plat_airborne_ticks =
            (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;
}

// The light (cbik/bike) contact + suspension solve — the client-executed
// subset of Entity_ProcessLightVehiclePhysics [orig: @0x479600; sole caller
// the cbik mover Entity_UpdateLightVehiclePhysics @0x483FE0, call @0x486672,
// water arg 0 via the cbike dispatcher @0x48eff4, frameFlags 1]. Witness:
// docs/world/vehicle-client-movers-re.md §9. Six probes on the hull
// centerline — two wheels + three spine + one mid-hull — and the 2-corner
// AXLE fit (Entity_UpdateVehicleChassisOrientation [orig: @0x468A50]): fwd =
// the normalized front-to-rear wheel line through the lifted corners, roll
// continuity by orthonormalizing against the previous up, Z = the mean of
// the two lifted wheel corners [orig: `*(out+8) = (c0.z + c1.z) * 0.5` via
// flt_7C3B94 @0x468D34-region].
// Cited deferrals (same seams as §7/§8): authority impact/eject legs (the
// head-on rider ejection ladder @0x47A343.., the belly-strike eject, the
// underside crush), the wheelie machinery and the +100/tick sink growth
// (the Suspension_CompressWheelQuadratic @0x45CFB0 /
// Suspension_OscillateWheelFast @0x45D110 step itself is LIVE through
// vehicle_suspension.cpp over the two wheel depths),
// the tip-over/crash tumble (the parked bike's 298261 BAM/tick fall-over,
// Entity_QueueSuspensionForce legs, the flip byte at the 0..100 def clamp),
// the grounded heading/lean smoother (Entity_SmoothHeadingToTarget
// [orig: @0x45B2C0, call @0x47a7d3] — the FPU-garbled roll-rate producer;
// the lean is presentation-additive and stays a named deferral pinned to its
// disassembly), the contact-direction store (D-NET-161), entity momentum
// exchange, and every sound/FX/overlay send.
bool light_contact_solve_active(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           traits.box_x_hi != traits.box_x_lo &&
           (((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000) > 0;
}

void light_contact_solve(World &world, Entity &veh, const VehicleTraits &traits,
                         Entity::VehicleMotorState &m,
                         int32_t start_x, int32_t start_y,
                         int32_t &px, int32_t &py, int32_t &pz) {
    // ---- sleep fast-path [orig: @0x47972C..0x479790]: velocities/speed/
    // rates zero, not airborne, not carried, slideDecay in (-300,-1] (the
    // unsigned `> 0xFFFFFED4` compare — the bike window is 300, not the
    // tracked/wheeled 350), planar compare, energy zero, not crashed. Action
    // gated on the mover's frameFlags=1: undo the dribble and return — the
    // bike sleep has NO contact-byte refresh [orig: Z -= slideDecay
    // @0x47976B; slideDecay >>= 1 @0x479777].
    if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 &&
        m.wheel_rate_bam == 0 && (veh.flags & kEntityFlagInAir) == 0 &&
        m.slide_z > -300 && m.slide_z < 0 &&
        px == start_x && py == start_y) {
        pz -= m.slide_z;
        m.slide_z >>= 1;
        return;
    }

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

    const VehicleEulerBasis basis = vehicle_euler_basis(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
    int32_t probes[6][3];
    for (int i = 0; i < 6; ++i) {
        int32_t rotated[3];
        basis.q22.rotate_point(probes_model[i], rotated);
        probes[i][0] = px + rotated[0];
        probes[i][1] = py + rotated[1];
        probes[i][2] = pz + rotated[2];
    }

    // ---- the two force passes + severity [orig: Entity_CheckCollisionState
    // @0x479C2B/@0x479E7C]. Same torque sheds [orig: @0x479C89/@0x479CD1/
    // @0x479D0F]; the sev-3 authority damage (unitType-3 kill at 29300, the
    // mass<=3 light multiplier), scrape sound and momentum exchange are
    // deferred; the 0.25 cut scans probes 0..4 — the mid-hull probe is
    // excluded [orig: init from probe 0 @0x479DEB, `while (v61 < 5)`
    // @0x479E36-region].
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[6];
    int32_t sev = 0;
    for (int i = 0; i < 6; ++i)
        sev = std::max(sev, plat_terrain_probe(world, probes[i][0],
                                               probes[i][1], probes[i][2], r,
                                               soft, hard, forces[i]));
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
        if (ddx * ddx + ddy * ddy > int64_t(0x8000) * 0x8000)
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
        int32_t sev2 = 0;
        for (int i = 0; i < 6; ++i)
            sev2 = std::max(sev2, plat_terrain_probe(world, probes[i][0],
                                                     probes[i][1],
                                                     probes[i][2], r,
                                                     soft, hard, forces2[i]));
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
        px += int32_t(dX);
        py += int32_t(dY);
    }

    // ---- the in-water flag: the plain two-wheel average against the
    // waterline — the bike form has NO hysteresis and NO hull-bottom offset
    // [orig: `(z0 + z1) >> 1 >= WaterZ` @0x479F7C..0x479F90]. The per-wheel
    // water support rides the dispatcher arg (0 for cbik). Splash FX +
    // overlay sends deferred.
    if (world.env.water_z != 0) {
        if (((probes[0][2] + probes[1][2]) >> 1) >= world.env.water_z)
            veh.flags &= ~0x8000u;
        else
            veh.flags |= 0x8000u;
    }

    // ---- the any-contact speed halver [orig: `if (anyContact && |vel| >
    // 17580) currentSpeed >>= 1` @0x47A66E..0x47A683]: planar-force contact
    // at speed sheds half. (The head-on detector's authority damage/eject
    // ladder is deferred — a bike takes damage instead of the tank's wall
    // stop.)
    {
        bool any_contact = false;
        for (int i = 0; i < 6; ++i)
            if (forces[i].fx != 0 || forces[i].fy != 0) any_contact = true;
        if (any_contact) {
            const double vmag = std::sqrt(double(m.vel_x) * m.vel_x +
                                          double(m.vel_y) * m.vel_y +
                                          double(m.slide_z) * m.slide_z);
            if (vmag > 17580.0) m.speed >>= 1;
        }
    }

    // ---- solve select [orig: the `!d_front && !d_rear` split @0x47A985].
    const int32_t up_z16 = static_cast<int32_t>(basis.up[2] * 65536.0);
    const int32_t side_z16 = static_cast<int32_t>(basis.side[2] * 65536.0);
    if (d[0] == 0 && d[1] == 0) {
        // Both wheels off: the rear-contact run resets, airborne sets, and
        // the chassis call runs its airborne arm — the axle fit of the
        // UNLIFTED corners, an attitude identity [orig: the reset @0x47AA13;
        // Flags |= 0x2000 @0x47AC84; the crashed/parked Z-lift and the
        // spine-crash latch are deferred with the wreck machine].
        m.light_rear_contact_ticks = 0;
        veh.flags |= kEntityFlagInAir;
        m.plat_airborne_ticks += 1;
        return;
    }
    // ---- wheel contact: the 2-corner axle fit [orig: the grounded arm of
    // Entity_UpdateVehicleChassisOrientation @0x468B03..0x468D3B]. Corners
    // from the isSquare quad — ±half the inset foot length along the basis
    // forward row [orig: Entity_ComputeBoundingQuad @0x45B6E0 isSquare arm:
    // c0 = pos + 0.5*len*fwd, c1 = pos - 0.5*len*fwd] — lifted by the wheel
    // d's in the spring loop [orig: dest[2] += d0 / dest[5] += d1].
    veh.flags &= ~kEntityFlagInAir;
    // The bike's spring leg over its two wheel depths (the quadratic/fast
    // pair, the same calls as the tracked solve) [orig: the light solve's
    //  Suspension_CompressWheelQuadratic / OscillateWheelFast calls].
    {
        const int32_t wheel_depths[4] = {d[0], d[1], 0, 0};
        vehicle_suspension_latch(world, veh);
        vehicle_suspension_step(world, veh, traits, wheel_depths);
    }
    const int32_t half_len =
            ((traits.foot_x_hi - r) - (traits.foot_x_lo + r)) >> 1;
    int32_t cf[3], cr[3];
    for (int i = 0; i < 3; ++i) {
        const int32_t off = static_cast<int32_t>(
                (static_cast<int64_t>(half_len) *
                         static_cast<int32_t>(basis.fwd[i] * 65536.0) +
                 0x8000) >> 16);
        cf[i] = off;
        cr[i] = -off;
    }
    cf[0] += px; cf[1] += py; cf[2] += pz + d[0];
    cr[0] += px; cr[1] += py; cr[2] += pz + d[1];
    // fwd = the normalized axle line; right = normalize(old_up x fwd); up =
    // fwd x right — roll continuity against the previous up row
    // [orig: the cross/normalize chain @0x468BC1..0x468CE1]; then the
    // standard substitute Euler pair (the same convention plat_fit_corners
    // uses; Math_FixedPointMatrixToEulerAngles @0x613310 interior = the
    // shared pending witness).
    {
        const int64_t axle[3] = {int64_t(cf[0]) - cr[0], int64_t(cf[1]) - cr[1],
                                 int64_t(cf[2]) - cr[2]};
        int32_t fwd[3];
        q16_normalize(axle, fwd);
        const int32_t old_up[3] = {
            static_cast<int32_t>(basis.up[0] * 65536.0),
            static_cast<int32_t>(basis.up[1] * 65536.0),
            static_cast<int32_t>(basis.up[2] * 65536.0)};
        int64_t raw[3];
        q16_cross(old_up, fwd, raw);
        int32_t right[3];
        q16_normalize(raw, right);
        q16_cross(fwd, right, raw);
        int32_t up[3];
        q16_normalize(raw, up);
        const double fxy = std::sqrt(double(fwd[0]) * fwd[0] +
                                     double(fwd[1]) * fwd[1]);
        m.air_pitch_bam = bam_of_atan2(double(fwd[2]), fxy);
        m.air_roll_bam = bam_of_atan2(double(right[2]), double(up[2]));
        veh.pitch = static_cast<int16_t>(std::lround(
                double(m.air_pitch_bam) * kDegreesPerBam));
        veh.roll = static_cast<int16_t>(std::lround(
                double(m.air_roll_bam) * kDegreesPerBam));
    }
    // Z select: crashed/no-up/parked ride the latch machine; the live leg
    // clamps slideDecay non-positive and adopts the fit Z — the mean of the
    // two lifted wheel corners [orig: `slideDecay = min(slideDecay, 0);
    // Position.Z = out.z` @0x47AF52..0x47AF60; out.z = (c0.z + c1.z) * 0.5
    // via flt_7C3B94].
    if (up_z16 > 0) {
        if (m.slide_z > 0) m.slide_z = 0;
        pz = static_cast<int32_t>(
                (int64_t(cf[2]) + cr[2]) / 2);
    } else {
        int32_t maxd = 0;
        for (int i = 0; i < 5; ++i) maxd = std::max(maxd, d[i]);
        pz += maxd;
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
    m.grounded = up_z16 > 4096 && std::abs(side_z16) < 40960 &&
                 d[1] != 0 && m.light_rear_contact_ticks > 1;
    m.plat_airborne_ticks = 0;
}

} // namespace detail
} // namespace opennova::world
