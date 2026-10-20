#include "carrier_motion.h"
// Throwables: the thrown-round class motors, placed-device conversion, and the
// device think/detonate chain. Witness record: docs/world/world-wac-ai-re.md §27
// (engine-research 2026-07-20, retail Jointops.exe kong IDB).
#include <base/io/bam.h>
#include <base/io/rotating_prng.h>
#include <runtime/world/throwables.h>

#include <cmath>
#include <cstring>

#include <base/io/strutil.h>
#include <formats/def/def.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/fire_sound.h> // play_round_impact_sound
#include <runtime/world/physics_class_table.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>
#include <base/io/crt_ftol.h>
#include <base/io/fixed.h>

namespace opennova::world {

namespace {

constexpr double kPi = io::kPi;
constexpr double kBamPerRad = io::kBamPerRadian;

// Q16 multiply with the witnessed +0x8000 rounding [orig: the pervasive
// (a * b + 0x8000) >> 16 idiom in every motor].
inline int32_t qmul(int64_t a, int64_t b) {
    return static_cast<int32_t>((a * b + 0x8000) >> 16);
}

} // namespace

int32_t throwable_item_for_viewer(int32_t friendly_item, int32_t enemy_item,
                                  uint8_t item_team, uint8_t viewer_team) {
    // Enemy selection falls back to the friendly item when no foe TrcrID was
    // authored [orig: RoundData_SpawnRound @ 0x4ec787..0x4ec79d].
    if (item_team != viewer_team && enemy_item != 0) return enemy_item;
    return friendly_item;
}

bool throwable_surface_accepts_stick(const FixedVec3 &normal_q16) {
    // Retail tests the face normal, not the reflected velocity: nz must point
    // upward and exceed half the horizontal normal magnitude
    // [orig: satchel @ 0x448858..0x4488c4; claymore @ 0x447802..0x4478a7].
    if (normal_q16.z <= 0) return false;
    const double horizontal = std::hypot(double(normal_q16.x), double(normal_q16.y));
    return double(normal_q16.z) > horizontal * 0.5;
}

// [orig: Entity_OrientToSurfaceNormal @ 0x445fa0]. The normal is rotated into
// the yaw-local frame by the transposed yaw-only Q22 matrix with the witnessed
// +0x200000 rounding [orig: Math_BuildFixedPointRotationMatrixYXZ @ 0x615400
// (yaw, 0, 0); Matrix_Transpose3x3WithNegateCol3 @ 0x6136d0;
// Math_TransformPointFixedPoint22 @ 0x412e90], then each atan2 leg scales by
// g_NegBamPerRadian = -2^31/pi (truncated) and folds -0x40000000 off the caller bias.
// Yaw is written back verbatim; pitch reads atan2(z, y), roll atan2(z, x).
void throwable_stick_pose(const int32_t normal_q16[3], int32_t yaw_bam,
                          int32_t pitch_bias_bam, int32_t roll_bias_bam,
                          int32_t &pitch_bam, int32_t &roll_bam) {
    // [orig: Math_BuildFixedPointRotationMatrixYXZ `fmul dbl_7C3608` @0x61541F]
    const double a = static_cast<double>(yaw_bam) * io::kRadiansPerBamX87;
    const int32_t c = static_cast<int32_t>(std::cos(a) * io::kQ22One);
    const int32_t s = static_cast<int32_t>(std::sin(a) * io::kQ22One);
    const int64_t nx = normal_q16[0], ny = normal_q16[1];
    const int32_t lx = static_cast<int32_t>((ny * s + nx * c + 0x200000) >> 22);
    const int32_t ly = static_cast<int32_t>((ny * c + nx * -s + 0x200000) >> 22);
    const int32_t lz = normal_q16[2];
    // Both legs scale by g_NegBamPerRadian = -kBamPerRad.
    const int32_t roll_leg = static_cast<int32_t>(
            std::atan2(static_cast<double>(lz), static_cast<double>(lx)) * -kBamPerRad);
    const int32_t pitch_leg = static_cast<int32_t>(
            std::atan2(static_cast<double>(lz), static_cast<double>(ly)) * -kBamPerRad);
    roll_bam = static_cast<int32_t>(static_cast<uint32_t>(roll_bias_bam) -
                                    static_cast<uint32_t>(roll_leg) - 0x40000000u);
    pitch_bam = static_cast<int32_t>(static_cast<uint32_t>(pitch_bias_bam) -
                                     static_cast<uint32_t>(pitch_leg) - 0x40000000u);
}

ThrowClass throw_class_from_tag(const char *tag) {
    if (tag == nullptr || tag[0] == '\0') return ThrowClass::kNone;
    // The retail tables match the full 8-byte tag [orig: the class-name scans
    // over @ 0x813000 / @ 0x82abc8]; JO data authors lowercase tags.
    auto eq = [tag](const char *name) {
        size_t i = 0;
        for (; name[i] != '\0'; ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i])));
            if (a != name[i]) return false;
        }
        return tag[i] == '\0';
    };
    if (eq("stng")) return ThrowClass::kStinger;
    if (eq("hlfr")) return ThrowClass::kHellfire;
    if (eq("jvln")) return ThrowClass::kJavelin;
    if (eq("nade")) return ThrowClass::kNade;
    if (eq("schl")) return ThrowClass::kSatchel;
    if (eq("clym")) return ThrowClass::kClaymore;
    if (eq("vmne")) return ThrowClass::kAVMine;
    if (eq("lndm")) return ThrowClass::kLandmine;
    return ThrowClass::kNone;
}

ThrowClass throw_motor_from_move_function(const char *move_function) {
    switch (physics_class_from_move_function(move_function != nullptr ? move_function : "")) {
    case PhysicsClass::Stng: return ThrowClass::kStinger;
    case PhysicsClass::Hlfr: return ThrowClass::kHellfire;
    case PhysicsClass::Jvln: return ThrowClass::kJavelin;
    case PhysicsClass::Nade: return ThrowClass::kNade;
    case PhysicsClass::Schl: return ThrowClass::kSatchel;
    case PhysicsClass::Clym: return ThrowClass::kClaymore;
    default: return ThrowClass::kNone;
    }
}

// [orig: the release leg of the fire binding @ 0x4e07e9 — held < 0x1F ticks ->
// 0xFF; else clamp((held - 31) * (1/93), 0.1, 1.0) * 255 (flt_7CD390 = 1/93,
// flt_7C69F4 = 0.1, flt_7CA29C = 255); a 0 result falls back to 0 = unscaled.]
uint8_t power_throw_charge_from_hold(int32_t held_ticks) {
    if (held_ticks < 31) return 255;
    double v = double(held_ticks - 31) * (1.0 / 93.0);
    if (v > 1.0) v = 1.0;
    if (v < 0.1) v = 0.1;
    const int32_t charge = static_cast<int32_t>(v * 255.0);
    return static_cast<uint8_t>(charge);
}

// One step of the engine rotate-LCG [orig: rol4(s + rol11(s), 4) ^ 1 — the
// independent dword_31BFBB8 shrapnel-fan generator; the inline fan stream
// @ 0x4eb92e — same generator.]
uint16_t ThrowableSim::fan_prng() {
    return io::rotating_prng_next16(fan_prng_state);
}

// ----------------------------------------------------------------------------
// The shared motor pieces.
// ----------------------------------------------------------------------------
namespace {

struct MotorFrame {
    int32_t px, py, pz;    // position Q16
    int32_t vx, vy, vz;    // velocity Q16 / tick
    int32_t prev_z;        // pre-move z (the water-transition edge)
};

int32_t throwable_water_q16(const World &world) {
    // Environment zero means no authored water. Fixed-point cannot represent
    // the host's -1e9 dry sentinel, so INT32_MIN is the lowest equivalent
    // plane for the motor comparisons.
    return world.env.water_z != 0 ? world.env.water_z : INT32_MIN;
}

Entity *entity_for_lifetime(World &world, EntityHandle handle, uint64_t spawn_id) {
    Entity *entity = world.registry.get(handle);
    if (entity == nullptr || spawn_id == 0 || entity->registry_spawn_id != spawn_id)
        return nullptr;
    return entity;
}

int16_t item_angle_degrees_from_bam(int32_t bam) {
    return static_cast<int16_t>(std::lround(double(bam) * kDegreesPerBam));
}

// The motors read and write the round's Position and velocity words in place
// (D-NET-507). [orig: Entity_UpdateGrenadePhysics @0x4443D0 - the entity's
//  +4/+8/+0xC and +0x98/+0x9C/+0xA0]
void load_frame(const LiveRound &r, MotorFrame &f) {
    f.px = r.pos.x.q16;
    f.py = r.pos.y.q16;
    f.pz = r.pos.z.q16;
    f.vx = r.vel.x.q16;
    f.vy = r.vel.y.q16;
    f.vz = r.vel.z.q16;
    f.prev_z = f.pz;
}

void store_frame(LiveRound &r, const MotorFrame &f) {
    r.pos.set_q16({f.px, f.py, f.pz});
    r.vel.set_q16({f.vx, f.vy, f.vz});
}

int32_t horizontal_speed_q16(const MotorFrame &f) {
    const double vx = double(f.vx), vy = double(f.vy);
    double m = std::sqrt(vx * vx + vy * vy);
    if (m > 2147418100.0) m = 2147418100.0; // [orig: the fcom clamp @ 0x444064]
    return static_cast<int32_t>(m);
}

// Charmap surface material under a motor frame (no charmap -> 1 -> dirt, the
// sampler's own default). The bounce presentation plays material + 4 through
// the impact presenter, whose clamp makes a class past the table the obj row.
// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510 (Entity_UpdateGrenadePhysics
// @ 0x4443d0); consumed as material + 4 @ 0x44481f by AmmoDef_ProcessImpactEffect
// @ 0x40a170 @ 0x444824, a tag of 28 or more -> 4 @ 0x40a1bc..0x40a1c1]
int32_t sampled_ground_surface(const World &world, const MotorFrame &f) {
    return terrain::surface_type_at_fixed(world.tables.surface_map, f.px, f.py);
}

int32_t speed3_q16(const MotorFrame &f) {
    const double vx = double(f.vx), vy = double(f.vy), vz = double(f.vz);
    double m = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (m > 2147418100.0) m = 2147418100.0;
    return static_cast<int32_t>(m);
}

void push_motor_effect(World &world, RoundSim &sim, const LiveRound &r, int tag,
                       const Vec3 &at, uint32_t tick,
                       bool present_effect = true, bool present_sound = true) {
    RoundImpact imp;
    imp.position = at;
    imp.direction = Vec3{0.0f, 0.0f, 1.0f};
    imp.ammo_index = r.ammo_index;
    imp.effect_tag = tag;
    imp.present_effect = present_effect;
    imp.present_sound = present_sound;
    imp.tick = tick;
    imp.source_order = sim.next_impact_order++;
    // The motors' impact-effect calls play the row's sound at once, whatever
    // the presentation queue holds [orig: AmmoDef_ProcessImpactEffect @0x40a216
    //  from Entity_UpdateGrenadePhysics / the device thinks].
    play_round_impact_sound(world, imp);
    if (sim.impacts.size() >= RoundSim::kMaxPendingImpacts) return;
    sim.impacts.push_back(imp);
}

// Terrain ground sample (Q16) at a Q16 position.
// [orig: Terrain_SampleHeightBilinear @ 0x6067b0 leaf; the world frame is
// (mission_x, -mission_y) — the collision.cpp convention.]
int32_t ground_q16(const terrain::TerrainHeightField *terrain, int32_t x, int32_t y) {
    if (terrain == nullptr || !terrain->valid()) return INT32_MIN;
    return terrain::height_field_height_bilinear_fixed(*terrain, x, y); // the words (D-NET-507)
}

// The contact presentation's descriptor carries the sound bit while the
// round's contact count byte, compared as a SIGNED byte, is at most 5: a
// count that wraps past 127 sounds again.
// [orig: cmp byte ptr [esi+155h], 5 then setle -- Entity_UpdateGrenadePhysics
//  @0x4447D3 / @0x4447E1, Entity_UpdateClaymorePhysics @0x4478C8 / @0x4478D6,
//  Entity_UpdateSatchelPhysics @0x44893D / @0x44894B]
static bool contact_count_sounds(const LiveRound &r) {
    return static_cast<int8_t>(r.bounce_count) <= 5;
}

// The motors' terrain ground test runs only while bit 0x80 of the round's
// +0x114 word (the ammo's NoCollide, or the Indoors stamp its spawn took from
// the shooter) and its Flags 0x800000 (its BB state) are both clear.
// [orig: Entity_UpdateGrenadePhysics @0x444299 / @0x4442A6;
//  Entity_UpdateSatchelPhysics @0x4485A1 / @0x4485AA;
//  Entity_UpdateClaymorePhysics @0x447553 / @0x44755C]
bool motor_terrain_test_runs(const LiveRound &r, const AmmoTableEntry &ammo) {
    return (ammo.flags & def::DEF_AMMO_FLAG_NOCOLLIDE) == 0 && !r.indoors_word &&
           !r.indoors_flags;
}

// The swept item raycast the motors run — pools 2/1 only (no terrain, no
// persons: a flying grenade passes through people), from the round's saved
// pose to its moved one [orig: the two Projectile_RaycastProximitySlots(2/1,
// ...) calls @ 0x444619..0x444667; the start is savedLivePose @0x44452D].
bool motor_item_sweep(World &world, CollisionWorld *collision, const LiveRound &r,
                      const FixedVec3 &start, const MotorFrame &from_to, int32_t speed,
                      ProjectileHit &out, const int32_t *ray_dir = nullptr,
                      int32_t ray_length = 0) {
    if (collision == nullptr || speed <= 0) return false;
    const CollisionWorld::RayDebugScope ray_scope(
            collision, CollisionWorld::RayDebugCategory::kThrowable);
    ProjectileTrace trace;
    trace.start = start;
    trace.end = FixedVec3{from_to.px, from_to.py, from_to.pz};
    trace.owner = r.owner;
    trace.radius_q16 = 0;
    trace.ammo_flags = 0; // foliage/material gates ride the caller ammo below
    // Pools 2/1 only. Disable every earlier trace domain at the query boundary:
    // a nearer terrain, water, or person hit cannot mask the farther pool-2/1
    // entity that retail's two dedicated walks would find.
    trace.walk_terrain = false;
    trace.walk_water = false;
    trace.walk_persons = false;
    // A visual client's decoded remote throwable sweeps the wire-keyed dyn
    // proxies exactly like the bullet walk (round_sim.cpp): on a retail client
    // this sweep IS the ordinary pool-2/1 walk over its wire-built entities.
    trace.include_wire_proxies =
        r.consequence_mode == RoundConsequenceMode::VisualOnly;
    trace.shooter_wire_handle = r.shooter_handle;
    trace.owner_mount_exclusion = false;
    // Its two walks share one ray record [orig: @0x44461D..0x44467A].
    trace.shared_ray_record = true;
    if (ray_dir != nullptr && ray_length > 0) {
        trace.ray_dir_q16 = FixedVec3{ray_dir[0], ray_dir[1], ray_dir[2]};
        trace.ray_length_q16 = ray_length;
    }
    const ProjectileHit hit = collision->trace_projectile(world, trace);
    if (!hit.hit()) return false;
    if (hit.hit_class != ProjectileHitClass::StaticEntity &&
        hit.hit_class != ProjectileHitClass::DynamicEntity)
        return false; // defensive: the motor query contains only pools 2/1
    out = hit;
    return true;
}

// [orig: Math_FixedPointNormalize3 @0x613280 — magnitude in x87 double,
//  TRUNCATED to an int, then each component `(c << 16) / (int)magnitude` as a
//  signed 64-bit division]. A zero vector would divide by zero in retail; the
//  caller only reaches it with trunc(hspeed) != 0, so the guard is inert.
void fixed_normalize3(int32_t v[3]) {
    const double mag = std::sqrt(static_cast<double>(v[1]) * v[1] +
                                 static_cast<double>(v[2]) * v[2] +
                                 static_cast<double>(v[0]) * v[0]);
    const int32_t imag = static_cast<int32_t>(mag);
    if (imag == 0) return;
    for (int i = 0; i < 3; ++i)
        v[i] = static_cast<int32_t>((static_cast<int64_t>(v[i]) << 16) / imag);
}

// [orig: Physics_ComputeReflectionForce @ 0x4e4310] The normal starts as -v
// normalized in x87 (65536/|v| per component, ftol; zero when |v| == 0)
// @0x4e432d..0x4e43c6 and is REPLACED by the struck face's normal when the hit
// carries bone/face data @0x4e43ea..0x4e449d — our sweep hands that face
// normal in with ProjectileHit::face_normal_found. Then, in 16.16:
//   dot     = (v . n) >> 16 from the ORIGINAL velocity        @0x4e44df
//   hspeed  = sqrt(vx^2 + vy^2) / 65536  (NO vz)              @0x4e44dd..0x4e44f2
//   if trunc(hspeed) != 0: v = normalize3(v)                  @0x4e44fe..0x4e4510
//   v -= (dot * n + 0x8000) >> 16                              @0x4e4530..0x4e4563
//   v += ((0x10000 - |dot|) * n + 0x8000) >> 16                @0x4e458b..0x4e45c6
//   damped = ftol(hspeed * 0.35f * 65536)   (flt_7C6FA8, flt_7C32BC)
//   v = (damped * v + 0x8000) >> 16                            @0x4e45f0..0x4e462f
// The exit speed therefore scales with the HORIZONTAL speed only, and — below
// 1 u/tick, i.e. every thrown grenade — quadratically with it, because v is
// left un-normalized; nothing renormalizes the result. The earlier float port
// rescaled to 0.35 x |v_in| in 3-D, which popped near-vertical drops and
// under-bounced fast horizontal hits.
// The reflection's starting normal, the negated velocity at unit length
// (zero for a still round), which a struck face's normal replaces; it is the
// out-normal too when no face is found [orig: Physics_ComputeReflectionForce
// @0x4E431D..0x4E43D1, fdivr flt_7C32BC @0x4e437d].
void negated_unit_velocity(const MotorFrame &f, int32_t n[3]) {
    n[0] = n[1] = n[2] = 0;
    const double nvx = -static_cast<double>(f.vx);
    const double nvy = -static_cast<double>(f.vy);
    const double nvz = -static_cast<double>(f.vz);
    const double len = std::sqrt(nvx * nvx + nvy * nvy + nvz * nvz);
    if (len > 0.0) {
        const double s = 65536.0 / len;
        n[0] = static_cast<int32_t>(nvx * s);
        n[1] = static_cast<int32_t>(nvy * s);
        n[2] = static_cast<int32_t>(nvz * s);
    }
}

// `face_found`: the reflection's face lookup found the struck face, whose
// normal (a stored zero one included) replaces the negated unit velocity
// [orig: Physics_ComputeReflectionForce @0x4E4416..0x4E449D].
void reflect_velocity(MotorFrame &f, const int32_t normal_q16[3], bool face_found) {
    int32_t v[3] = {f.vx, f.vy, f.vz};
    int32_t n[3] = {normal_q16[0], normal_q16[1], normal_q16[2]};
    if (!face_found) negated_unit_velocity(f, n);
    const int32_t dot = static_cast<int32_t>(
            (static_cast<int64_t>(v[1]) * n[1] + static_cast<int64_t>(v[0]) * n[0] +
             static_cast<int64_t>(v[2]) * n[2]) >> 16);
    const double hspeed = std::sqrt(static_cast<double>(v[0]) * v[0] +
                                    static_cast<double>(v[1]) * v[1]) / 65536.0;
    if (static_cast<int32_t>(hspeed) != 0) fixed_normalize3(v);
    for (int i = 0; i < 3; ++i)
        v[i] -= static_cast<int32_t>((static_cast<int64_t>(dot) * n[i] + 0x8000) >> 16);
    const int32_t absdot = dot < 0 ? -dot : dot;
    for (int i = 0; i < 3; ++i)
        v[i] += static_cast<int32_t>(
                (static_cast<int64_t>(0x10000 - absdot) * n[i] + 0x8000) >> 16);
    const int32_t damped = static_cast<int32_t>(
            hspeed * static_cast<double>(0.35f) * 65536.0); // [orig: @0x4e455d/@0x4e45c0]
    for (int i = 0; i < 3; ++i)
        v[i] = static_cast<int32_t>((static_cast<int64_t>(damped) * v[i] + 0x8000) >> 16);
    f.vx = v[0];
    f.vy = v[1];
    f.vz = v[2];
}

// The flying-phase wrapper: a LiveRound has no registry entity, so the
// function's own savedLivePose backup has nothing to stamp; the placed-device
// leg stamps its registry entity instead. Lifetime/flag detach semantics are
// the reimpl handle-reuse guards.
void follow_parent(World &world, LiveRound &r) {
    Entity *parent = entity_for_lifetime(world, r.parent, r.parent_spawn_id);
    if (parent == nullptr || (parent->engine_flags & 0x02000001u) != 0) {
        r.parent = EntityHandle{};
        r.parent_spawn_id = 0;
        return;
    }
	CarrierMotionPose p;
	p.pos[0] = r.pos.x.q16;
	p.pos[1] = r.pos.y.q16;
    p.pos[2] = r.pos.z.q16;
    p.yaw_bam = r.yaw_bam;
    p.pitch_bam = r.pitch_bam;
    p.roll_bam = r.roll_bam;
	follow_carrier_motion(*parent, p);
	r.pos.set_q16({p.pos[0], p.pos[1], p.pos[2]});
    r.yaw_bam = p.yaw_bam;
    r.pitch_bam = p.pitch_bam;
    r.roll_bam = p.roll_bam;
}

} // namespace

// ----------------------------------------------------------------------------
// The grenade motor [orig: Entity_UpdateGrenadePhysics @ 0x443F50].
// ----------------------------------------------------------------------------
static bool motor_nade(World &world, RoundSim &sim, LiveRound &r,
                       const AmmoTableEntry &ammo, CollisionWorld *collision,
                       const terrain::TerrainHeightField *terrain,
                       bool allow_consequences) {
    MotorFrame f;
    load_frame(r, f);
    // The round's savedLivePose: its last tick's committed position, the
    // sweep's start and the floor query's point [orig: the store
    // @0x444A3B..0x444A4D; read @0x444443..0x444456 and @0x44452D..0x444539].
    FixedVec3 saved{f.px, f.py, f.pz};
    const int32_t water = throwable_water_q16(world);
    bool bounce_eligible = true; // [orig: v91]
    bool ground_hit = false;     // [orig: v86]
    int32_t ground_surface = 0;  // effect-tag surface for the bounce dust

    // Drag + horizontal advance [orig: @ 0x443f95-0x443feb].
    if (f.vx != 0) {
        f.vx = qmul(f.vx, ammo.drag_fp16);
        f.px += f.vx;
    }
    if (f.vy != 0) {
        f.vy = qmul(f.vy, ammo.drag_fp16);
        f.py += f.vy;
    }
    // Split gravity [orig: @ 0x443ffa — above water -167, submerged -55 and no
    // bounce this tick].
    if (f.pz > water) {
        f.vz -= 167;
    } else {
        f.vz -= 55;
        bounce_eligible = false;
    }
    f.pz += f.vz;
    // Spin integration [orig: @ 0x44402e — pitch += spin1, yaw += spin0; the
    // roll spin only decays].
    r.pitch_bam += r.spin_pitch;
    r.yaw_bam += r.spin_yaw;

    int32_t horiz = horizontal_speed_q16(f);
    if (horiz < 2048) horiz = 0; // [orig: @ 0x44407d]

    // Water transitions on the pre/post-move z edge [orig: @ 0x44408b-0x4441ad].
    if (f.prev_z >= water) {
        if (f.pz <= water && f.prev_z > water && horiz != 0) {
            // entering the water: splash + 0.25 damp on the horizontal
            push_motor_effect(world, sim, r, 11, Vec3{static_cast<float>(from_fixed(f.px)),
                                               static_cast<float>(from_fixed(f.py)),
                                               static_cast<float>(from_fixed(water))},
                              world.logic_tick);
            f.vx = qmul(f.vx, 0x4000);
            f.vy = qmul(f.vy, 0x4000);
        }
    } else if (f.pz >= water) {
        // breaching upward: 0.45 damp on all three [orig: 29491/65536]
        f.vx = qmul(f.vx, 29491);
        f.vy = qmul(f.vy, 29491);
        f.vz = qmul(f.vz, 29491);
    }
    // Submerged drag [orig: @ 0x4441b5 — spins 0.9, velocity 0.95].
    if (f.pz <= water) {
        r.spin_yaw = qmul(r.spin_yaw, 58982);
        r.spin_pitch = qmul(r.spin_pitch, 58982);
        r.spin_roll = qmul(r.spin_roll, 58982);
        f.vx = qmul(f.vx, 62259);
        f.vy = qmul(f.vy, 62259);
        f.vz = qmul(f.vz, 62259);
    }
    // Terrain clamp + bounce [orig: @ 0x4442ad-0x444431], skipped on the
    // round's NoCollide or Indoors bits.
    if (motor_terrain_test_runs(r, ammo)) {
        const int32_t ground = ground_q16(terrain, f.px, f.py);
        if (ground != INT32_MIN && f.pz < ground) {
            f.pz = ground;
            r.spin_yaw = qmul(r.spin_yaw, 52428);
            r.spin_pitch = qmul(r.spin_pitch, 52428);
            r.spin_roll = qmul(r.spin_roll, 52428);
            f.vx = qmul(f.vx, 52428);
            f.vy = qmul(f.vy, 52428);
            if (horiz != 0) {
                // moving: bounce [orig: velZ * -13107/65536, rand spin kicks]
                ++r.bounce_count;
                f.vz = qmul(f.vz, -13107);
                ground_hit = true;
                // Charmap surface at the bounce point (no charmap -> 1 -> dirt)
                // [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510 @ 0x4443d0;
                // the bounce presentation applies material + 4 @ 0x44481f]
                ground_surface = sampled_ground_surface(world, f);
                const int32_t kick_yaw =
                        (world.next_prng16() % 10) - 5; // [orig: %10-5]
                const int32_t kick_pitch = world.next_prng16() % 10;
                r.spin_yaw += 11930464 * kick_yaw;
                r.spin_pitch += 11930464 * kick_pitch - 59652320;
            } else {
                // at rest: lie flat [orig: roll = 0x3FFFFFC0, pitch = 0]
                r.spin_yaw = 0;
                r.spin_pitch = 0;
                r.spin_roll = 0;
                r.roll_bam = 0x3FFFFFC0;
                r.pitch_bam = 0;
            }
        }
    }
    // The landed-on entity carries the round: the follow first backs the
    // moved pose up as the saved one, then adds the parent's delta.
    // [orig: the groundEntity test @0x444434, the call @0x44443B;
    //  Entity_InterpolateFromParentDelta @0x4A8D60 - the backup
    //  @0x4A8D7D..0x4A8DB4]
    if (r.parent.valid()) {
        saved = FixedVec3{f.px, f.py, f.pz};
        store_frame(r, f);
        follow_parent(world, r);
        f.px = r.pos.x.q16;
        f.py = r.pos.y.q16;
        f.pz = r.pos.z.q16;
    }
    // The floor under the saved point, queried 0x2000 above it: a pool-2/1
    // face below, else the terrain, which the round's Flags 0x800000 (its BB
    // state, not the +0x114 bit) drops; `landed` takes the entity whose face
    // it was. The landing below reads it. The query's face walk is not yet
    // retail's (D-COL-17).
    // [orig: the query point @0x444443..0x444471, `and ecx, 800000h`
    //  @0x44445C as no_limit, the Entity_ComputeClampedDisplacement call
    //  @0x44447B]
    EntityHandle landed;
    const int32_t floor =
            collision != nullptr
                    ? collision->minefield_ground(
                              world, EntityHandle{},
                              FixedVec3{saved.x, saved.y, io::bam_add(saved.z, 0x2000)},
                              r.indoors_flags, &landed)
                    : INT32_MIN;
    // The swept item raycast [orig: @ 0x444522-0x444729].
    if (horiz != 0) {
        ProjectileHit hit;
        MotorFrame target = f;
        // The ray context: the velocity over the horizontal speed (2^32 /
        // horiz, a Q16 product per axis) and that speed as its length; a hit's
        // distance is its parameter in that length, kept only below it.
        // [orig: the direction @0x444493..0x444519, the length dest[9]
        //  @0x44458B; the face parameter @0x4E51B8; the strict keeps
        //  @0x444632 / @0x444667]
        const int32_t inv = static_cast<int32_t>(0x100000000LL / horiz);
        const int32_t dir[3] = {qmul(inv, f.vx), qmul(inv, f.vy), qmul(inv, f.vz)};
        if (motor_item_sweep(world, collision, r, saved, target, speed3_q16(f), hit, dir,
                             horiz) &&
            hit.distance_q16 < horiz) {
            // The contact backs off 2048 along the ray: the round lands at the
            // saved pose plus the direction times max(t - 2048, 0), the effect
            // and the landing reading it there.
            // [orig: the back-off @0x444694..0x4446FC, stored @0x444746..0x4447AC]
            const int32_t t = hit.distance_q16;
            const int32_t back = t > 2048 ? t - 2048 : 0;
            f.px = saved.x + qmul(dir[0], back);
            f.py = saved.y + qmul(dir[1], back);
            f.pz = saved.z + qmul(dir[2], back);
            // Every sweep return is an item hit; a wire-proxy hit carries no
            // registry identity but still reflects (retail's decoded pool-1
            // entities bounce grenades).
            if (bounce_eligible) {
                const int32_t n[3] = {hit.normal_q16.x, hit.normal_q16.y,
                                      hit.normal_q16.z};
                reflect_velocity(f, n, hit.face_normal_found);
                ++r.bounce_count;
            }
            ground_hit = true;
            ground_surface = hit.surface_type >= 0 ? hit.surface_type : 0;
        }
    }
    // Bounce presentation [orig: @ 0x4447c3 — material + 4, above water only].
    // The descriptor is deliberately sound-only for contacts 1..5
    // (flags 0x80000400), then has neither presentation leg (0x00000400).
    // In particular, retail does NOT submit the dirt row's Effect_FragGrndDirt
    // particle here; that same particle is also authored on the fuse's obj row.
    if (ground_hit && ground_surface != 0 && f.pz >= water &&
        contact_count_sounds(r)) {
        push_motor_effect(world, sim, r, ground_surface + 4,
                          Vec3{static_cast<float>(from_fixed(f.px)),
                               static_cast<float>(from_fixed(f.py)),
                               static_cast<float>(from_fixed(f.pz))},
                          world.logic_tick, false, true);
    }
    // The landing: a falling round below the floor takes its height and its
    // entity as the groundEntity (none on the terrain); on an entity it stops
    // falling, lies flat and loses its yaw and pitch spins and a fifth of
    // its horizontal speed. [orig: @0x444830..0x44483B, the Z store
    // @0x44483D, the groundEntity store @0x444846, the entity leg
    // @0x44484B..0x4448A8]
    if (f.pz < floor && f.vz < 0) {
        f.pz = floor;
        const Entity *ground = world.registry.get(landed);
        r.parent = ground != nullptr ? landed : EntityHandle{};
        r.parent_spawn_id = ground != nullptr ? ground->registry_spawn_id : 0;
        if (ground != nullptr) {
            r.spin_pitch = 0;
            r.spin_yaw = 0;
            r.roll_bam = 0x3FFFFFC0;
            f.vz = 0;
            f.vx = qmul(f.vx, 52428);
            f.vy = qmul(f.vy, 52428);
        }
    }
    store_frame(r, f);

    // RoundSim advances age storage before calling the motor, so the retail
    // pre-decrement elapsed value is one less here.
    const int32_t elapsed = r.age_ticks > 0 ? r.age_ticks - 1 : 0;
    // ARM: the obj effect fires once when elapsed == arm_age (the smoke-pour
    // start) [orig: @ 0x444908 — initial(+676) - remaining(+684) == arm_age].
    if (ammo.arm_age_ticks > 0 && elapsed == ammo.arm_age_ticks)
        push_motor_effect(world, sim, r, 4, r.pos, world.logic_tick);

    // FUSE: two ticks before expiry [orig: @ 0x444976 — above water arms the
    // detonate-at-expiry flag (0x1000); submerged detonates NOW with the
    // depth-keyed underwater tags and zeroes the age]. The flag is consumed by
    // RoundSim's expiry head, the Projectile_UpdatePhysics head that runs
    // this motor, and so shares its kztype gate: a grenade without a
    // kill-zone class (JO's smoke) releases there silently. The submerged
    // push tests no class. [orig: the flag @0x444A29; the push @0x444A0F]
    const int32_t remaining = r.max_age_ticks - elapsed;
    if (remaining == 2) {
        // The Z word against the water word, and the depth a word difference
        // past 0x30000 (D-NET-507) [orig: `cmp ecx, eax; jge` @0x444984..0x444986,
        //  `sub edx, ecx` / `cmp edx, 30000h` @0x4449A2..0x4449A4, `jg` @0x4449C2].
        if (r.pos.z.q16 >= water) {
            r.det_at_expiry = true;
        } else {
            const int32_t depth = io::bam_sub(water, r.pos.z.q16);
            // Tags 27 and 26 sit at the water surface, tag 25 at the round
            // [orig: the descriptor z = g_EnvWaterHeightFixed @0x4449BE for
            //  the calls @0x4449CC (26) / @0x4449DE (27); the round z
            //  restored @0x4449EE for @0x4449F2 (25)].
            const Vec3 surface{r.pos.x, r.pos.y, static_cast<float>(from_fixed(water))};
            if (depth > 0x30000) {
                push_motor_effect(world, sim, r, 27, surface, world.logic_tick);
                push_motor_effect(world, sim, r, 25, r.pos, world.logic_tick);
            } else {
                push_motor_effect(world, sim, r, 26, surface, world.logic_tick);
            }
            if (allow_consequences) detonate_round(world, r, r.pos, ammo);
            r.det_at_expiry = false;
            return false; // slot releases
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// The satchel/claymore motor [orig: Entity_UpdateSatchelPhysics @ 0x4482A0 /
// Entity_UpdateClaymorePhysics @ 0x4472F0 — one skeleton, claymore stays
// upright and never terrain-bounces its spin].
// ----------------------------------------------------------------------------

// A pools-2/1 walk under a caller's ray context, for the charge motors' own
// queries: the domains off, the decoded proxies on a visual client as the item
// sweep's. `owner_slot`: the ray's ray[17] is the round's owner (the slide and
// the sweep), whose decoded wire identity stands in for it on a visual client;
// false for the object floor, whose ray[17] is the round itself. ray[18..20]
// stay zero (the motors' memsets), so no mount is excluded.
// `shared_ray_record`: the item sweep's two walks share one ray record
// (ProjectileTrace::shared_ray_record); the slide walks one entity and the
// object floor copies its kept record whole [orig: Physics_RaycastProximityEntities
// @0x538350, its qmemcpy keeps].
ProjectileHit charge_ray(World &world, CollisionWorld *collision, const LiveRound &r,
                         const FixedVec3 &start, const FixedVec3 &end, bool owner_slot,
                         const int32_t dir[3], int32_t length, EntityHandle only_entity,
                         bool shared_ray_record = false) {
    ProjectileHit none;
    if (collision == nullptr || length <= 0) return none;
    const CollisionWorld::RayDebugScope ray_scope(
            collision, CollisionWorld::RayDebugCategory::kThrowable);
    ProjectileTrace trace;
    trace.start = start;
    trace.end = end;
    trace.owner = owner_slot ? r.owner : EntityHandle{};
    trace.radius_q16 = 0;
    trace.ammo_flags = 0;
    trace.walk_terrain = false;
    trace.walk_water = false;
    trace.walk_persons = false;
    trace.include_wire_proxies =
        r.consequence_mode == RoundConsequenceMode::VisualOnly;
    if (owner_slot) trace.shooter_wire_handle = r.shooter_handle;
    trace.owner_mount_exclusion = false;
    trace.ray_dir_q16 = FixedVec3{dir[0], dir[1], dir[2]};
    trace.ray_length_q16 = length;
    trace.only_entity = only_entity;
    trace.shared_ray_record = shared_ray_record;
    return collision->trace_projectile(world, trace);
}

// Entity_ProcessTerrainBounce(round, 0), a misnomer: a parented round's test
// of its parent alone, from its saved pose 4096 lower to its moved one along
// (0, 0, -1) over a length of 1024. A hit below that length reflects the
// velocity off the struck face and counts a contact; then a face whose
// (0, 0, -1) dot lies in (-0.5, 0), a steep one, pushes the horizontal
// velocity along its normal by trunc((dot + 1) * 512).
// [orig: Entity_ProcessTerrainBounce @0x4459F0 -- the ray @0x445A19..0x445AC0,
//  Weapon_ValidateHitTarget and the < 1024 keep @0x445AC7..0x445AEA, the
//  reflection @0x445AFF, the count @0x445B04, the dot @0x445B67..0x445B97,
//  the 2^-16 scale and the two compares @0x445B9B..0x445BBD (flt_7C3310,
//  flt_7C59B0 = -0.5), the push @0x445BBF..0x445C08 (flt_7C3280 = 1.0,
//  flt_7C59AC = 512.0)]
void charge_parent_slide(World &world, CollisionWorld *collision, LiveRound &r,
                         const FixedVec3 &saved, MotorFrame &f) {
    if (!r.parent.valid()) return;
    const int32_t down[3] = {0, 0, -0x10000};
    const ProjectileHit hit = charge_ray(
            world, collision, r, FixedVec3{saved.x, saved.y, saved.z - 4096},
            FixedVec3{f.px, f.py, f.pz}, true, down, 1024, r.parent);
    if (!hit.hit() || hit.distance_q16 >= 1024) return;
    // The dot and the push read the reflection's out-normal: the face's own
    // (ProjectileHit::local_normal_q16), or with no face found the negated
    // unit velocity it starts from.
    int32_t out_n[3] = {hit.local_normal_q16.x, hit.local_normal_q16.y,
                        hit.local_normal_q16.z};
    if (!hit.face_normal_found) negated_unit_velocity(f, out_n);
    const FixedVec3 out{out_n[0], out_n[1], out_n[2]};
    const int32_t n[3] = {hit.normal_q16.x, hit.normal_q16.y, hit.normal_q16.z};
    reflect_velocity(f, n, hit.face_normal_found);
    ++r.bounce_count;
    const int32_t dot = static_cast<int32_t>((static_cast<int64_t>(down[2]) * out.z) >> 16);
    const double dot_units = static_cast<double>(dot) * (1.0 / 65536.0);
    if (dot_units < 0.0 && dot_units > -0.5) {
        const int32_t scale = static_cast<int32_t>((dot_units + 1.0) * 512.0);
        f.vx += qmul(scale, out.x);
        f.vy += qmul(scale, out.y);
    }
}

// Physics_RaycastProximityEntities(round, saved, saved - 5 u): a falling
// round's object floor. Pools 2 then 1 along the unit segment, each hit kept
// only below the length so far, the end clipping to the nearest; the round is
// the walk's excluded slot, so no row of it is excluded. No hit leaves the
// end, 5 u under the saved pose.
// [orig: Physics_RaycastProximityEntities @0x538350 -- the unit direction
//  @0x538374..0x5383DA, the exclusion ray_state[17] @0x538453, the two keeps
//  @0x5384EE / @0x538528, the clipped end @0x538545..0x5385AA; called
//  @0x4483EA (satchel, the end `sub 50000h` @0x4483CA) / @0x447434 (claymore)]
int32_t charge_object_floor(World &world, CollisionWorld *collision, const LiveRound &r,
                            const FixedVec3 &saved) {
    const int32_t end_z = saved.z - 0x50000;
    const int32_t down[3] = {0, 0, -0x10000};
    const ProjectileHit hit = charge_ray(world, collision, r, saved,
                                         FixedVec3{saved.x, saved.y, end_z},
                                         false, down, 0x50000, EntityHandle{});
    if (!hit.hit() || hit.distance_q16 >= 0x50000) return end_z;
    return saved.z + static_cast<int32_t>(
            (static_cast<int64_t>(down[2]) * hit.distance_q16 + 0x8000) >> 16);
}

static bool motor_charge(World &world, RoundSim &sim, LiveRound &r,
                         const AmmoTableEntry &ammo, CollisionWorld *collision,
                         const terrain::TerrainHeightField *terrain,
                         bool claymore, bool allow_consequences) {
    MotorFrame f;
    load_frame(r, f);
    // savedLivePose: the tick-start pose, the previous tail's copy
    // [orig: the tail @0x448C88..0x448CB8 / @0x447C0B..0x447C39].
    const FixedVec3 saved{f.px, f.py, f.pz};
    const int32_t water = throwable_water_q16(world);

    // Drag + epsilon stop [orig: |v'| < 256 -> 0].
    if (f.vx != 0) {
        f.vx = qmul(f.vx, ammo.drag_fp16);
        f.px += f.vx;
        if (std::abs(f.vx) < 256) f.vx = 0;
    }
    if (f.vy != 0) {
        f.vy = qmul(f.vy, ammo.drag_fp16);
        f.py += f.vy;
        if (std::abs(f.vy) < 256) f.vy = 0;
    }
    // A parented round tests its parent's face first [orig: the groundEntity
    // test and the call @0x448361..0x448369 (satchel) / @0x4473B1..0x4473B9
    // (claymore)].
    charge_parent_slide(world, collision, r, saved, f);
    // Inverted gravity split [orig: satchel @ 0x4482f8 — above water -55
    // (light toss), submerged -167 (sinks fast)].
    if (f.pz > water) f.vz -= 55;
    else f.vz -= 167;
    f.pz += f.vz;
    if (claymore) {
        // [orig: clym @ 0x447377 — yaw -= spin, pitch pinned 0]
        r.yaw_bam -= r.spin_yaw;
        r.pitch_bam = 0;
    } else {
        // [orig: schl @ 0x44830e — angles -= spins]
        r.yaw_bam -= r.spin_yaw;
        r.pitch_bam -= r.spin_pitch;
    }
    // A falling round's object floor; a rising one's is 0
    // [orig: the floor's 0 seed @0x4482D5 / @0x447325; the vz <= 0 test
    // @0x4483AC / @0x4473F6, the floor's store @0x4483F6 / @0x447440].
    int32_t floor = 0;
    if (f.vz <= 0) floor = charge_object_floor(world, collision, r, saved);
    // Water entry splash + submerged damp [orig: entry tag 11; schl 0.98 spin
    // 0.94 vel, clym velXY * 0.5].
    if (f.pz <= water) {
        if (f.prev_z > water)
            push_motor_effect(world, sim, r, 11, Vec3{static_cast<float>(from_fixed(f.px)),
                                               static_cast<float>(from_fixed(f.py)),
                                               static_cast<float>(from_fixed(water))},
                              world.logic_tick);
        if (claymore) {
            f.vx = qmul(f.vx, 0x8000);
            f.vy = qmul(f.vy, 0x8000);
        } else {
            r.spin_yaw = qmul(r.spin_yaw, 64225);
            r.spin_pitch = qmul(r.spin_pitch, 64225);
            r.spin_roll = qmul(r.spin_roll, 64225);
            f.vx = qmul(f.vx, 61603);
            f.vy = qmul(f.vy, 61603);
            f.vz = qmul(f.vz, 61603);
        }
    }
    int32_t speed = speed3_q16(f);
    if (speed < 256) speed = 0; // [orig: @0x448599 / @0x44754B]

    // Terrain: the ground raises the floor, and a round below it stops there
    // with no bounce and no parent; skipped on the round's NoCollide or
    // Indoors bits. ground_height keeps the sampled height for the rest gate
    // (0 when the test did not run).
    // [orig: satchel @0x4485B1..0x448615 (the floor raise @0x4485CD..0x4485CF,
    //  Terrain_GetSurfaceTypeAtPosition @0x4485E0); claymore @0x447563..0x4475C7
    //  (Terrain_GetSurfaceTypeAtPosition @0x447592)]
    bool ground_hit = false;
    int32_t ground_surface = 0;
    int32_t ground_height = 0;
    if (motor_terrain_test_runs(r, ammo)) {
        const int32_t ground = ground_q16(terrain, f.px, f.py);
        if (ground != INT32_MIN) {
            ground_height = ground;
            if (floor < ground) floor = ground;
            if (f.pz < ground) {
                ++r.bounce_count;
                f.pz = ground;
                f.vx = 0;
                f.vy = 0;
                f.vz = 0;
                speed = 0;
                ground_hit = true;
                ground_surface = sampled_ground_surface(world, f);
                r.parent = EntityHandle{};
                r.parent_spawn_id = 0;
            }
        }
    }
    // The parent follow, after the terrain test: its backup makes the moved
    // pose the saved one the sweep starts from and an item contact restores
    // [orig: @0x448619..0x44861F / @0x4475CB..0x4475D1 ->
    // Entity_InterpolateFromParentDelta @0x4A8D60, the backup
    // @0x4A8D7D..0x4A8DB4].
    FixedVec3 start = saved;
    if (r.parent.valid()) {
        start = FixedVec3{f.px, f.py, f.pz};
        store_frame(r, f);
        follow_parent(world, r);
        f.px = r.pos.x.q16;
        f.py = r.pos.y.q16;
        f.pz = r.pos.z.q16;
    }
    // The swept item raycast under the motor's ray context (the velocity over
    // the 3-D speed, that speed as the length; each pool's hit kept only below
    // the length so far). A contact reflects the velocity off the struck
    // face; a face whose normal points up past half its horizontal part
    // orients the round to it and stops its spins, the satchel (pitch bias
    // 0xC0000040) also taking the entity as its groundEntity; the round goes
    // back to its saved pose and counts the contact; the claymore takes the
    // entity as its groundEntity on every contact. Neither stops the round:
    // the rest gate below decides. Every pools-2/1 hit carries an entity, so
    // the entity-less arm (the roll's clear) never runs.
    // [orig: satchel -- the direction @0x448629..0x448704, the length
    //  @0x448724, the keeps @0x4487CD / @0x4487FF, the reflection @0x448853,
    //  the stick @0x448882..0x4488C4, the store @0x448905..0x44891E; claymore
    //  -- @0x4475DB..0x4476D6, @0x44777F / @0x4477B1, @0x4477FD,
    //  @0x44782C..0x44785E, the store @0x447884..0x4478A4, the groundEntity
    //  @0x4478A7]
    if (speed != 0) {
        const int32_t inv = static_cast<int32_t>(0x100000000LL / speed);
        const int32_t dir[3] = {qmul(inv, f.vx), qmul(inv, f.vy), qmul(inv, f.vz)};
        const ProjectileHit hit = charge_ray(world, collision, r, start,
                                             FixedVec3{f.px, f.py, f.pz}, true, dir,
                                             speed, EntityHandle{},
                                             /*shared_ray_record=*/true);
        if (hit.hit() && hit.distance_q16 < speed) {
            // The struck entity: a registry row, or a decoded mover's verified
            // twin; a wire proxy without one reflects but cannot parent.
            const EntityHandle struck = hit.geometry_entity.valid() ? hit.geometry_entity
                                                                    : hit.wire_registry_twin;
            // The stick test and the pose read the reflection's out-normal: the
            // face's own (ProjectileHit::local_normal_q16), or, with no face
            // found, the negated unit velocity the reflection starts from
            // [orig: Physics_ComputeReflectionForce @0x4E43CA..0x4E43D1 /
            //  @0x4E448A..0x4E4491; satchel @0x448858..0x4488AA; claymore
            //  @0x447802..0x447850].
            int32_t out[3] = {hit.local_normal_q16.x, hit.local_normal_q16.y,
                              hit.local_normal_q16.z};
            if (!hit.face_normal_found) negated_unit_velocity(f, out);
            const int32_t n[3] = {hit.normal_q16.x, hit.normal_q16.y, hit.normal_q16.z};
            reflect_velocity(f, n, hit.face_normal_found);
            if (throwable_surface_accepts_stick(FixedVec3{out[0], out[1], out[2]})) {
                throwable_stick_pose(out, r.yaw_bam,
                                     claymore ? 0 : static_cast<int32_t>(0xC0000040), 0,
                                     r.pitch_bam, r.roll_bam);
                r.spin_yaw = 0;
                r.spin_pitch = 0;
                if (!claymore) {
                    r.spin_roll = 0;
                    r.parent = struck;
                    const Entity *parent = world.registry.get(struck);
                    r.parent_spawn_id = parent != nullptr ? parent->registry_spawn_id : 0;
                }
            }
            f.px = start.x;
            f.py = start.y;
            f.pz = start.z;
            ++r.bounce_count;
            if (claymore) {
                r.parent = struck;
                const Entity *parent = world.registry.get(struck);
                r.parent_spawn_id = parent != nullptr ? parent->registry_spawn_id : 0;
            }
            ground_hit = true;
            ground_surface = hit.surface_type >= 0 ? hit.surface_type : 0;
        }
    }
    // The contact presents through the impact presenter with the grenade's
    // descriptor, sound only for the first five contacts (0x80000400), then
    // neither leg (0x400), and no water-height test of its own (the grenade's
    // alone has one, @0x44480F..0x444818): the presenter's ClipWaterFx gate
    // drops a round whose Z and +0x88 copy both sit at or under the plane, the
    // copy each motor tick's tail refreshes, so the Z this tick began at. The
    // item sweep walks pools 2/1 alone, so the switch's person and water arms
    // (3, 4) never arise and the face byte is the surface.
    // [orig: Entity_UpdateClaymorePhysics @0x4478C8..0x44790E, the tail copy
    //  @0x447C23; Entity_UpdateSatchelPhysics @0x44893D..0x448983, the tail copy
    //  @0x448CA2; AmmoDef_ProcessImpactEffect @0x40A18D..0x40A1B1]
    // The gate's +0x88 copy is the saved pose's Z, the follow's backup for a
    // parented round [orig: Entity_InterpolateFromParentDelta @0x4A8D84..0x4A8D96].
    const bool clipped_by_water = (ammo.flags & kAmmoFlagClipWaterFx) != 0 &&
                                  f.pz <= water && start.z <= water;
    if (ground_hit && ground_surface != 0 && !clipped_by_water && contact_count_sounds(r))
        push_motor_effect(world, sim, r, ground_surface + 4,
                          Vec3{static_cast<float>(from_fixed(f.px)),
                               static_cast<float>(from_fixed(f.py)),
                               static_cast<float>(from_fixed(f.pz))},
                          world.logic_tick, false, true);

    // The landing and the five-contact stop: a falling round below the floor
    // lands on it with no groundEntity; one past five contacts (a signed byte)
    // that did not land loses its velocity and takes its own Z as the floor and
    // the ground height. The rest gate then passes a round within 0xFF over
    // the floor (unsigned) that sits on the ground height, has no speed, or is
    // not rising.
    // [orig: satchel @0x44898B..0x4489EE (the landing @0x44899E..0x4489A1, the
    //  stop @0x4489A6..0x4489C3, the gate @0x4489C7..0x4489EE); claymore
    //  @0x447916..0x447979]
    int32_t rest_floor = floor;
    if (f.pz >= floor || f.vz >= 0) {
        if (static_cast<int8_t>(r.bounce_count) > 5) {
            rest_floor = f.pz;
            f.vz = 0;
            f.vx = 0;
            f.vy = 0;
            ground_height = f.pz;
        }
    } else {
        f.pz = floor;
        r.parent = EntityHandle{};
        r.parent_spawn_id = 0;
    }
    const uint32_t over = static_cast<uint32_t>(f.pz) - static_cast<uint32_t>(rest_floor);
    const bool rest = over <= 0xFF &&
                      (ground_height == rest_floor || speed == 0 || f.vz <= 0);
    store_frame(r, f);
    if (!rest) return true;
    // The rest leg: an unparented round lifts 4096 and loses its update
    // callback, a parented one keeps following its parent; the spins and the
    // vertical speed stop. The authority converts the round into its placed
    // device (the round slot releases); any other peer keeps the round 248
    // ticks more, its aging back on.
    // [orig: satchel @0x4489F6..0x448A22, the conversion @0x448A2E..0x448C12,
    //  the client leg @0x448C1A..0x448C24; claymore @0x447981..0x4479AD,
    //  @0x4479B9..0x447B93, @0x447B9B..0x447BA5]
    // The Z word lifts by 0x1000, a word add (D-NET-507) [orig: `add edx, 1000h`
    //  / `mov [esi+0Ch], edx` @0x448A07..0x448A0D; claymore @0x447992..0x447998].
    if (!r.parent.valid()) r.pos.z.q16 = io::bam_add(r.pos.z.q16, 0x1000);
    r.spin_yaw = 0;
    r.spin_pitch = 0;
    r.vel.z.q16 = 0;
    if (allow_consequences) {
        r.vel.set_q16({0, 0, 0});
        world.throwables.place_from_round(world, r, ammo);
        return false; // the round slot releases [orig: source round expires]
    }
    r.motor = ThrowClass::kNone;
    r.rest_follows_parent = r.parent.valid();
    // The age word becomes 248 and the post-motor decrement takes one.
    r.max_age_ticks = r.age_ticks + 248 - 1;
    return true;
}

// The round's own BB refresh after its motor: its Flags 0x800000 clears and
// is set again from the blink walk its def's type picks: none for a building
// (type 5); for a vehicle or a person (1 / 3) its own candidate slice, which
// a round never holds (the slot allocator zeroes all 0x30C bytes); the static
// building prefix for any other type (every shipped throwable's def is
// `type object`) or no def. The +0x114 bit stays as the spawn stamped it.
// [orig: Projectile_UpdatePhysics - the motor call @0x4E9F1E, the
//  Entity_BuildProximityList call @0x4E9F21; Entity_BuildProximityList
//  @0x4B3DC0 - the clear @0x4B3E0C, the def+0x5C read @0x4B3E47, the type
//  tests @0x4B3E4A..0x4B3E5F, the static walk @0x4B3E65..0x4B3F39, the set
//  @0x4B40C5; CEntityManager_AllocateSlot @0x4EAAD0, the memset @0x4EABC8]
static void refresh_round_indoors(World &world, LiveRound &round,
                                  CollisionWorld *collision) {
    round.indoors_flags = false;
    if (collision == nullptr) return;
    const ThrowableClassRow *row = world.throwables.classes.get(round.item_type_id);
    const int32_t type = row != nullptr ? row->def_type : 0;
    if (type == def::DEF_ITEM_TYPE_BUILDING || type == def::DEF_ITEM_TYPE_VEHICLE ||
            type == def::DEF_ITEM_TYPE_PERSON)
        return;
    const int32_t at[3] = {round.pos.x.q16, round.pos.y.q16, round.pos.z.q16};
    BlinkAccum accum;
    collision->query_blink_boxes_at_point(world, at, accum);
    round.indoors_flags = (accum.flags & kBlinkIndoorsBit) != 0;
}

bool throwable_motor_tick(World &world, RoundSim &sim, LiveRound &round,
                          const AmmoTableEntry &ammo, CollisionWorld *collision,
                          const terrain::TerrainHeightField *terrain,
                          bool allow_consequences) {
    bool alive = true;
    switch (round.motor) {
    case ThrowClass::kNade:
        alive = motor_nade(world, sim, round, ammo, collision, terrain,
                           allow_consequences);
        break;
    case ThrowClass::kSatchel:
    case ThrowClass::kAVMine: // AT mine authors move_function schl
        alive = motor_charge(world, sim, round, ammo, collision, terrain, false,
                             allow_consequences);
        break;
    case ThrowClass::kClaymore:
        alive = motor_charge(world, sim, round, ammo, collision, terrain, true,
                             allow_consequences);
        break;
    default:
        // A charge round at rest on a non-authority peer follows its parent
        // (its +452 is the parent follow) and takes the BB refresh after it
        // [orig: the rest leg's +452 store @0x4489FB / @0x447986;
        //  Projectile_UpdatePhysics @0x4E9F1E..0x4E9F21].
        if (round.rest_follows_parent) {
            if (round.parent.valid()) follow_parent(world, round);
            refresh_round_indoors(world, round, collision);
            return true;
        }
        // no motor bound: the round drifts unmoved and takes no BB refresh,
        // both sitting behind the +452 test (retail leaves +452 null)
        // [orig: @0x4E9F0C..0x4E9F1B]
        return true;
    }
    refresh_round_indoors(world, round, collision);
    return alive;
}

// ----------------------------------------------------------------------------
// Placed devices.
// ----------------------------------------------------------------------------
bool ThrowableSim::place_from_round(World &world, const LiveRound &round,
                                    const AmmoTableEntry &ammo) {
    if (devices.size() >= static_cast<size_t>(kCapacity)) return false;

    EntityHandle parent_handle;
    Entity *parent_entity = nullptr;
    if (round.parent.valid()) {
        Entity *candidate = world.registry.get(round.parent);
        if (candidate != nullptr &&
            (round.parent_spawn_id == 0 ||
             candidate->registry_spawn_id == round.parent_spawn_id)) {
            parent_handle = round.parent;
            parent_entity = candidate;
        }
    }

    // The registry entity the device lives as — pool 1 [orig:
    // Entity_CloneFromTemplateByType @ 0x4398a0 routes items.def type
    // vehicle/object into pool 1; the sweeps @ 0x546e00/0x546ed0/0x547160 all
    // walk pool 1].
    Entity seed;
    const int32_t item_id =
            round.item_type_id != 0 ? round.item_type_id
                                    : (ammo.tracer_item_friendly != 0
                                               ? ammo.tracer_item_friendly
                                               : ammo.tracer_item_enemy);
    seed.kind = EntityKind::Item;
    seed.has_item_def = true;
    seed.position = round.pos;
    seed.heading = round.yaw_bam;
    seed.pitch = item_angle_degrees_from_bam(round.pitch_bam);
    seed.roll = item_angle_degrees_from_bam(round.roll_bam);
    seed.ground_target = parent_handle;
    seed.team = round.team;
    seed.item_id = item_id;
    // The copy carries the round's Flags 0x800000, its BB state as the last
    // tick's refresh left it (this tick's runs after the motor), which the
    // device keeps until its first think visit rebuilds it; the +0x114 bit
    // rides along too, read by no device leg.
    // [orig: Entity_ConvertRoundToPlacedEntity @0x5455B0 - the memcpy
    //  @0x5455D6; Entity_UpdatePool1Slot @0x4B8E1B..0x4B8E25]
    if (round.indoors_flags) seed.flags |= kEntityFlagIndoors;
    if (const ThrowableClassRow *row = classes.get(item_id)) {
        seed.item_type_index = row->item_type_index; // the template's +0x1C
        // [orig: Entity_InitFromItemDef @ 0x49e550 — Health/Armor from the def]
        if (row->health_max != 0) {
            seed.health = row->health_max;
            seed.health_max = row->health_max;
        }
        seed.armor_impact = row->armor_impact;
        seed.armor_kz = row->armor_kz;
        // The copy carries the round's def pointer and graphic model (+0x20,
        // +0x30), so the device reads its def's type and attrib words as a
        // resolved entity does (the S2C 0x18 answer to a C2S 0x0F query takes
        // the type from the def), and keeps the model only when the round
        // kept it: a non-tracer shot's spawn cleared it (a later collision
        // resolve restamps it here, D-ITEMDEF-30).
        // [orig: Entity_ConvertRoundToPlacedEntity @0x5455B0 - the def test
        //  @0x5455B5, the memcpy @0x5455D6; RoundData_SpawnRound @0x4EC900;
        //  NetPacket_SerializeObjectToBuffer @0x504DB9]
        seed.item_type = static_cast<uint8_t>(row->def_type);
        stamp_item_attrib(seed, row->attrib, row->attrib2);
        seed.has_graphic_model = row->has_graphic_model && round.tracer;
        // The conversion copies the round and runs the model init on the
        // placed entity, whose first leg ORs Flags 0x8000000 for a NoTarget
        // def (items.def 101891 "Satchel Charge"): the AI target walks and
        // the LOS walk then skip the device, and the load stream carries it.
        // [orig: Entity_UpdateSatchelPhysics @0x448A3E /
        //  Entity_UpdateClaymorePhysics @0x4479C9 ->
        //  Entity_ConvertRoundToPlacedEntity @0x5455B0, the memcpy @0x5455D6,
        //  the Entity_InitFromModel call @0x5455D8 -> @0x40DC4F]
        if (row->no_target) seed.engine_flags |= kEntityFlagNoTarget;
        // The init's hp-0 leg: an indestructible def's entity takes Flags
        // 0x4000000, Health 1 and subType 0xFF, and the def's two armor
        // words turn 0xFFFF, as resolve_item_traits's leg does.
        // [orig: Entity_InitFromModel @0x40DC85 - Flags @0x40DC8E, def+0x190 /
        //  def+0x192 @0x40DC95 / @0x40DC9F, Health @0x40DCA6, subType @0x40DCAF]
        if (row->health_max == 0) {
            seed.engine_flags |= kEntityFlagIndestructible;
            seed.health = 1;
            seed.sub_type = 0xFF;
            seed.armor_impact = -1;
            seed.armor_kz = -1;
        }
        // Its matrix-built leg reads the copied round's update callback
        // (+0x1C4), which the rest leg nulled for an unparented round and set
        // to the parent follow for a parented one, so an unparented device
        // takes the bit whatever its def type.
        // [orig: Entity_InitFromModel @0x40E0D4..0x40E105 - the +0x1C4 test
        //  @0x40E0DC, the OR @0x40E105; the rest legs' +0x1C4 stores: satchel
        //  @0x4489FB / @0x448A10, claymore @0x447986 / @0x44799B]
        if (!round.parent.valid() || row->def_type == def::DEF_ITEM_TYPE_DECORATION ||
                row->def_type == def::DEF_ITEM_TYPE_BUILDING ||
                row->def_type == def::DEF_ITEM_TYPE_OBJECT)
            seed.engine_flags |= kEntityFlagMatrixBuilt;
        // ...and the signatures [orig: @0x40E136..0x40E156].
        seed.radar_sig = row->radar_sig;
        seed.heat_sig = row->heat_sig;
    }
    // Health/armor come from the item def via the host traits sweep
    // [orig: Entity_InitFromItemDef @ 0x49e550 Health = def healthMax]; the
    // sweep lifts health when traits resolve. Until then the device is
    // shootable at the default.
    // The bound the proximity table carries; the face walk hits the device only
    // once its collision instance resolves (a model-less entity is a miss).
    seed.bound_radius = 0.5f;
    Entity *owner_ent = world.registry.get(round.owner);
    const EntityHandle handle = world.registry.spawn(1, seed);
    if (!handle.valid()) return false;
    Entity *placed_entity = world.registry.get(handle);
    if (placed_entity == nullptr) return false;

    PlacedDevice d;
    d.active = true;
    d.entity = handle;
    d.entity_spawn_id = placed_entity->registry_spawn_id;
    d.owner = round.owner;
    d.owner_spawn_id = owner_ent != nullptr ? owner_ent->registry_spawn_id : 0;
    d.owner_handle = round.shooter_handle;
    d.ammo_index = round.ammo_index;
    d.item_id = item_id;
    d.item_friendly = ammo.tracer_item_friendly;
    d.item_enemy = ammo.tracer_item_enemy;
    d.team = owner_ent != nullptr ? static_cast<uint8_t>(owner_ent->team) : round.team;
    d.think = round.think;
    d.pos = round.pos;
    d.yaw_bam = round.yaw_bam;
    d.pitch_bam = round.pitch_bam;
    d.roll_bam = round.roll_bam;
    d.parent = parent_handle;
    d.parent_spawn_id = parent_entity != nullptr ? parent_entity->registry_spawn_id : 0;
    // ARM DELAY: the un-aged remaining life [orig: noage skipped round aging,
    // so the clone's +684 still holds ~max_age; Entity_UpdatePool1Slot then
    // counts it down 1/tick and thinks at <= 0].
    d.think_delay_ticks = ammo.max_age_ticks;
    devices.push_back(d);

    ThrowableEvents::DeviceSpawn ev;
    ev.entity = handle.packed;
    ev.item_id = item_id;
    ev.owner_handle = d.owner_handle != EntityHandle::kInvalid
                              ? d.owner_handle
                              : d.owner.packed;
    ev.item_friendly = d.item_friendly;
    ev.item_enemy = d.item_enemy;
    ev.parent_handle = d.parent.valid()
                               ? d.parent.packed
                               : EntityHandle::kInvalid;
    ev.team = d.team;
    ev.pos = d.pos;
    ev.yaw_bam = d.yaw_bam;
    ev.pitch_bam = d.pitch_bam;
    ev.roll_bam = d.roll_bam;
    events.spawns.push_back(ev);
    // Both authority conversion motors enforce the cap AFTER their 0x59
    // send; the MOTOR keys it (AT mines author move_function schl and ride
    // the satchel motor's max-3 call with their own def id). A device still
    // arming (age >= 0) never qualifies as the eviction candidate, so an
    // all-arming surplus persists -- the retail null no-op.
    // [orig: Server_EnforcePlacedDeviceCapByOwner @0x5119e0 -- owner +368 +
    //  itemDef+80 match, min over +684 seeded 0 with strict < @0x511a42-44,
    //  count > max -> Server_RemoveEntityAndNotify @0x50a270 (null-safe);
    //  satchel motor caller @0x448be6 max 3; claymore @0x447b67 max 4]
    if (round.motor == ThrowClass::kSatchel ||
        round.motor == ThrowClass::kAVMine)
        enforce_device_cap(world, devices.back(), 3);
    else if (round.motor == ThrowClass::kClaymore)
        enforce_device_cap(world, devices.back(), 4);
    return true;
}

void ThrowableSim::enforce_device_cap(World &world,
                                      const PlacedDevice &placed,
                                      int32_t max_devices) {
    int32_t same_type_count = 0;
    PlacedDevice *oldest_armed = nullptr;
    int32_t oldest_age = 0;
    for (PlacedDevice &candidate : devices) {
        if (!candidate.active ||
            candidate.owner.packed != placed.owner.packed ||
            candidate.owner_spawn_id != placed.owner_spawn_id ||
            candidate.item_id != placed.item_id ||
            entity_for_lifetime(world, candidate.entity,
                                candidate.entity_spawn_id) == nullptr)
            continue;
        ++same_type_count;
        if (candidate.think_delay_ticks < oldest_age) {
            oldest_age = candidate.think_delay_ticks;
            oldest_armed = &candidate;
        }
    }
    if (same_type_count > max_devices && oldest_armed != nullptr)
        remove_device(world, *oldest_armed);
}

void ThrowableSim::detonate_satchels_by_owner(World &world, EntityHandle owner) {
    // [orig: Entity_DetonateSatchelsByOwner @ 0x546ed0 — Health(+286) = -1 on
    // every pool-1 satchel-ammo record owned by the firer; only SATCHEL ammo.]
    const Entity *live_owner = world.registry.get(owner);
    if (live_owner == nullptr) return;
    for (PlacedDevice &d : devices) {
        if (!d.active || d.owner.packed != owner.packed ||
            d.owner_spawn_id != live_owner->registry_spawn_id)
            continue;
        const AmmoTableEntry *ammo = world.tables.ammo.by_index(d.ammo_index);
        if (ammo == nullptr) continue;
        if (!strutil::iequals(ammo->name, "satchel")) continue;
        Entity *e = entity_for_lifetime(world, d.entity, d.entity_spawn_id);
        if (e != nullptr) e->health = -1;
    }
}

void ThrowableSim::remove_device(World &world, PlacedDevice &device) {
    if (!device.active) return;
    if (entity_for_lifetime(world, device.entity, device.entity_spawn_id) == nullptr) {
        device.active = false;
        return;
    }
    device.active = false;
    ThrowableEvents::DeviceRemove ev;
    ev.entity = device.entity.packed;
    events.removes.push_back(ev);
    world.registry.despawn(device.entity);
}

void ThrowableSim::detonate_device(World &world, PlacedDevice &device,
                                   const char *boom_ammo_name) {
    // [orig: the think handlers spawn a fire descriptor at the device position
    // with the boom ammo and the OWNER as source -> RoundData_SpawnRound's
    // instantkillzone leg queues the explosion; obj effect tag 4 at the pos.]
    const int boom_index = world.tables.ammo.index_of(boom_ammo_name);
    if (boom_index >= 0) {
        RoundSpawnParams p;
        p.owner = device.owner;
        p.shooter_handle = device.owner_handle;
        p.origin = device.pos;
        p.dir_yaw_bam = device.yaw_bam;
        p.dir_pitch_bam = device.pitch_bam;
        p.ammo_index = boom_index;
        // The think's descriptor reaches the pool with the word as last written,
        // which the think never writes; the port hands the fresh word (D-NET-776)
        // [orig: Entity_ClaymoreThink @0x4438C0 / Entity_SatchelThink @0x443670 /
        //  Entity_AVMineThink @0x443BB0 -> RoundData_SpawnRound;
        //  CEntityManager_AllocateSlot @0x4EAAE6].
        p.shot_seq = RoundSim::kFreshRoundWord;
        world.round_sim.spawn(world, p);
    }
    remove_device(world, device);
}

bool ThrowableSim::enemy_in_cone(World &world, CollisionWorld *collision,
                                 const terrain::TerrainHeightField *terrain,
                                 const PlacedDevice &device, float max_range_units,
                                 int32_t cone_half_bam, bool vehicles) {
    // [orig: Entity_FindEnemyInCone @ 0x43cba0 (pool 0 persons) /
    // Entity_FindEnemyVehicleInCone @ 0x43c9f0 (pool 1, def kind 1, moving).]
    if (max_range_units <= 0.0f) return false;
    CollisionWorld *queries = collision != nullptr ? collision : world.collision;
    if (queries != nullptr) queries->terrain = terrain;
    const int pool = vehicles ? 1 : 0;
    const size_t cap = world.registry.pool_capacity(pool);
    // Both variants trace from 0.3 u above the device [orig: +0x4CCC @0x43CA29
    // / @0x43CBD9].
    const int32_t dev[3] = {device.pos.x.q16, device.pos.y.q16, device.pos.z.q16};
    const int32_t eye[3] = {dev[0], dev[1], dev[2] + 0x4CCC};
    const int32_t max_range = to_fixed(max_range_units);
    for (size_t s = 0; s < cap; ++s) {
        const EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
        const Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        // A slot with no items.def row, no def or no model is no candidate
        // [orig: `cmp [esi+1Ch], 0` (ItemTypeIndex) @0x43CA5B / @0x43CC07, the
        // def [esi+20h] @0x43CA67 / @0x43CC13, the graphic model [esi+30h]
        // @0x43CA6E / @0x43CC19].
        if (e->item_type_index == 0 || !e->has_item_def || !e->has_graphic_model) continue;
        // The vehicle sweep skips Flags bits 0 and 1 (inactive, dead) [orig:
        // `test byte [esi+24h], 3` @0x43CA61]; the person sweep bit 0 alone
        // [orig: `test byte [esi+24h], 1` @0x43CC0D].
        if ((e->engine_flags & (vehicles ? 0x3u : 0x1u)) != 0) continue;
        // [orig: same team skipped unless the TeamTriggerClaymore rule 0x8000]
        if (e->team == device.team && !team_trigger_claymore) continue;
        if (vehicles) {
            if (e->item_type != 1) continue; // [orig: def kind +92 == 1 @0x43CA8D]
            // [orig: the device's own parent (+0x28) @0x43CA92]
            if (h.packed == device.parent.packed) continue;
        }
        // The distance is the fixed-point deltas' length, capped at
        // 2147418112.0 and truncated, against the fixed range [orig:
        // @0x43CA97..0x43CAE9 / @0x43CC38..0x43CC8B, _ftol2_sse].
        const int32_t pos[3] = {e->position.x.q16, e->position.y.q16,
                                e->position.z.q16};
        const double dx = double(int32_t(uint32_t(pos[0]) - uint32_t(dev[0])));
        const double dy = double(int32_t(uint32_t(pos[1]) - uint32_t(dev[1])));
        const double dz = double(int32_t(uint32_t(pos[2]) - uint32_t(dev[2])));
        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > 2147418112.0) dist = 2147418112.0; // [orig: flt_7C19E0]
        if (io::retail_ftol_sse2(dist) > max_range) continue;
        // atan2 scales by g_NegBamPerRadian = -kBamPerRad under the round-toward-zero
        // control word into a qword whose low dword is kept, and the angle is
        // -Yaw minus that: bearing minus Yaw. The abs is cdq/xor/sub and the
        // compare unsigned (`ja`), so 0x7FFFFFFF passes every bearing but the
        // exact antipode [orig: @0x43CAEF..0x43CB31 / @0x43CC91..0x43CCD3, the
        // person sweep's fpatan @0x43CC9D].
        const int32_t converted = io::retail_fistp_truncate_low_dword(
                std::atan2(dy, dx) * -kBamPerRad);
        const int32_t signed_diff = static_cast<int32_t>(
                (0u - static_cast<uint32_t>(device.yaw_bam)) -
                static_cast<uint32_t>(converted));
        const uint32_t diff = signed_diff < 0
                                      ? uint32_t(0) - static_cast<uint32_t>(signed_diff)
                                      : static_cast<uint32_t>(signed_diff);
        if (diff > static_cast<uint32_t>(cone_half_bam))
            continue;
        // The retail query tests terrain plus sector solids, excluding the
        // device and candidate themselves. CollisionWorld::raycast_clear is
        // the shared full-query structural port; collision-less harnesses keep
        // the terrain leaf as their host fallback. The vehicle sweep aims 1.0 u
        // above the candidate's origin [orig: `add eax, 10000h` @0x43CB52 into
        // the end point pushed @0x43CB44], the person sweep at the origin itself
        // [orig: `lea ebp, [esi+4]` @0x43CC4A, pushed @0x43CCD9].
        // [orig: Physics_RaycastSegment @ 0x415550, a result of 1 or 2 clear;
        // the person sweep's hit return @0x43CCFD]
        const int32_t b[3] = {pos[0], pos[1], vehicles ? pos[2] + 0x10000 : pos[2]};
        if (queries != nullptr) {
            const CollisionWorld::RayDebugScope ray_scope(
                    queries, CollisionWorld::RayDebugCategory::kThrowable);
            if (!queries->raycast_clear(world, eye, b, device.entity, h)) continue;
        } else if (terrain != nullptr && los_terrain_blocked(*terrain, eye, b)) {
            continue;
        }
        // Only then the moving gate [orig: |entity+0x29C| >= 0xCCC (0.05 u/t)
        // @0x43CB70..0x43CB80].
        if (vehicles && std::abs(e->veh.speed) < 0xCCC) continue;
        return true;
    }
    return false;
}

PlacedDevice *ThrowableSim::device_for(const Entity &entity) {
    for (PlacedDevice &d : devices)
        if (d.active && d.entity == entity.handle &&
                d.entity_spawn_id == entity.registry_spawn_id)
            return &d;
    return nullptr;
}

void ThrowableSim::think_device(World &world, PlacedDevice &d, Entity *e,
                                CollisionWorld *collision,
                                const terrain::TerrainHeightField *terrain) {
    const AmmoTableEntry *ammo = world.tables.ammo.by_index(d.ammo_index);
    if (ammo == nullptr) return;
    const bool dead = e->health <= 0;
    switch (d.think) {
    case ThrowClass::kSatchel: {
        // [orig: Entity_SatchelThink @ 0x443670 — Health <= 0 only.]
        if (dead) detonate_device(world, d, "satchelboom");
        break;
    }
    case ThrowClass::kClaymore: {
        // [orig: Entity_ClaymoreThink @ 0x4438C0 — Health <= 0 or the
        // person cone; fires the shrapnel fan + the kill zone.]
        if (dead || enemy_in_cone(world, collision, terrain, d,
                                  ammo->kz_minradius,
                                  ammo->kz_pieslice_bam, false)) {
            const int shrap = world.tables.ammo.index_of("claymoreshrapnel");
            if (shrap >= 0) {
                RoundSpawnParams p;
                p.owner = d.owner;
                p.shooter_handle = d.owner_handle;
                p.origin = d.pos;
                p.dir_yaw_bam = d.yaw_bam;
                p.dir_pitch_bam = d.pitch_bam;
                p.ammo_index = shrap;
                p.shot_seq = RoundSim::kFreshRoundWord; // each pellet mints its id
                world.round_sim.spawn(world, p);
            }
            detonate_device(world, d, "claymorekillzone");
        }
        break;
    }
    case ThrowClass::kAVMine: {
        // [orig: Entity_AVMineThink @ 0x443BB0 — Health <= 0 or the moving
        // vehicle cone. AV_Mine authors no kz_pieslice, so its half-angle is
        // the allocator's 0x7FFFFFFF: every bearing within kz_minradius
        // (AmmoDef_AllocateSlot @0x409A93).]
        if (dead || enemy_in_cone(world, collision, terrain, d,
                                  ammo->kz_minradius,
                                  ammo->kz_pieslice_bam, true))
            detonate_device(world, d, "AV_Minekillzone");
        break;
    }
    default:
        // kNade never places; kLandmine (mission minefield items) thinks in
        // world/minefield.cpp (MinefieldSystem), not here.
        if (dead) remove_device(world, d);
        break;
    }
}

void ThrowableSim::follow_parent(World &world, PlacedDevice &d, Entity *e) {
    // Ride the stuck-to parent [orig: the placed motor +452 =
    // Entity_InterpolateFromParentDelta @ 0x4a8d60, installed by
    // Entity_ConvertRoundToPlacedEntity @ 0x5455fd].
    if (d.parent.valid()) {
        Entity *parent = entity_for_lifetime(world, d.parent, d.parent_spawn_id);
        if (parent != nullptr) {
            // The child's own pre-move savedLivePose backup
            // [orig: @ 0x4a8d7d..0x4a8db4].
            stamp_saved_live_pose(*e);
            CarrierMotionPose p;
            p.pos[0] = e->position.x.q16;
            p.pos[1] = e->position.y.q16;
            p.pos[2] = e->position.z.q16;
            p.yaw_bam = d.yaw_bam;
            p.pitch_bam = d.pitch_bam;
            p.roll_bam = d.roll_bam;
            follow_carrier_motion(*parent, p);
            d.pos.set_q16({p.pos[0], p.pos[1], p.pos[2]});
            d.yaw_bam = p.yaw_bam;
            d.pitch_bam = p.pitch_bam;
            d.roll_bam = p.roll_bam;
            // The followed pose's words, stored raw (D-NET-507).
            e->position.set_q16({p.pos[0], p.pos[1], p.pos[2]});
            e->heading = d.yaw_bam;
            e->pitch = item_angle_degrees_from_bam(d.pitch_bam);
            e->roll = item_angle_degrees_from_bam(d.roll_bam);
            e->ground_target = d.parent;
        } else {
            d.parent = EntityHandle{};
            d.parent_spawn_id = 0;
            e->ground_target = EntityHandle{};
        }
    }
}

// One placed device's pool-1 visit, in Entity_UpdatePool1Slot order: the
// think while the PRE-decrement age is <= 0 (after the row's own blink
// refresh), then the +0x1C4 motor (the stuck-to parent follow), then the
// unconditional decrement, wrapping like x86 -- the ever-falling negative
// value is the oldest-armed ordering key the device cap reads.
// [orig: Entity_UpdatePool1Slot @0x4B8DD0 -- the gate @0x4B8E1B (cmp/jg
//  before the decrement), the Entity_BuildProximityList call @0x4B8E25, the +0x1C8
//  think @0x4B8E3C, the +0x1C4 motor @0x4B8E53, `add [esi+2ACh],-1`
//  @0x4B8EA0]
void ThrowableSim::update_device(World &world, PlacedDevice &d,
                                 CollisionWorld *collision,
                                 const terrain::TerrainHeightField *terrain) {
    if (!d.active) return;
    // A device row the owner sweep already removed (the deploy leg, a Player
    // row's notifying removal, the leaver's teardown) just releases its record:
    // the visit itself never reads the owner, so a dead owner's devices stay
    // armed until that owner deploys again. [orig: Entity_UpdatePool1Slot
    //  @0x4B8DD0 reads no owner; the sweeps are Entity_RemovePlacedDevicesByOwner
    //  @0x546E00, called from Server_DeployPlayer @0x5178D8 and
    //  Server_RemoveEntityAndNotify @0x50A2BB]
    Entity *e = entity_for_lifetime(world, d.entity, d.entity_spawn_id);
    if (e == nullptr) {
        d.active = false;
        return;
    }
    if (d.think_delay_ticks <= 0) {
        if (collision != nullptr) collision->refresh_blink(world, *e);
        think_device(world, d, e, collision, terrain);
        if (!d.active) return;
        e = entity_for_lifetime(world, d.entity, d.entity_spawn_id);
        if (e == nullptr) {
            d.active = false;
            return;
        }
    }
    follow_parent(world, d, e);
    d.think_delay_ticks = static_cast<int32_t>(
            static_cast<uint32_t>(d.think_delay_ticks) - 1u);
}

void ThrowableSim::compact() {
    size_t w = 0;
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i].active) devices[w++] = devices[i];
    devices.resize(w);
}

} // namespace opennova::world
