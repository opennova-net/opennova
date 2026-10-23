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
#include <runtime/world/entity.h> // retail_q16_mul_rhu, the round-half-up 16.16 product
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
// The movers' MoveOrder 0x10 merge onto an occupant (defined in vehicle_motor.cpp).
void merge_occupant_free_look(World &, Entity &occupant);
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
constexpr double kBamToRadX87 = io::kRadiansPerBamX87; // dbl_7C3608, exact bits
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
    r[0] = int64_t(retail_q16_mul_rhu(a[1], b[2])) - retail_q16_mul_rhu(a[2], b[1]);
    r[1] = int64_t(retail_q16_mul_rhu(a[2], b[0])) - retail_q16_mul_rhu(a[0], b[2]);
    r[2] = int64_t(retail_q16_mul_rhu(a[0], b[1])) - retail_q16_mul_rhu(a[1], b[0]);
}

// The retail Q22 Rz(yaw)*Ry(-pitch)*Rx(roll) builder shared with collision and
// bone transforms (defined beside the platform solve in vehicle_motor.cpp).
VehicleEulerBasis vehicle_euler_basis(int32_t yaw_bam, int32_t pitch_bam,
                                      int32_t roll_bam);
// The same basis over a matrix a solve built otherwise (its rows read as Q22).
VehicleEulerBasis vehicle_basis_from_matrix(const CollisionMatrix &q22);

// The head matrix the tracked, wheeled, light, aircraft and platform solves build from the hull's
// euler words: through the scaled builder when the hull has an effective scale (entity +0x158,
// else its def's +0x1B8, which ours folds into uniform_scale_q16 at spawn), the plain builder
// otherwise. Every probe of the solve is transformed through it (place_probes).
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - @0x47C58B..0x47C5E2;
//  Entity_ProcessWheeledVehiclePhysics @0x475DE0 - @0x4761F9..0x476250, the probe transforms
//  @0x4764E0..0x47660B; Entity_ProcessLightVehiclePhysics @0x479600 - @0x47985D..0x4798B4, the
//  probe transforms @0x4799B2..0x479BFF; Entity_ProcessAircraftContactPhysics @0x47EF10 -
//  @0x47F28E..0x47F2E5; Entity_ProcessPlatformPhysics @0x481870 - @0x481BEE..0x481C45;
//  Math_BuildFixedPointRotationMatrixFromEulerAnglesAndScale @0x614210;
//  Math_BuildFixedPointMatrixFromEulerAngles @0x613F40]
CollisionMatrix vehicle_solve_matrix(const Entity &veh);
// The occupant matrix a solve sets from its head matrix's rows: each row extracted (`sar 6`) and
// set back (`shl 6`), so every rotation word loses its low six bits; the light solve reads its up
// and side words, every row and its chassis calls' matrix from it.
// [orig: Entity_ProcessLightVehiclePhysics @0x479600 - the extracts
//  Math_ExtractRow1FromFixedPoint22 @0x6137D0 (the call @0x4798C4),
//  Math_ExtractRow0FromFixedPoint22 @0x613770 (@0x4798D6), Math_ExtractRow2FromFixedPoint22
//  @0x6137A0 (@0x4798E8), the sets Math_SetRow1FromVec3Scaled @0x613880 (@0x4798FA),
//  Math_SetRow0FromVec3Scaled @0x613820 (@0x47990C), Math_SetRow2FromVec3Scaled @0x613850
//  (@0x47991E)]
CollisionMatrix vehicle_matrix_rows(const CollisionMatrix &head);
// The tracked and wheeled solves' crash tests' up word: the head matrix's row 2 extract
// normalized on the x87 (the length of the three squares' sum, each component times 65536.0f
// over it, truncated; a zero length all 0), its z. The tests dot it with the world up (0, 0, 0x10000),
// whose x operand is a zeroed local, so the dot is the z. Both solves normalize all three
// extracts into their occupant matrix, but rebuild the matrix unscaled from the euler words before
// any other reader, so only the tests see a normalized word.
// [orig: Entity_ProcessWheeledVehiclePhysics @0x475DE0 - the normalize @0x476366..0x4763CC
//  (flt_7C32BC loaded @0x4762B2, `fdivrp` @0x476391), the world up var_418 / var_414 / var_410 =
//  0 / 0 / 0x10000 @0x4760F5..0x4760FD, the dot @0x4776DB..0x47773D;
//  Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - the normalize @0x47C72F..0x47C795, the world
//  up @0x47C486..0x47C494, the dot @0x47D6BF..0x47D722; the normalize form is q16_normalize's
//  (Q3-11)]
int32_t vehicle_crash_test_up(const CollisionMatrix &head);

// One bilinear terrain probe force [orig: Entity_ComputeCollisionForces
// @0x462150, terrain loop @0x462246..0x4624CA]: 4 samples at ±r, gradient
// force, penetration, slope classing against soft/hard cos22 thresholds.
using PlatProbeForce = VehicleProbeForce;

int32_t plat_terrain_probe(const World &world, int32_t X, int32_t Y, int32_t Z, int32_t r,
		int32_t soft, int32_t hard, PlatProbeForce &out, bool wheel_probe = false);

// The probe placement every contact solve shares: each model-space probe
// rotated by the solve's Q22 head matrix (vehicle_solve_matrix: the effective
// scale included), then offset by the hull position.
template <int N>
inline void place_probes(const CollisionMatrix &matrix,
                         const int32_t (&probes_model)[N][3], int32_t px,
                         int32_t py, int32_t pz, int32_t (&probes)[N][3]) {
    for (int i = 0; i < N; ++i) {
        int32_t rotated[3];
        matrix.rotate_point(probes_model[i], rotated);
        probes[i][0] = px + rotated[0];
        probes[i][1] = py + rotated[1];
        probes[i][2] = pz + rotated[2];
    }
}

// Which of retail's two force passes a solve calls. They differ in the terrain broad phase only:
// a probe whose four samples all lie more than its radius below it is skipped.
// - CheckCollisionState (the tracked, wheeled and light solves) skips only past the
//   first four, the wheel probes, which it always evaluates.
//   [orig: Entity_CheckCollisionState @0x462A30, the `wheelIdx < 4` test @0x462C13..0x462C45]
// - ComputeCollisionForces (the aircraft, platform and physics-0 suspension solves) skips
//   every probe: each call passes no per-wheel array, and the skip tests that array alone.
//   [orig: Entity_ComputeCollisionForces @0x462150, the skip @0x4622D2; the null
//    per_wheel_distances pushes of Entity_ProcessAircraftContactPhysics @0x47F855 / @0x47FE13
//    and Entity_ProcessVehicleSuspension @0x4640AB / @0x464638, and the null argument at
//    both Entity_ProcessPlatformPhysics calls @0x4820A4 / @0x482818]
enum class ProbeForcePass : uint8_t { CheckCollisionState, ComputeCollisionForces };

// Terrain and models share the authored probe array.
int32_t vehicle_probe_pass(World &world, Entity &vehicle, ProbeForcePass pass,
		const int32_t (*probes)[3], const int32_t *radii, int count, int32_t soft, int32_t hard,
		PlatProbeForce *forces, int32_t px, int32_t py, int32_t pz, EntityHandle *hit_entity);

template <int N>
inline int32_t plat_probe_pass(World &world, Entity &vehicle, ProbeForcePass pass,
		const int32_t (&probes)[N][3], const int32_t (&radii)[N], int32_t soft, int32_t hard,
		PlatProbeForce (&forces)[N], int32_t px, int32_t py, int32_t pz,
		EntityHandle *hit_entity = nullptr) {
	return vehicle_probe_pass(
			world, vehicle, pass, probes, radii, N, soft, hard, forces, px, py, pz, hit_entity);
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

// The shared second contact pass, after the planar probe shift by the first `push_count` probes'
// summed forces (the light solve leaves its mid-hull probe out). The probe records keep pass two
// verbatim: retail's second call writes the same record buffer, so their depths are pass two's
// whatever it found; the per-probe wall bytes it only ever sets keep pass one's. The separate
// depth copy `d` averages with pass two only when pass two classified a contact, then takes the
// contacted entity's mass share. Each solve reads both views at its own sites. The share's entity
// is the hit slot: the tracked and light solves hand their second call the same slot, which that
// call resets and rewrites (`pass_two_hit_slot`: pass two's hit sizes the share); the aircraft
// hands its second call none (pass one's hit). Boat keeps its depth buffer locally because the
// grounded branch consumes it.
// [orig: Entity_ProcessAircraftContactPhysics @0x47EF10 - the second Entity_ComputeCollisionForces
//  into the records with a null hit slot @0x47FED0..0x47FF11, the average into the copy
//  @0x47FF1F..0x48002B, the mass share on pass one's hit (ebx from @0x47F8A8) @0x480032..0x48013C;
//  Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - the second Entity_CheckCollisionState into the
//  records (`lea eax, [esp+..+var_1CC]` @0x47D1CC) and the hit slot (`lea edx, [esp+..+var_274]`
//  @0x47D1C7), the call @0x47D209, the average @0x47D217..0x47D329, the mass share reading that
//  slot @0x47D330; Entity_ProcessLightVehiclePhysics @0x479600 - the five-probe push
//  @0x47A20F..0x47A2A8, the second call @0x47A2F6, the average @0x47A30A..0x47A3F3, the mass share
//  @0x47A3FA..0x47A511; Entity_CheckCollisionState @0x462A30 zeroes the records
//  @0x462A5D..0x462A71, resets its hit slot @0x462AA7..0x462AB2 and only sets the wall bytes
//  @0x46326E]
template <int N>
inline void plat_second_pass(World &world, Entity &vehicle, ProbeForcePass pass,
		int32_t (&probes)[N][3], const int32_t (&radii)[N], int32_t soft, int32_t hard,
		PlatProbeForce (&forces)[N], int32_t (&d)[N], int32_t &px, int32_t &py, int32_t pz,
		EntityHandle &hit, bool pass_two_hit_slot, int push_count = N) {
	int64_t dX = 0, dY = 0;
	for (int i = 0; i < push_count; ++i) {
		dX += forces[i].fx;
		dY += forces[i].fy;
	}
	for (int i = 0; i < N; ++i) {
        probes[i][0] += int32_t(dX);
        probes[i][1] += int32_t(dY);
    }
    PlatProbeForce forces2[N];
	const int32_t sev2 = plat_probe_pass(world, vehicle, pass, probes, radii, soft, hard, forces2,
			px, py, pz, pass_two_hit_slot ? &hit : nullptr);
	for (int i = 0; i < N; ++i) {
		const bool wall = forces[i].wall_contact || forces2[i].wall_contact;
		forces[i] = forces2[i];
		forces[i].wall_contact = wall;
	}
	if (sev2 != 0) {
		int64_t dX2 = 0, dY2 = 0;
		for (int i = 0; i < push_count; ++i) {
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
// against the hull-bottom reference and the world water word, compared raw: a
// map with no water authored keeps the word 0, a plane at Z 0. [orig:
// Entity_ProcessTrackedVehiclePhysics @0x47C1C0 (@0x47D516..0x47D53A, `cmp edx, ebx;
// jge`); Entity_ProcessAircraftContactPhysics @0x47EF10 (the average and hysteresis
// @0x48021A..0x480226, the compare @0x480239..0x48023B);
// Environment_InitDefaults @0x57C010 (@0x57C011..0x57C01E)]
void vehicle_water_entry(World &, Entity &, int32_t x, int32_t y, bool was_water);
void vehicle_trail_water_transition(Entity &e, const VehicleTraits &traits, bool was_water);
void vehicle_sample_trails(
		World &world, Entity &e, const VehicleTraits &traits, int32_t target_speed);
void vehicle_update_trail_lane(
		World &world, Entity &e, const VehicleTraits &traits, uint8_t lane, int32_t intensity);
template <int N>
inline void plat_water_flag(World &world, Entity &veh, const VehicleTraits &traits,
		const int32_t (&probes)[N][3], int32_t r, int32_t hull_bottom_neg, int32_t water_z) {
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
// `depths` is the aircraft's or tracked solve's contact copy (seven depths); the other families
// pass none.
void vehicle_crash_state(
		World &world, Entity &e, const VehicleTraits &traits, const int32_t *depths, int32_t up_z16);
void vehicle_flush_sinks(World &world, Entity &e, int32_t up_z16, int32_t pz);
void vehicle_expire_contact_wake(World &, Entity &);
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
	// The suspension call's output matrix, the one its euler words come from: the aircraft
	// solve's parked arm reads its rows 0 and 2 after the call [orig:
	// Entity_ProcessAircraftContactPhysics @0x47EF10, the `matrix` handed to
	// Entity_ProcessWheeledVehicleSuspension @0x481475 and read @0x48157C..0x4815A0].
	CollisionMatrix matrix;
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
// The non-square bounding quad over a width (along the side row) and a length (along the
// forward row), each halved inside: c0 (+f, +s), c1 (+f, -s), c2 (-f, -s), c3 (-f, +s).
// [orig: Entity_ComputeBoundingQuad @0x45B6E0, the non-square arm]
void vehicle_bounding_quad(const CollisionMatrix &matrix, int32_t width, int32_t length,
		int32_t x, int32_t y, int32_t z, int32_t out[4][3]);
// The square bounding quad: c0 / c1 the origin plus / minus half the larger |width| / |length|
// along `axis`, c2 / c3 their copies lifted by half |z_span| along the entity's euler matrix's
// row 2: c2 (-axis, lifted), c3 (+axis, lifted).
// [orig: Entity_ComputeBoundingQuad @0x45B6E0, the square arm]
void vehicle_square_quad(const CollisionMatrix &euler, int32_t z_span, int32_t width,
		int32_t length, const int32_t axis[3], int32_t x, int32_t y, int32_t z, int32_t out[4][3]);
// Fold the corner quad, each corner moved by its live force record, into the retained chassis
// matrix. [orig: Entity_ComputeChassisOrientation @0x463940]
void vehicle_compute_chassis_orientation(Entity &, const int32_t corners[4][3], int mode);
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
// The crashed tracked hull's strike forces: the same with the lighter press and lift.
// [orig: Entity_ApplyTrackedVehicleSuspensionForces @0x463750]
void vehicle_apply_tracked_suspension_forces(World &, Entity &, const VehicleTraits &,
		bool has_contact, const int32_t *support, const CollisionMatrix *matrix);

// The air + ground contact/suspension solves (vehicle_contact_solve.cpp) — the
// client-executed subsets of Entity_ProcessAircraftContactPhysics
// [orig: @0x47EF10] and Entity_ProcessTrackedVehiclePhysics [orig: @0x47C1C0].
// The box predicates mark a model whose collision block holds no box extents,
// which keeps the terrain-clamp stand-in; a model-less hull runs each solve's
// prologue only.
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
// retail bail is the graphicModel NULL return @0x4760BF..0x47610C -> @0x476016,
// past the sleep leg's rest-state rows @0x475fe2).
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
// The bike's live wall leg: any wall byte from either pass halves a speed past 17580; retail's
// head-on tiers read wall bytes nothing writes (D-NET-NEW-L6-23).
// [orig: Entity_ProcessLightVehiclePhysics @0x479600, @0x47AE05..0x47B1A9]
void light_wall_speed_halving(Entity::VehicleMotorState &m, const PlatProbeForce (&forces)[6]);
// The upright grounded bike's landing compression: a wheel clear of the terrain by its probe's
// gap gives it back from its compression (floored at 0) and re-derives its extension, while the
// handbrake latch holds or the contact direction is nonzero (D-NET-NEW-L6-27).
// [orig: Entity_ProcessLightVehiclePhysics @0x479600, @0x47BC3D..0x47BCF3]
void light_landing_compression(Entity::VehicleMotorState &m, const PlatProbeForce (&forces)[6]);
void light_contact_solve(World &world, Entity &veh, const VehicleTraits &traits,
                         Entity::VehicleMotorState &m,
                         int32_t start_x, int32_t start_y,
                         int32_t &px, int32_t &py, int32_t &pz);

} // namespace detail
} // namespace opennova::world
