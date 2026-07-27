// Throwables: the thrown-round class motors, placed-device conversion, and the
// device think/detonate chain. Witness record: docs/world/world-wac-ai-re.md §27
// (engine-research 2026-07-20, retail Jointops.exe kong IDB).
#include "world/throwables.h"

#include <cmath>
#include <cstring>

#include "io/strutil.h"
#include "terrain/height_field.h"
#include "world/angle.h"
#include "world/ammo_table.h"
#include "world/collision.h"
#include "world/round_sim.h"
#include "world/world.h"

namespace opennova::world {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBamPerRad = 4294967296.0 / (2.0 * kPi);

// Q16 multiply with the witnessed +0x8000 rounding [orig: the pervasive
// (a * b + 0x8000) >> 16 idiom in every motor].
inline int32_t qmul(int64_t a, int64_t b) {
    return static_cast<int32_t>((a * b + 0x8000) >> 16);
}

inline int32_t q16(double v) { return to_fixed(v); }

// Mission-frame direction of a BAM yaw/pitch pair (the §5.16 bearing frame the
// round spawner uses).
inline void bam_dir(int32_t yaw_bam, int32_t pitch_bam, double out[3]) {
    const double yaw = double(yaw_bam) / kBamPerRad;
    const double pitch = double(pitch_bam) / kBamPerRad;
    out[0] = std::cos(yaw) * std::cos(pitch);
    out[1] = std::sin(yaw) * std::cos(pitch);
    out[2] = std::sin(pitch);
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
    if (eq("nade")) return ThrowClass::kNade;
    if (eq("schl")) return ThrowClass::kSatchel;
    if (eq("clym")) return ThrowClass::kClaymore;
    if (eq("vmne")) return ThrowClass::kAVMine;
    if (eq("lndm")) return ThrowClass::kLandmine;
    return ThrowClass::kNone;
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

namespace {
// One step of the engine rotate-LCG [orig: rol4(s + rol11(s), 4) ^ 1 — the
// PRNG_Next16 @ 0x6130a0 generator; world-local streams, the ai.cpp/destruction
// precedent (stream identity with retail is not reproducible)].
inline uint32_t throwable_prng_step(uint32_t &s) {
    const uint32_t rol11 = (s << 11) | (s >> 21);
    uint32_t r = s + rol11;
    r = ((r << 4) | (r >> 28)) ^ 1u;
    s = r;
    return r;
}
} // namespace

// [orig: PRNG_Next16 @ 0x6130a0 on dword_31BFBB0 — the bounce spin kicks.]
int32_t ThrowableSim::prng16() {
    return static_cast<int32_t>(throwable_prng_step(prng16_state) & 0xFFFFu);
}

// [orig: the inline fan stream on dword_31BFBB8 @ 0x4eb92e — same generator.]
uint16_t ThrowableSim::fan_prng() {
    return static_cast<uint16_t>(throwable_prng_step(fan_prng_state));
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

void load_frame(const LiveRound &r, MotorFrame &f) {
    f.px = to_fixed(r.pos.x);
    f.py = to_fixed(r.pos.y);
    f.pz = to_fixed(r.pos.z);
    f.vx = to_fixed(r.vel.x);
    f.vy = to_fixed(r.vel.y);
    f.vz = to_fixed(r.vel.z);
    f.prev_z = f.pz;
}

void store_frame(LiveRound &r, const MotorFrame &f) {
    r.pos.x = static_cast<float>(from_fixed(f.px));
    r.pos.y = static_cast<float>(from_fixed(f.py));
    r.pos.z = static_cast<float>(from_fixed(f.pz));
    r.vel.x = static_cast<float>(from_fixed(f.vx));
    r.vel.y = static_cast<float>(from_fixed(f.vy));
    r.vel.z = static_cast<float>(from_fixed(f.vz));
}

int32_t horizontal_speed_q16(const MotorFrame &f) {
    const double vx = double(f.vx), vy = double(f.vy);
    double m = std::sqrt(vx * vx + vy * vy);
    if (m > 2147418100.0) m = 2147418100.0; // [orig: the fcom clamp @ 0x444064]
    return static_cast<int32_t>(m);
}

int32_t speed3_q16(const MotorFrame &f) {
    const double vx = double(f.vx), vy = double(f.vy), vz = double(f.vz);
    double m = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (m > 2147418100.0) m = 2147418100.0;
    return static_cast<int32_t>(m);
}

void push_motor_effect(RoundSim &sim, const LiveRound &r, int tag,
                       const Vec3 &at, uint32_t tick,
                       bool present_effect = true, bool present_sound = true) {
    if (sim.impacts.size() >= RoundSim::kMaxPendingImpacts) return;
    RoundImpact imp;
    imp.position = at;
    imp.direction = Vec3{0.0f, 0.0f, 1.0f};
    imp.ammo_index = r.ammo_index;
    imp.effect_tag = tag;
    imp.present_effect = present_effect;
    imp.present_sound = present_sound;
    imp.tick = tick;
    imp.source_order = sim.next_impact_order++;
    sim.impacts.push_back(imp);
}

// Terrain ground sample (Q16) at a Q16 position.
// [orig: Terrain_SampleHeightBilinear @ 0x6067b0 leaf; the world frame is
// (mission_x, -mission_y) — the collision.cpp convention.]
int32_t ground_q16(const terrain::TerrainHeightField *terrain, int32_t x, int32_t y) {
    if (terrain == nullptr || !terrain->valid()) return INT32_MIN;
    const float h = terrain::height_field_height_world_bilinear(
            *terrain, static_cast<float>(from_fixed(x)),
            static_cast<float>(-from_fixed(y)));
    return to_fixed(h);
}

// The swept item raycast the motors run — pools 2/1 only (no terrain, no
// persons: a flying grenade passes through people) [orig: the two
// Projectile_RaycastProximitySlots(2/1, ...) calls @ 0x444619..0x444667].
bool motor_item_sweep(World &world, CollisionWorld *collision, const LiveRound &r,
                      const MotorFrame &from_to, int32_t speed,
                      ProjectileHit &out) {
    if (collision == nullptr || speed <= 0) return false;
    ProjectileTrace trace;
    trace.start = FixedVec3{to_fixed(r.pos.x), to_fixed(r.pos.y), to_fixed(r.pos.z)};
    trace.end = FixedVec3{from_to.px, from_to.py, from_to.pz};
    trace.owner = r.owner;
    trace.radius_q16 = 0;
    trace.ammo_flags = 0; // foliage/material gates ride the caller ammo below
    // Pools 2/1 only: no person walk, so a bystander cannot mask the vehicle
    // behind them (the post-trace class filter below cannot recover a farther
    // hit the nearest-person result already consumed).
    trace.walk_persons = false;
    // A visual client's decoded remote throwable sweeps the wire-keyed dyn
    // proxies exactly like the bullet walk (round_sim.cpp): on a retail client
    // this sweep IS the ordinary pool-2/1 walk over its wire-built entities.
    trace.include_wire_proxies =
        r.consequence_mode == RoundConsequenceMode::VisualOnly;
    trace.shooter_wire_handle = r.shooter_handle;
    trace.shooter_carrier_wire_handle = r.shooter_carrier_handle;
    const ProjectileHit hit = collision->trace_projectile(world, trace);
    if (!hit.hit()) return false;
    if (hit.hit_class != ProjectileHitClass::StaticEntity &&
        hit.hit_class != ProjectileHitClass::DynamicEntity)
        return false; // terrain/water legs are not part of the motor sweep
    out = hit;
    return true;
}

// [orig: Physics_ComputeReflectionForce @ 0x4e4310] — normal = the hit face
// normal (fallback: -v normalized); v' = (v - (v.n)n) + n * (1 - |v.n|), then
// |v'| rescaled to 0.35 * |v_in| (flt_7C6FA8).
void reflect_velocity(MotorFrame &f, const int32_t normal_q16[3]) {
    const double vx = from_fixed(f.vx), vy = from_fixed(f.vy), vz = from_fixed(f.vz);
    const double vin = std::sqrt(vx * vx + vy * vy + vz * vz);
    double nx = from_fixed(normal_q16[0]);
    double ny = from_fixed(normal_q16[1]);
    double nz = from_fixed(normal_q16[2]);
    const double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nlen > 1e-6) {
        nx /= nlen;
        ny /= nlen;
        nz /= nlen;
    } else if (vin > 1e-6) {
        nx = -vx / vin;
        ny = -vy / vin;
        nz = -vz / vin;
    } else {
        return;
    }
    const double dot = vx * nx + vy * ny + vz * nz;
    double rx = vx - dot * nx;
    double ry = vy - dot * ny;
    double rz = vz - dot * nz;
    const double pop = 1.0 - std::fabs(dot); // [orig: (0x10000 - |dot|) leg]
    rx += pop * nx;
    ry += pop * ny;
    rz += pop * nz;
    const double rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    const double target = vin * 0.35; // [orig: flt_7C6FA8 damping]
    if (rlen > 1e-6 && target > 0.0) {
        rx *= target / rlen;
        ry *= target / rlen;
        rz *= target / rlen;
    }
    f.vx = to_fixed(rx);
    f.vy = to_fixed(ry);
    f.vz = to_fixed(rz);
}

// Stick pose: align the device against the hit face [orig:
// Entity_OrientToSurfaceNormal @ 0x445fa0 — pitch/roll derived from the
// surface normal (the exact euler decomposition is decompiler-mangled;
// this port derives the same aligned pose geometrically, D-THROW-4)].
void stick_pose_from_normal(const int32_t normal_q16[3], bool keep_yaw,
                            int32_t &yaw_bam, int32_t &pitch_bam, int32_t &roll_bam) {
    const double nx = from_fixed(normal_q16[0]);
    const double ny = from_fixed(normal_q16[1]);
    const double nz = from_fixed(normal_q16[2]);
    if (!keep_yaw) {
        // face the normal: yaw along the outward normal
        if (std::fabs(nx) > 1e-6 || std::fabs(ny) > 1e-6)
            yaw_bam = static_cast<int32_t>(
                    static_cast<int64_t>(std::llround(std::atan2(ny, nx) * kBamPerRad)));
    }
    const double horiz = std::sqrt(nx * nx + ny * ny);
    pitch_bam = static_cast<int32_t>(
            static_cast<int64_t>(std::llround(std::atan2(horiz, nz) * kBamPerRad)));
    roll_bam = 0;
}

// The parent-follow leg: translate with the stuck-to entity, orbiting its yaw
// [orig: Entity_InterpolateFromParentDelta @ 0x4a8d60 applies the parent's
// position delta + rotation delta through the 22-bit trig; the pitch/roll legs
// stay open (D-THROW-4)].
void follow_parent(World &world, LiveRound &r) {
    Entity *parent = entity_for_lifetime(world, r.parent, r.parent_spawn_id);
    if (parent == nullptr || (parent->engine_flags & 0x02000001u) != 0) {
        r.parent = EntityHandle{};
        r.parent_spawn_id = 0;
        r.parent_tracking = false;
        return;
    }
    if (!r.parent_tracking) {
        r.parent_prev_pos = parent->position;
        r.parent_prev_yaw_bam = bam_heading_from_mission_yaw_deg(parent->yaw);
        r.parent_tracking = true;
        return;
    }
    const Vec3 dp{parent->position.x - r.parent_prev_pos.x,
                  parent->position.y - r.parent_prev_pos.y,
                  parent->position.z - r.parent_prev_pos.z};
    const int32_t yaw_now = bam_heading_from_mission_yaw_deg(parent->yaw);
    const int32_t dyaw = static_cast<int32_t>(
            static_cast<uint32_t>(yaw_now) - static_cast<uint32_t>(r.parent_prev_yaw_bam));
    // rotate the offset from the parent by the yaw delta
    const double lx = double(r.pos.x - r.parent_prev_pos.x);
    const double ly = double(r.pos.y - r.parent_prev_pos.y);
    const double a = double(dyaw) / kBamPerRad;
    const double ca = std::cos(a), sa = std::sin(a);
    r.pos.x = r.parent_prev_pos.x + static_cast<float>(lx * ca - ly * sa) +
              static_cast<float>(dp.x);
    r.pos.y = r.parent_prev_pos.y + static_cast<float>(lx * sa + ly * ca) +
              static_cast<float>(dp.y);
    r.pos.z += static_cast<float>(dp.z);
    r.yaw_bam = static_cast<int32_t>(static_cast<uint32_t>(r.yaw_bam) +
                                     static_cast<uint32_t>(dyaw));
    r.parent_prev_pos = parent->position;
    r.parent_prev_yaw_bam = yaw_now;
}

} // namespace

// ----------------------------------------------------------------------------
// The grenade motor [orig: Entity_UpdateGrenadePhysics @ 0x443F50].
// ----------------------------------------------------------------------------
static bool motor_nade(World &world, RoundSim &sim, LiveRound &r,
                       const AmmoTableEntry &ammo, CollisionWorld *collision,
                       const terrain::TerrainHeightField *terrain,
                       bool allow_consequences) {
    if (r.parent.valid()) follow_parent(world, r);
    MotorFrame f;
    load_frame(r, f);
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
            push_motor_effect(sim, r, 11, Vec3{static_cast<float>(from_fixed(f.px)),
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
    // Terrain clamp + bounce [orig: @ 0x4442ad-0x444431; nocollide 0x80 skips].
    if ((ammo.flags & 0x80u) == 0) {
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
                ground_surface = 1; // no-charmap default material: dirt (tag 5,
                                    // the ballistic terrain default); the
                                    // surface-map override remains D-WPN-15
                const int32_t kick_yaw =
                        (world.throwables.prng16() % 10) - 5; // [orig: %10-5]
                const int32_t kick_pitch = world.throwables.prng16() % 10;
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
    // The swept item raycast [orig: @ 0x444522-0x444729].
    if (horiz != 0) {
        ProjectileHit hit;
        MotorFrame target = f;
        if (motor_item_sweep(world, collision, r, target, speed3_q16(f), hit)) {
            // back the contact off 2048 Q16 [orig: v44 - 2048 @ 0x444694]
            f.px = hit.position_q16.x;
            f.py = hit.position_q16.y;
            f.pz = hit.position_q16.z;
            // Every sweep return is an item hit; a wire-proxy hit carries no
            // registry identity but still reflects (retail's decoded pool-1
            // entities bounce grenades). reflect_velocity's -v fallback covers
            // the normal-less sphere stand-ins.
            if (bounce_eligible) {
                const int32_t n[3] = {hit.normal_q16.x, hit.normal_q16.y,
                                      hit.normal_q16.z};
                reflect_velocity(f, n);
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
        r.bounce_count <= 5) {
        push_motor_effect(sim, r, ground_surface + 4,
                          Vec3{static_cast<float>(from_fixed(f.px)),
                               static_cast<float>(from_fixed(f.py)),
                               static_cast<float>(from_fixed(f.pz))},
                          world.logic_tick, false, true);
    }
    store_frame(r, f);

    // RoundSim advances age storage before calling the motor, so the retail
    // pre-decrement elapsed value is one less here.
    const int32_t elapsed = r.age_ticks > 0 ? r.age_ticks - 1 : 0;
    // ARM: the obj effect fires once when elapsed == arm_age (the smoke-pour
    // start) [orig: @ 0x444908 — initial(+676) - remaining(+684) == arm_age].
    if (ammo.arm_age_ticks > 0 && elapsed == ammo.arm_age_ticks)
        push_motor_effect(sim, r, 4, r.pos, world.logic_tick);

    // FUSE: two ticks before expiry [orig: @ 0x444976 — above water arms the
    // detonate-at-expiry flag (0x1000); submerged detonates NOW with the
    // depth-keyed underwater tags and zeroes the age].
    const int32_t remaining = r.max_age_ticks - elapsed;
    if (remaining == 2) {
        if (r.pos.z >= from_fixed(water)) {
            r.det_at_expiry = true;
        } else {
            const double depth = from_fixed(water) - r.pos.z;
            if (depth > 3.0) {
                push_motor_effect(sim, r, 27,
                                  Vec3{r.pos.x, r.pos.y,
                                       static_cast<float>(from_fixed(water))},
                                  world.logic_tick);
                push_motor_effect(sim, r, 25, r.pos, world.logic_tick);
            } else {
                push_motor_effect(sim, r, 26, r.pos, world.logic_tick);
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
static bool motor_charge(World &world, RoundSim &sim, LiveRound &r,
                         const AmmoTableEntry &ammo, CollisionWorld *collision,
                         const terrain::TerrainHeightField *terrain,
                         bool claymore, bool allow_consequences) {
    if (r.parent.valid()) follow_parent(world, r);
    MotorFrame f;
    load_frame(r, f);
    const int32_t water = throwable_water_q16(world);
    bool ground_hit = false;
    bool rest = false;
    int32_t ground_surface = 0;

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
    // Water entry splash + submerged damp [orig: entry tag 11; schl 0.98 spin
    // 0.94 vel, clym velXY * 0.5].
    if (f.pz <= water) {
        if (f.prev_z > water)
            push_motor_effect(sim, r, 11, Vec3{static_cast<float>(from_fixed(f.px)),
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
    if (speed < 256) speed = 0; // [orig: @ 0x4484xx epsilon]

    // Terrain: full stop, no bounce [orig: velocity zeroed, parent cleared].
    int32_t ground = INT32_MIN;
    if ((ammo.flags & 0x80u) == 0) {
        ground = ground_q16(terrain, f.px, f.py);
        if (ground != INT32_MIN && f.pz < ground) {
            ++r.bounce_count;
            f.pz = ground;
            f.vx = 0;
            f.vy = 0;
            f.vz = 0;
            speed = 0;
            ground_hit = true;
            ground_surface = 1; // no-charmap default material: dirt (D-WPN-15)
            r.parent = EntityHandle{};
            r.parent_spawn_id = 0;
            r.parent_tracking = false;
        }
    }
    // The swept item raycast + stick [orig: reflect; reflected z-speed above
    // half the impact speed -> orient to the face + spins zeroed + parent].
    if (speed != 0) {
        ProjectileHit hit;
        if (motor_item_sweep(world, collision, r, f, speed, hit)) {
            f.px = hit.position_q16.x;
            f.py = hit.position_q16.y;
            f.pz = hit.position_q16.z;
            ++r.bounce_count;
            {
                const int32_t n[3] = {hit.normal_q16.x, hit.normal_q16.y,
                                      hit.normal_q16.z};
                reflect_velocity(f, n);
                // Stick/parenting needs a registry entity: a wire-proxy hit
                // reflects but cannot parent — device-on-decoded-vehicle
                // tracking is the D-THROW-7/D-WPN-8 residual.
                if (hit.geometry_entity.valid() &&
                    throwable_surface_accepts_stick(hit.normal_q16)) {
                    // stick to the face
                    stick_pose_from_normal(n, claymore, r.yaw_bam, r.pitch_bam,
                                           r.roll_bam);
                    if (!claymore) r.roll_bam = -0x3FFFFFC0; // [orig: schl -90 deg]
                    r.spin_yaw = 0;
                    r.spin_pitch = 0;
                    r.spin_roll = 0;
                    f.vx = 0;
                    f.vy = 0;
                    f.vz = 0;
                    r.parent = hit.geometry_entity;
                    if (const Entity *parent = world.registry.get(r.parent))
                        r.parent_spawn_id = parent->registry_spawn_id;
                    r.parent_tracking = false;
                    rest = true;
                }
            }
            ground_hit = true;
            ground_surface = hit.surface_type >= 0 ? hit.surface_type : 0;
        }
    }
    if (ground_hit && ground_surface != 0 && f.pz >= water)
        push_motor_effect(sim, r, ground_surface + 4,
                          Vec3{static_cast<float>(from_fixed(f.px)),
                               static_cast<float>(from_fixed(f.py)),
                               static_cast<float>(from_fixed(f.pz))},
                          world.logic_tick);

    // Rest gate [orig: height-over-ground <= 0xFF and not rising].
    if (!rest && ground != INT32_MIN) {
        const int32_t over = f.pz - ground;
        if (over >= 0 && over <= 0xFF && (speed == 0 || f.vz <= 0)) rest = true;
    }
    store_frame(r, f);
    if (rest) {
        // lift 4096 Q16 off the ground when unparented [orig: @ 0x4c... the
        // rest leg], then convert.
        if (!r.parent.valid()) r.pos.z += static_cast<float>(from_fixed(4096));
        r.vel = Vec3{0.0f, 0.0f, 0.0f};
        if (allow_consequences)
            world.throwables.place_from_round(world, r, ammo);
        return false; // the round slot releases [orig: source round expires]
    }
    return true;
}

bool throwable_motor_tick(World &world, RoundSim &sim, LiveRound &round,
                          const AmmoTableEntry &ammo, CollisionWorld *collision,
                          const terrain::TerrainHeightField *terrain,
                          bool allow_consequences) {
    switch (round.motor) {
    case ThrowClass::kNade:
        return motor_nade(world, sim, round, ammo, collision, terrain,
                          allow_consequences);
    case ThrowClass::kSatchel:
    case ThrowClass::kAVMine: // AT mine authors move_function schl
        return motor_charge(world, sim, round, ammo, collision, terrain, false,
                            allow_consequences);
    case ThrowClass::kClaymore:
        return motor_charge(world, sim, round, ammo, collision, terrain, true,
                            allow_consequences);
    default:
        // no motor bound: the round drifts unmoved (retail leaves +452 null)
        return true;
    }
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
    seed.yaw = static_cast<int16_t>(
            std::lround(mission_yaw_deg_from_bam_heading(round.yaw_bam)));
    seed.pitch = item_angle_degrees_from_bam(round.pitch_bam);
    seed.roll = item_angle_degrees_from_bam(round.roll_bam);
    seed.ground_target = parent_handle;
    seed.team = round.team;
    seed.item_id = item_id;
    if (const ThrowableClassRow *row = classes.get(item_id)) {
        // [orig: Entity_InitFromItemDef @ 0x49e550 — Health/Armor from the def]
        if (row->health_max != 0) {
            seed.health = row->health_max;
            seed.health_max = row->health_max;
        }
        seed.armor_impact = row->armor_impact;
        seed.armor_kz = row->armor_kz;
    }
    // Health/armor come from the item def via the host traits sweep
    // [orig: Entity_InitFromItemDef @ 0x49e550 Health = def healthMax]; the
    // sweep lifts health when traits resolve. Until then the device is
    // shootable at the default.
    // The device is shootable through the bounded item-sphere fallback until a
    // real collision instance resolves (the D-ITEM-1 stand-in semantics).
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
    d.hit_word = round.shot_seq;
    d.ammo_index = round.ammo_index;
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
    if (parent_entity != nullptr) {
        d.parent_offset = Vec3{round.pos.x - parent_entity->position.x,
                               round.pos.y - parent_entity->position.y,
                               round.pos.z - parent_entity->position.z};
        d.parent_yaw_at_stick = bam_heading_from_mission_yaw_deg(parent_entity->yaw);
        d.device_yaw_at_stick = round.yaw_bam;
    }
    devices.push_back(d);

    ThrowableEvents::DeviceSpawn ev;
    ev.entity = handle.packed;
    ev.item_friendly = d.item_friendly;
    ev.item_enemy = d.item_enemy;
    ev.team = d.team;
    ev.pos = d.pos;
    ev.yaw_bam = d.yaw_bam;
    ev.pitch_bam = d.pitch_bam;
    ev.roll_bam = d.roll_bam;
    events.spawns.push_back(ev);
    return true;
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
        const AmmoTableEntry *ammo = world.ammo.by_index(d.ammo_index);
        if (ammo == nullptr) continue;
        if (!strutil::iequals(ammo->name, "satchel")) continue;
        Entity *e = entity_for_lifetime(world, d.entity, d.entity_spawn_id);
        if (e != nullptr) e->health = -1;
    }
}

void ThrowableSim::remove_devices_by_owner(World &world, EntityHandle owner) {
    // [orig: Server_ProcessPlayerDeath -> Entity_RemovePlacedDevicesByOwner
    // @ 0x546e00 -> Server_RemoveEntityAndNotify @ 0x50a270 — silent removal.]
    const Entity *live_owner = world.registry.get(owner);
    if (live_owner == nullptr) return;
    for (PlacedDevice &d : devices) {
        if (!d.active || d.owner.packed != owner.packed ||
            d.owner_spawn_id != live_owner->registry_spawn_id)
            continue;
        remove_device(world, d);
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
    const int boom_index = world.ammo.index_of(boom_ammo_name);
    if (boom_index >= 0) {
        RoundSpawnParams p;
        p.owner = device.owner;
        p.shooter_handle = device.owner_handle;
        p.origin = device.pos;
        p.dir_yaw_bam = device.yaw_bam;
        p.dir_pitch_bam = device.pitch_bam;
        p.ammo_index = boom_index;
        p.shot_seq = device.hit_word;
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
    const int32_t eye[3] = {to_fixed(device.pos.x), to_fixed(device.pos.y),
                            to_fixed(device.pos.z) + 19660}; // [orig: +0.3u]
    for (size_t s = 0; s < cap; ++s) {
        const EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
        const Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        if ((e->engine_flags & 0x02000001u) != 0) continue; // dead/inactive
        if (h.packed == device.parent.packed && vehicles) continue;
        // [orig: same team skipped unless the TeamTriggerClaymore rule 0x8000]
        if (e->team == device.team && !team_trigger_claymore) continue;
        if (vehicles) {
            if (e->item_type != 1) continue; // [orig: def kind +92 == 1]
            // moving gate [orig: |entity+0x29C| >= 3276 = 0.05 u/t]
            if (std::abs(e->veh.speed) < 3276) continue;
        }
        const double dx = double(e->position.x) - double(device.pos.x);
        const double dy = double(e->position.y) - double(device.pos.y);
        const double dz = double(e->position.z) - double(device.pos.z);
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > double(max_range_units)) continue;
        // The original's radians-to-BAM constant is negative, making its
        // minus-Yaw-minus-converted expression equal bearing minus Yaw.
        // Subtraction and absolute value wrap in 32 bits
        // [orig: @ 0x43cc9d..0x43ccfd].
        const int32_t bearing = static_cast<int32_t>(
                static_cast<uint32_t>(std::llround(std::atan2(dy, dx) * kBamPerRad)));
        const int32_t signed_diff = static_cast<int32_t>(
                static_cast<uint32_t>(bearing) -
                static_cast<uint32_t>(device.yaw_bam));
        const uint32_t diff = signed_diff < 0
                                      ? uint32_t(0) - static_cast<uint32_t>(signed_diff)
                                      : static_cast<uint32_t>(signed_diff);
        if (diff > static_cast<uint32_t>(cone_half_bam))
            continue;
        // The retail query tests terrain plus sector solids, excluding the
        // device and candidate themselves. CollisionWorld::raycast_clear is
        // the shared full-query structural port; collision-less harnesses keep
        // the terrain leaf as their host fallback.
        // [orig: Entity_FindEnemyInCone @ 0x43cba0 ->
        // Physics_RaycastSegment @ 0x415550]
        const int32_t b[3] = {to_fixed(e->position.x), to_fixed(e->position.y),
                              to_fixed(e->position.z) + 19660};
        if (queries != nullptr) {
            if (!queries->raycast_clear(world, eye, b, device.entity, h)) continue;
        } else if (terrain != nullptr && los_terrain_blocked(*terrain, eye, b)) {
            continue;
        }
        return true;
    }
    return false;
}

void ThrowableSim::tick(World &world, CollisionWorld *collision,
                        const terrain::TerrainHeightField *terrain) {
    for (PlacedDevice &d : devices) {
        if (!d.active) continue;
        Entity *e = entity_for_lifetime(world, d.entity, d.entity_spawn_id);
        if (e == nullptr) {
            d.active = false;
            continue;
        }
        // Owner death/leave removes the device silently [orig:
        // Server_ProcessPlayerDeath @ 0x5178d8 / Server_RemoveEntityAndNotify
        // @ 0x50a270 -> Entity_RemovePlacedDevicesByOwner @ 0x546e00].
        {
            const Entity *owner = entity_for_lifetime(world, d.owner, d.owner_spawn_id);
            if (owner == nullptr || owner->health <= 0) {
                remove_device(world, d);
                continue;
            }
        }
        // ride the stuck-to parent
        if (d.parent.valid()) {
            Entity *parent = entity_for_lifetime(world, d.parent, d.parent_spawn_id);
            if (parent != nullptr) {
                const int32_t parent_yaw = bam_heading_from_mission_yaw_deg(parent->yaw);
                const int32_t dyaw = static_cast<int32_t>(
                        static_cast<uint32_t>(parent_yaw) -
                        static_cast<uint32_t>(d.parent_yaw_at_stick));
                const double a = double(dyaw) / kBamPerRad;
                const double ca = std::cos(a), sa = std::sin(a);
                d.pos.x = parent->position.x +
                          static_cast<float>(double(d.parent_offset.x) * ca -
                                             double(d.parent_offset.y) * sa);
                d.pos.y = parent->position.y +
                          static_cast<float>(double(d.parent_offset.x) * sa +
                                             double(d.parent_offset.y) * ca);
                d.pos.z = parent->position.z + d.parent_offset.z;
                d.yaw_bam = static_cast<int32_t>(
                        static_cast<uint32_t>(d.device_yaw_at_stick) +
                        static_cast<uint32_t>(dyaw));
                e->position = d.pos;
                e->yaw = static_cast<int16_t>(
                        std::lround(mission_yaw_deg_from_bam_heading(d.yaw_bam)));
                e->pitch = item_angle_degrees_from_bam(d.pitch_bam);
                e->roll = item_angle_degrees_from_bam(d.roll_bam);
                e->ground_target = d.parent;
            } else {
                d.parent = EntityHandle{};
                d.parent_spawn_id = 0;
                e->ground_target = EntityHandle{};
            }
        }
        // ARM delay [orig: Entity_UpdatePool1Slot age -1/tick, think at <= 0].
        if (d.think_delay_ticks > 0) {
            --d.think_delay_ticks;
            continue;
        }
        const AmmoTableEntry *ammo = world.ammo.by_index(d.ammo_index);
        if (ammo == nullptr) continue;
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
                const int shrap = world.ammo.index_of("claymoreshrapnel");
                if (shrap >= 0) {
                    RoundSpawnParams p;
                    p.owner = d.owner;
                    p.shooter_handle = d.owner_handle;
                    p.origin = d.pos;
                    p.dir_yaw_bam = d.yaw_bam;
                    p.dir_pitch_bam = d.pitch_bam;
                    p.ammo_index = shrap;
                    p.shot_seq = d.hit_word;
                    world.round_sim.spawn(world, p);
                }
                detonate_device(world, d, "claymorekillzone");
            }
            break;
        }
        case ThrowClass::kAVMine: {
            // [orig: Entity_AVMineThink @ 0x443BB0 — Health <= 0 or the moving
            // vehicle cone (data-dead in retail JO: pieslice 0).]
            if (dead || enemy_in_cone(world, collision, terrain, d,
                                      ammo->kz_minradius,
                                      ammo->kz_pieslice_bam, true))
                detonate_device(world, d, "AV_Minekillzone");
            break;
        }
        default:
            // kNade never places; kLandmine (mission minefield items) stays
            // unported — D-THROW-6.
            if (dead) remove_device(world, d);
            break;
        }
    }
    // compact released slots
    size_t w = 0;
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i].active) devices[w++] = devices[i];
    devices.resize(w);
}

} // namespace opennova::world
