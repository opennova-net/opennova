#pragma once

// Internal to engine/runtime/world's vehicle-motor TUs — not part of world/vehicle_motor.h.
// Split out of vehicle_motor.cpp (the oversize-TU ratchet).
//
// The shared platform-solve sub-contract — the x87 trig pair, the Q22 Euler
// basis, the probe placement, the bilinear terrain probe force, the severity
// speed response and the 4-normal plane fit — that the watercraft platform
// solve (vehicle_motor.cpp) and the aircraft, tracked, wheeled and light
// contact solves (vehicle_contact_solve.cpp) consume, plus the contact-solve
// entry points the movers call at their witnessed call sites.

#include <cmath>
#include <algorithm>
#include <cstdint>

#include <runtime/world/collision.h>
#include <runtime/world/vehicle_motor.h>
#include <base/io/bam.h>
#include <base/io/fixed.h>

namespace opennova::world {

// Analog steer scale: BAM/tick per axis unit [orig: @0x48b783 `(192426 * analogZ) >> 1`;
// the same 2^32/360/62 deg/s->BAM/tick constant the turn_rate parse uses].
// Shared by the ground/boat input block (vehicle_motor.cpp) and the air one
// (vehicle_motor_air.cpp).
constexpr int32_t kAnalogSteerScale = 192426;

// x86 SHL used by the vehicle angle/rate integrators: keep only the low
// 32 bits at every step, exactly like the retail register operation.
inline int32_t bam_shl_wrap(int32_t value, unsigned shift) {
    while (shift-- != 0) value = io::bam_dbl(value);
    return value;
}

// x86 IMUL low-dword result. The shared BAM helpers cover add/sub/shift/abs;
// this is the remaining multiply primitive needed by the aircraft rate caps.
inline int32_t bam_mul_wrap(int32_t lhs, int32_t rhs) {
    return static_cast<int32_t>(static_cast<uint32_t>(lhs) *
                                static_cast<uint32_t>(rhs));
}

// The occupant whose input this machine should consume (defined in
// vehicle_motor.cpp beside the controller resolve; the air TU consumes it).
// Retail's gate is `(occ->Flags & 0x100) && (occ == g_LocalPlayerEntity ||
// is_authority)` [orig: Entity_UpdateAircraftPhysics @0x490310 input gate; the
// ground twin is Entity_UpdateVehiclePhysics @0x48b0ff].
Entity *resolve_piloting_player(World &world, Entity &veh, const VehicleTraits &traits);

int32_t turn_pilot_view(World &, Entity &, int32_t delta);
void stage_player_vehicle_input(World &, Entity &, Entity &, const VehicleTraits &);
// The client chase template of each mover family (defined in vehicle_motor.cpp).
enum class VehicleChaseFamily : uint8_t { Plain, Ground, Bike, Tank };
void vehicle_client_chase(Entity &, VehicleChaseFamily family, int32_t entry_up_z16);

namespace detail {

// Shared physics=0 pad/spine suspension. [orig: Entity_ProcessVehicleSuspension @0x463C60]
void vehicle_simple_contact(
		World &, Entity &, const VehicleTraits &, int32_t &, int32_t &, int32_t &);

// Mover-entry ground-object refresh and carried pose; run before client chase.
// [orig: Entity_UpdateVehiclePhysics @0x48AF00; boat @0x48D480; air @0x490310]
int32_t vehicle_ground_height_at(
		World &world, Entity &vehicle, const VehicleTraits &traits, const int32_t position[3]);
void vehicle_refresh_ground_link(World &world, Entity &vehicle, const VehicleTraits &traits);
void vehicle_follow_carrier(World &world, Entity &vehicle);

// Boat/air family trig: retail computes THESE movers' sin/cos with x87
// fsin/fcos scaled by the verbatim BAM->radian constant dbl_7C3608 =
// 1.4629627251502471e-9 (~pi/0x7FF..., deliberately NOT the exact inverse of
// the atan2 scale dbl_7C19D8 — port both constants verbatim)
// [orig: fld dbl_7C3608 @0x48EB32/@0x4905BF/@0x492006; the ground family
// keeps the 1024-entry table equivalents in vehicle_motor.cpp (D-INF-4)].
constexpr double kBamToRadX87 = 1.4629627251502471e-9; // dbl_7C3608, exact bits
inline int32_t cos22_of_bam_x87(int32_t bam) {
    return static_cast<int32_t>(
            std::cos(static_cast<double>(bam) * kBamToRadX87) * io::kQ22One);
}
inline int32_t sin22_of_bam_x87(int32_t bam) {
    return static_cast<int32_t>(
            std::sin(static_cast<double>(bam) * kBamToRadX87) * io::kQ22One);
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
using PlatProbeForce = VehicleProbeForce;

int32_t plat_terrain_probe(const World &world, int32_t X, int32_t Y, int32_t Z, int32_t r,
		int32_t soft, int32_t hard, PlatProbeForce &out, bool wheel_probe = false);

// The probe placement every contact solve shares: each model-space probe
// rotated by the hull's Q22 euler basis, then offset by the hull position.
template <int N>
inline void place_probes(const VehicleEulerBasis &basis,
                         const int32_t (&probes_model)[N][3], int32_t px,
                         int32_t py, int32_t pz, int32_t (&probes)[N][3]) {
    for (int i = 0; i < N; ++i) {
        int32_t rotated[3];
        basis.q22.rotate_point(probes_model[i], rotated);
        probes[i][0] = px + rotated[0];
        probes[i][1] = py + rotated[1];
        probes[i][2] = pz + rotated[2];
    }
}

// Terrain and models share the authored probe array. The boat variant's
// terrain broad phase applies to every point; the other families always
// evaluate the first four wheel points. [orig: @0x462C13..0x462C45]
int32_t vehicle_probe_pass(World &world, Entity &vehicle, const int32_t (*probes)[3],
		const int32_t *radii, int count, int32_t soft, int32_t hard, PlatProbeForce *forces,
		int32_t px, int32_t py, int32_t pz, EntityHandle *hit_entity);

template <int N>
inline int32_t plat_probe_pass(World &world, Entity &vehicle, const int32_t (&probes)[N][3],
		const int32_t (&radii)[N], int32_t soft, int32_t hard, PlatProbeForce (&forces)[N],
		int32_t px, int32_t py, int32_t pz, EntityHandle *hit_entity = nullptr) {
	return vehicle_probe_pass(
			world, vehicle, probes, radii, N, soft, hard, forces, px, py, pz, hit_entity);
}

// The lighter contacted machine takes its share of the separation. Each
// family scales its own saved depth count (boat leaves its seventh depth).
// [orig: ground @0x47D336..0x47D458; boat @0x482957..0x482A7F]
void vehicle_contact_mass_share(World &world, const Entity &vehicle, EntityHandle hit, int32_t &dx,
		int32_t &dy, int32_t *depths, int count);

// Severity-three damage, scrape edge and momentum transfer. The caller has
// already applied its torque speed decay. [orig: @0x47CD00..0x47CF55]
void vehicle_landing_damage(
		World &, Entity &, const VehicleTraits &, const int32_t *depth, int count, int32_t up);
void vehicle_crush_damage(
		World &, Entity &, const VehicleTraits &, const int32_t *spine, int32_t up);

void vehicle_contact_impact(World &world, Entity &vehicle, const VehicleTraits &traits,
		int severity, EntityHandle hit, int32_t px, int32_t py, int32_t pz);

// The sev-3 0.25-cut distance gate every solve runs: the STRONGEST planar
// force among the first `scan` probes must sit > 0x8000 (0.5 u) from the
// hull position in the plane.
template <int N>
inline bool strongest_probe_beyond_hull(const PlatProbeForce (&forces)[N],
                                        const int32_t (&probes)[N][3], int scan,
                                        int32_t px, int32_t py) {
    int strongest = 0;
    int64_t best = -1;
    for (int i = 0; i < scan; ++i) {
        const int64_t sfx = forces[i].fx, sfy = forces[i].fy;
        const int64_t mag2 = sfx * sfx + sfy * sfy;
        if (mag2 > best) { best = mag2; strongest = i; }
    }
    const int64_t ddx = int64_t(probes[strongest][0]) - px;
    const int64_t ddy = int64_t(probes[strongest][1]) - py;
    return ddx * ddx + ddy * ddy > int64_t(0x8000) * 0x8000;
}

// The speed response every contact solve runs on its first-pass severity:
// severities 1 and 3 decay the speed by >> (torque + 2), severity 2 by
// >> (torque + 1); a terrain-only severity 3 whose strongest planar probe among
// the first `scan` lies beyond the hull (strongest_probe_beyond_hull) then cuts
// the speed to one quarter (flt_7C333C). The impact fold (damage, sound,
// momentum) is vehicle_contact_impact's. Each solve cites its own sites.
template <int N>
inline void contact_speed_response(Entity::VehicleMotorState &m, const VehicleTraits &traits,
                                   int32_t sev, EntityHandle hit_entity,
                                   const PlatProbeForce (&forces)[N],
                                   const int32_t (&probes)[N][3], int scan, int32_t px,
                                   int32_t py) {
    if (sev == 1) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31);
    } else if (sev == 2) {
        m.speed -= m.speed >> ((traits.torque + 1) & 31);
    } else if (sev == 3) {
        m.speed -= m.speed >> ((traits.torque + 2) & 31);
        if (!hit_entity.valid() && strongest_probe_beyond_hull(forces, probes, scan, px, py))
            m.speed = int32_t(m.speed * 0.25);
    }
}

// The shared second contact pass averages forces after the planar probe shift and applies the
// contacted entity mass share. Boat keeps its depth buffer locally because the grounded branch
// consumes it.
template <int N>
inline void plat_second_pass(World &world, Entity &vehicle, int32_t (&probes)[N][3],
		const int32_t (&radii)[N], int32_t soft, int32_t hard, PlatProbeForce (&forces)[N],
		int32_t (&d)[N], int32_t &px, int32_t &py, int32_t pz, EntityHandle hit) {
	int64_t dX = 0, dY = 0;
	for (int i = 0; i < N; ++i) {
		dX += forces[i].fx;
		dY += forces[i].fy;
	}
	for (int i = 0; i < N; ++i) {
        probes[i][0] += int32_t(dX);
        probes[i][1] += int32_t(dY);
    }
    PlatProbeForce forces2[N];
	const int32_t sev2 =
			plat_probe_pass(world, vehicle, probes, radii, soft, hard, forces2, px, py, pz);
	for (int i = 0; i < N; ++i)
		forces[i].terrain_gap = forces2[i].terrain_gap;
	if (sev2 != 0) {
		int64_t dX2 = 0, dY2 = 0;
		for (int i = 0; i < N; ++i) {
            dX2 += forces2[i].fx;
            dY2 += forces2[i].fy;
        }
        for (int i = 0; i < N; ++i) d[i] = (forces2[i].fz + d[i]) >> 1;
        dX = (dX2 + dX) >> 1;
        dY = (dY2 + dY) >> 1;
	}
	int32_t dx = int32_t(dX), dy = int32_t(dY);
	vehicle_contact_mass_share(world, vehicle, hit, dx, dy, d, N);
	px += dx;
	py += dy;
}

// The in-water flag with the r/2 hysteresis shared by the contact solves:
// the four pad probes' average Z (lowered by r/2 while already in water)
// against the hull-bottom reference and the world water plane. W == 0 = our
// no-water-world sentinel (retail worlds always carry a plane).
void vehicle_water_entry(World &, Entity &, int32_t x, int32_t y, bool was_water);
void vehicle_trail_water_transition(Entity &e, const VehicleTraits &traits, bool was_water);
void vehicle_sample_trails(
		World &world, Entity &e, const VehicleTraits &traits, int32_t target_speed);
void vehicle_update_trail_lane(
		World &world, Entity &e, const VehicleTraits &traits, uint8_t lane, int32_t intensity);
template <int N>
inline void plat_water_flag(World &world, Entity &veh, const VehicleTraits &traits,
		const int32_t (&probes)[N][3], int32_t r, int32_t hull_bottom_neg, int32_t water_z) {
	if (water_z == 0)
		return;
	const bool was_water = (veh.flags & 0x8000u) != 0;
	int32_t avg = (probes[0][2] + probes[1][2] + probes[2][2] + probes[3][2]) >> 2;
	if ((veh.flags & 0x8000u) != 0u)
		avg -= r >> 1;
	if (hull_bottom_neg + avg >= water_z)
        veh.flags &= ~0x8000u;
    else
        veh.flags |= 0x8000u;
	vehicle_trail_water_transition(veh, traits, was_water);
	int lowest = 0;
	for (int i = 1; i < 4; ++i)
		if (probes[i][2] < probes[lowest][2])
			lowest = i;
	vehicle_water_entry(world, veh, probes[lowest][0], probes[lowest][1], was_water);
}

// The retained contact direction joins the contact solve to next tick's
// acceleration, velocity, wheelspin and skid effects.
void vehicle_health_effects(World &world, Entity &e, const VehicleTraits &traits, bool critical);
void vehicle_release_damage_effects(World &world, Entity &e);
void vehicle_smoke_effect(World &world, Entity &e, bool release = false);
void vehicle_crash_state(
		World &world, Entity &e, const VehicleTraits &traits, const bool *contacts, int32_t up_z16);
void vehicle_expire_contact_wake(World &, Entity &);
void vehicle_kill_crash_occupants(World &, Entity &);
void vehicle_rebuild_rest_orientation(World &, Entity &, bool inverted);
void vehicle_rest_state(World &, Entity &, VehicleFamily);
void vehicle_emit_skid_effects(World &, Entity &, const VehicleTraits &);
bool vehicle_has_contact_direction(const Entity::VehicleMotorState &m);
bool vehicle_traction_acceleration(
		World &world, Entity &vehicle, const VehicleTraits &traits, int32_t target_speed);
void vehicle_wheel_traction_tick(
		World &world, Entity &vehicle, const VehicleTraits &traits, int32_t target_speed);
bool vehicle_traction_velocity(
		World &world, Entity &vehicle, const VehicleTraits &traits, int32_t target_speed);
void vehicle_capture_contact_direction(Entity &vehicle, const VehicleEulerBasis &basis);
void vehicle_contact_downhill_tail(Entity &vehicle, int32_t pz);

// The shared 4-normal plane fit [orig: identical in both solvers —
// Entity_ComputeSuspensionOrientation @0x46CAB7.. / the wheeled twin
// @0x46BAF9..; verbatim ASYMMETRIC aggregation, blockers §1]: rows from the
// 4 lever-corner targets; Z = the plain corner average [orig: solvedPos.Z
// @0x46E099..0x46E0B3]. Outputs pitch/roll BAM via the standard atan2
// decomposition (the @0x613310 interior = pending witness).
struct PlatFit {
	int32_t yaw_bam = 0;
	int32_t pitch_bam = 0;
	int32_t roll_bam = 0;
	int32_t z_avg = 0;          // plain 4-corner average (orientation solver)
    int32_t positive_z_avg = 0; // corners with z > 0 only (wheeled solver leg C)
    double fwd_z = 0.0; // unit forward vertical component (beach term feed)
};

// Assemble the three Q16 axes into the solver's Q22 matrix for extraction.
// [orig: Math_FixedPointMatrixToEulerAngles @0x613310]
inline void vehicle_axes_to_euler(const int32_t forward[3], const int32_t side[3],
		const int32_t up[3], int32_t &yaw, int32_t &pitch, int32_t &roll) {
	CollisionMatrix matrix;
	for (int row = 0; row < 3; ++row) {
		matrix.m[4 * row] = bam_shl_wrap(forward[row], 6);
		matrix.m[4 * row + 1] = bam_shl_wrap(side[row], 6);
		matrix.m[4 * row + 2] = bam_shl_wrap(up[row], 6);
	}
	int32_t euler[3];
	collision_matrix_to_euler(matrix, euler);
	yaw = euler[0];
	pitch = euler[1];
	roll = euler[2];
}

void plat_fit_corners(const int32_t c[4][3], PlatFit &out, CollisionMatrix *matrix = nullptr);
bool vehicle_suspension_fit(World &, Entity &, int32_t corners[4][3], const bool *contacts,
		PlatFit &, int32_t px, int32_t py, int32_t pz, bool tank = false);
void vehicle_boat_suspension_fit(World &, Entity &, const VehicleTraits &, int32_t corners[4][3],
		PlatFit &, int32_t px, int32_t py, int32_t pz);
void vehicle_boat_lean(Entity &, const VehicleTraits &, int32_t side_z16);
void vehicle_bike_lean(Entity &, const VehicleTraits &, int32_t side_z16);
void vehicle_apply_lean(Entity &, CollisionMatrix &);
void vehicle_clear_chassis(Entity::VehicleMotorState &);
void vehicle_recoil_impulse(
		World &, Entity &, const VehicleTraits &, int32_t amplitude, const int32_t direction[3]);
void vehicle_apply_chassis(World &, Entity &, CollisionMatrix &);
void vehicle_clear_chassis_forces(Entity &, const int32_t corners[4][3], int mode);
// [orig: Entity_QueueSuspensionForce @0x45C0B0]
void vehicle_queue_suspension_force(
		Entity::VehicleMotorState &, int channel, int32_t rate, const int32_t direction[3]);
// The crashed tank's track-strike forces and tumble cue. `support` holds the
// four wheel depths the retail call reads at record offset +8.
// [orig: Entity_ApplyWheelSuspensionForces @0x463560]
void vehicle_apply_wheel_suspension_forces(World &, Entity &, const VehicleTraits &,
		bool has_contact, const int32_t *support, const CollisionMatrix *matrix);

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
