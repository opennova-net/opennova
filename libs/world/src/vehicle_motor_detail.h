#pragma once

// Internal to libs/world's vehicle-motor TUs — not part of world/vehicle_motor.h.
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

} // namespace detail
} // namespace opennova::world
