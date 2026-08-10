#pragma once

// Internal to engine/runtime/world's vehicle-motor TUs — not part of world/vehicle_motor.h.
// Split out of vehicle_motor.cpp (the oversize-TU ratchet). Motion only — every
// body is unchanged, and each original-code citation moved with the code it
// annotates.
//
// The shared platform-solve sub-contract — the x87 trig pair, the Q22 Euler
// basis, the bilinear terrain probe force, and the 4-normal plane fit — that
// the watercraft platform solve (vehicle_motor.cpp) and the air/ground contact
// solves (vehicle_contact_solve.cpp) both consume, plus the contact-solve entry
// points the movers call at their witnessed call sites.

#include <cmath>
#include <cstdint>

#include "world/collision.h"
#include "world/vehicle_motor.h"

namespace opennova::world {

namespace detail {

// Boat/air family trig: retail computes THESE movers' sin/cos with x87
// fsin/fcos scaled by the verbatim BAM->radian constant dbl_7C3608 =
// 1.4629627251502471e-9 (~pi/0x7FF..., deliberately NOT the exact inverse of
// the atan2 scale dbl_7C19D8 — port both constants verbatim)
// [orig: fld dbl_7C3608 @0x48EB32/@0x4905BF/@0x492006; the ground family
// keeps the 1024-entry table equivalents in vehicle_motor.cpp (D-INF-4)].
constexpr double kBamToRadX87 = 1.4629627251502471e-9; // dbl_7C3608, exact bits
inline int32_t cos22_of_bam_x87(int32_t bam) {
    return static_cast<int32_t>(
            std::cos(static_cast<double>(bam) * kBamToRadX87) * 4194304.0);
}
inline int32_t sin22_of_bam_x87(int32_t bam) {
    return static_cast<int32_t>(
            std::sin(static_cast<double>(bam) * kBamToRadX87) * 4194304.0);
}

struct VehicleEulerBasis {
    CollisionMatrix q22;
    double fwd[3] = {};
    double side[3] = {};
    double up[3] = {};
};

// atan2 in this engine's BAM convention: radians * 2^31/pi, with the exact
// binary constant (the pair with the sin/cos scale is deliberately NOT an
// exact inverse — port both verbatim) [orig: dbl_7C19D8 = 683565275.5764316].
inline int32_t bam_of_atan2(double y, double x) {
    if (y == 0.0 && x == 0.0) return 0; // fpatan(0,0) == 0
    return static_cast<int32_t>(std::atan2(y, x) * 683565275.5764316);
}

// Round-half-up 16.16 product [orig: the `imul; add 0x8000; adc; shrd 16`
// idiom every solver product uses].
inline int32_t q16_mul_rhu(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b + 0x8000) >> 16);
}

// The witnessed solver normalize: `v * 65536.0 / sqrt(dot)` in x87 double,
// EACH COMPONENT ftol'd back to a 16.16 int; zero length -> (0,0,0)
// [orig: 0x46C9BA..0x46CA14 pattern, repeated at every edge/normal site].
inline void q16_normalize(const int64_t v[3], int32_t out[3]) {
    const double n = std::sqrt(double(v[0]) * double(v[0]) +
                               double(v[1]) * double(v[1]) +
                               double(v[2]) * double(v[2]));
    if (n <= 0.0) { out[0] = out[1] = out[2] = 0; return; }
    out[0] = static_cast<int32_t>(double(v[0]) * 65536.0 / n);
    out[1] = static_cast<int32_t>(double(v[1]) * 65536.0 / n);
    out[2] = static_cast<int32_t>(double(v[2]) * 65536.0 / n);
}

// Cross of two quantized 16.16 vectors, each term a round-half-up product.
inline void q16_cross(const int32_t a[3], const int32_t b[3], int64_t r[3]) {
    r[0] = int64_t(q16_mul_rhu(a[1], b[2])) - q16_mul_rhu(a[2], b[1]);
    r[1] = int64_t(q16_mul_rhu(a[2], b[0])) - q16_mul_rhu(a[0], b[2]);
    r[2] = int64_t(q16_mul_rhu(a[0], b[1])) - q16_mul_rhu(a[1], b[0]);
}

// The retail Q22 Rz(yaw)*Ry(-pitch)*Rx(roll) builder shared with collision and
// bone transforms (defined beside the platform solve in vehicle_motor.cpp).
VehicleEulerBasis vehicle_euler_basis(int32_t yaw_bam, int32_t pitch_bam,
                                      int32_t roll_bam);

// One bilinear terrain probe force [orig: Entity_ComputeCollisionForces
// @0x462150, terrain loop @0x462246..0x4624CA]: 4 samples at ±r, gradient
// force, penetration, slope classing against soft/hard cos22 thresholds.
struct PlatProbeForce {
    int32_t fx = 0, fy = 0, fz = 0; // force[i]; fz = push-up (-pen)
};

int32_t plat_terrain_probe(const World &world, int32_t X, int32_t Y, int32_t Z,
                           int32_t r, int32_t soft, int32_t hard,
                           PlatProbeForce &out);

// The shared 4-normal plane fit [orig: identical in both solvers —
// Entity_ComputeSuspensionOrientation @0x46CAB7.. / the wheeled twin
// @0x46BAF9..; verbatim ASYMMETRIC aggregation, blockers §1]: rows from the
// 4 lever-corner targets; Z = the plain corner average [orig: solvedPos.Z
// @0x46E099..0x46E0B3]. Outputs pitch/roll BAM via the standard atan2
// decomposition (the @0x613310 interior = pending witness).
struct PlatFit {
    int32_t pitch_bam = 0;
    int32_t roll_bam = 0;
    int32_t z_avg = 0;          // plain 4-corner average (orientation solver)
    int32_t positive_z_avg = 0; // corners with z > 0 only (wheeled solver leg C)
    double fwd_z = 0.0; // unit forward vertical component (beach term feed)
};

void plat_fit_corners(const int32_t c[4][3], PlatFit &out);

// The air + ground contact/suspension solves (vehicle_contact_solve.cpp) — the
// client-executed subsets of Entity_ProcessAircraftContactPhysics
// [orig: @0x47EF10] and Entity_ProcessTrackedVehiclePhysics [orig: @0x47C1C0].
// The shared activity predicates keep each mover and its solve agreeing about
// who owns Z and the flags (boxless rows take the terrain-clamp stand-in).
bool air_contact_solve_active(const VehicleTraits &traits);
void aircraft_contact_solve(World &world, Entity &veh,
                            const VehicleTraits &traits,
                            Entity::VehicleMotorState &m,
                            int32_t start_x, int32_t start_y,
                            int32_t &px, int32_t &py, int32_t &pz);
bool ground_contact_solve_active(const VehicleTraits &traits);
void ground_contact_solve(World &world, Entity &veh, const VehicleTraits &traits,
                          Entity::VehicleMotorState &m,
                          int32_t start_x, int32_t start_y,
                          int32_t &px, int32_t &py, int32_t &pz);

// The wheeled (ctan/tank) contact + suspension solve — the client-executed
// subset of Entity_ProcessWheeledVehiclePhysics [orig: @0x475DE0; the live
// caller is the tank mover @0x488AB0, call @0x48a9ef, water arg 0 via the
// ctank dispatcher @0x48f004]. 13 probes: 4 wheel pads + 6 belly stations +
// 3 spine. Shares ground_contact_solve_active (same box/beam predicate; the
// retail bail is the same graphicModel NULL return @0x475fe2-region).
void wheeled_contact_solve(World &world, Entity &veh,
                           const VehicleTraits &traits,
                           Entity::VehicleMotorState &m,
                           int32_t start_x, int32_t start_y,
                           int32_t &px, int32_t &py, int32_t &pz);

// The light (cbik/bike) contact + suspension solve — the client-executed
// subset of Entity_ProcessLightVehiclePhysics [orig: @0x479600; sole caller
// the cbik mover @0x483FE0, call @0x486672, water arg 0 via the cbike
// dispatcher @0x48eff4, frameFlags 1]. 6 probes on the hull centerline: 2
// wheels + 3 spine + 1 mid-hull; the 2-corner axle fit replaces the quad fit
// [orig: Entity_UpdateVehicleChassisOrientation @0x468A50].
bool light_contact_solve_active(const VehicleTraits &traits);
void light_contact_solve(World &world, Entity &veh, const VehicleTraits &traits,
                         Entity::VehicleMotorState &m,
                         int32_t start_x, int32_t start_y,
                         int32_t &px, int32_t &py, int32_t &pz);

} // namespace detail
} // namespace opennova::world
