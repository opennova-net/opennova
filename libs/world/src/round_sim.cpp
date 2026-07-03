// Authoritative round flight + damage. See round_sim.h for the witness map and the
// tracked reimpl deferrals. docs/net/novaworld-net-re.md §5.60.
#include "world/round_sim.h"

#include <cmath>

#include "terrain/height_field.h"
#include "world/ammo_table.h"
#include "world/world.h"

namespace opennova::world {

namespace {

constexpr double kPi = 3.14159265358979323846;
// BAM32 -> radians (full turn = 2^32) [orig: engine-wide BAM convention, angle.h].
constexpr double kRadPerBam = (2.0 * kPi) / 4294967296.0;

// The MVP organic hit shape: a sphere over the torso. The witnessed hit test is the
// proximity list + bone-section collision (Projectile_RaycastProximitySlots @ 0x4E5340,
// Entity_ComputeBoneCollisionBounds) — this stands in until the collision-model port
// (tracked, §5.60), so every hit is a BODY hit (zone multiplier 1.0).
constexpr float kOrganicCenterZ = 0.9f;
constexpr float kOrganicRadius = 0.6f;

struct SegHit {
    bool hit = false;
    float t = 1.0e9f;
};

// Earliest intersection parameter of segment p0->p1 with a sphere.
SegHit segment_vs_sphere(const Vec3 &p0, const Vec3 &p1, const Vec3 &c, float r) {
    SegHit out;
    const float dx = p1.x - p0.x, dy = p1.y - p0.y, dz = p1.z - p0.z;
    const float fx = p0.x - c.x, fy = p0.y - c.y, fz = p0.z - c.z;
    const float a = dx * dx + dy * dy + dz * dz;
    if (a <= 0.0f) return out;
    const float b = 2.0f * (fx * dx + fy * dy + fz * dz);
    const float cc = fx * fx + fy * fy + fz * fz - r * r;
    const float disc = b * b - 4.0f * a * cc;
    if (disc < 0.0f) return out;
    const float sq = std::sqrt(disc);
    float t = (-b - sq) / (2.0f * a);
    if (t < 0.0f) t = (-b + sq) / (2.0f * a); // started inside
    if (t < 0.0f || t > 1.0f) return out;
    out.hit = true;
    out.t = t;
    return out;
}

// The kinetic damage number [orig: Weapon_CalcImpactDamage @ 0x4EC920]. `vel` is
// units/tick; the original computes (62 * |vel|_16.16) >> 16 = units/second, clamps to
// 1219 (@0x4ecad6), scales by weight_in_grains / 875 (@0x4ecb1a), applies the hit-zone
// multiplier (body = 1.0 — bone zones deferred) and the shooter-class byte (0.9 / 1.1 —
// the slot+89688 per-ammo class table, unported), floors at min_damage (@0x4ecb3a) and
// caps at max_damage when > 0 (@0x4ecb42). Authority-only by construction: only the host
// runs this sim at all (the original returns 0 for non-authority peers @0x4ec933).
int32_t calc_impact_damage(const Vec3 &vel, const AmmoTableEntry &ammo) {
    const double speed_per_tick =
            std::sqrt(double(vel.x) * vel.x + double(vel.y) * vel.y + double(vel.z) * vel.z);
    int32_t speed_scaled = static_cast<int32_t>(62.0 * speed_per_tick);
    if (speed_scaled >= 1219) speed_scaled = 1219;
    int32_t damage = speed_scaled * ammo.weight_in_grains / 875;
    // Zone multiplier 1.0 (body); class multiplier absent — tracked deferrals.
    if (damage <= ammo.min_damage) damage = ammo.min_damage;
    if (ammo.max_damage > 0 && damage >= ammo.max_damage) damage = ammo.max_damage;
    return damage;
}

} // namespace

int RoundSim::spawn(World &world, const RoundSpawnParams &params) {
    const AmmoTableEntry *ammo = world.ammo.by_index(params.ammo_index);
    // Null/non-ballistic ammo spawns nothing at this altitude: the Knife(1)/Medic(3)
    // kill zones are the immediate-raycast leaves [orig: kztype dispatch @0x4ec21f],
    // `hasitem` ammo places an item entity instead of flying.
    if (ammo == nullptr || ammo->velocity <= 0) return -1;
    if (ammo->kztype == 1 || ammo->kztype == 3) return -1;
    if ((ammo->flags & 0x200u) != 0) return -1; // hasitem

    int slot = -1;
    for (int i = 0; i < kCapacity; ++i) {
        if (!rounds[static_cast<size_t>(i)].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1; // pool exhausted [orig: allocator scan @0xB7DFA0 flags]

    // Wire fire direction -> mission-frame unit vector. The engine-frame yaw BAM maps to
    // a mission bearing via the same (90deg - yaw) inversion the 0x0A heading uses
    // (angle.h, entity_wire_bridge); pitch is mission-signed (positive = up), planar
    // component scaled by cos(pitch) [orig: the SpawnRound yaw/pitch trig @0x4ec430].
    const double yaw_rad = double(params.dir_yaw_bam) * kRadPerBam;
    const double bearing = kPi / 2.0 - yaw_rad;
    const double pitch = double(params.dir_pitch_bam) * kRadPerBam;
    const double cp = std::cos(pitch);
    const double speed_per_tick = double(ammo->velocity) / 62.0; // [orig: speed/62 @0x4ec508]

    LiveRound &r = rounds[static_cast<size_t>(slot)];
    r.active = true;
    r.owner = params.owner;
    r.shooter_handle = params.shooter_handle;
    r.ammo_index = params.ammo_index;
    r.adm_index = params.adm_index;
    r.shot_seq = params.shot_seq;
    r.pos = params.origin;
    r.vel.x = static_cast<float>(std::cos(bearing) * cp * speed_per_tick);
    r.vel.y = static_cast<float>(std::sin(bearing) * cp * speed_per_tick);
    r.vel.z = static_cast<float>(std::sin(pitch) * speed_per_tick);
    r.age_ticks = 0;
    // noage rounds never expire on time [orig: flag 0x4000]; everything else uses the
    // ammo max_age (already in 62 Hz ticks).
    r.max_age_ticks = ((ammo->flags & 0x4000u) != 0) ? INT32_MAX : ammo->max_age_ticks;
    ++active_count;
    return slot;
}

void RoundSim::tick(World &world, const terrain::TerrainHeightField *terrain) {
    if (active_count <= 0) return;

    const size_t pool0 = world.registry.pool_capacity(0);

    for (int i = 0; i < kCapacity; ++i) {
        LiveRound &r = rounds[static_cast<size_t>(i)];
        if (!r.active) continue;

        // Lifetime [orig: projectile+684 remaining-age check @0x4e9dae].
        if (++r.age_ticks > r.max_age_ticks) {
            r.active = false;
            --active_count;
            continue;
        }

        const Vec3 p0 = r.pos;
        Vec3 p1{p0.x + r.vel.x, p0.y + r.vel.y, p0.z + r.vel.z};

        // Earliest organic hit along this tick's segment [orig: proximity-list raycast
        // @0x4ea263..; pool-0 only at this altitude — §5.60 deferrals].
        float best_t = 2.0f;
        Entity *best_target = nullptr;
        uint16_t best_handle = 0xFFFF;
        const AmmoTableEntry *ammo = world.ammo.by_index(r.ammo_index);
        const float radius = kOrganicRadius + (ammo != nullptr ? ammo->bullet_radius : 0.0f);
        for (size_t s = 0; s < pool0; ++s) {
            const uint16_t packed = static_cast<uint16_t>(s); // pool 0 -> high nibble 0
            const EntityHandle h{packed};
            if (r.owner.valid() && h.packed == r.owner.packed) continue; // own rounds
            Entity *e = world.registry.get(h);
            if (e == nullptr || e->health <= 0) continue;
            // Indestructible entities take no damage [orig: Flags & 0x4000000 @0x4e7ff6].
            if ((e->engine_flags & 0x4000000u) != 0) continue;
            const Vec3 center{e->position.x, e->position.y, e->position.z + kOrganicCenterZ};
            const SegHit hit = segment_vs_sphere(p0, p1, center, radius);
            if (hit.hit && hit.t < best_t) {
                best_t = hit.t;
                best_target = e;
                best_handle = packed;
            }
        }

        // Terrain stop: first sub-step whose column height swallows the round
        // [orig: Terrain_RaycastHeightmapHiRes segment test in Projectile_UpdatePhysics;
        // bilinear column sampling is the tracked heightfield altitude]. Mission (x, y)
        // maps to the sampler as (x, -y) — the ai.cpp grounding convention.
        float terrain_t = 2.0f;
        if (terrain != nullptr && terrain->valid()) {
            const float seg_len = std::sqrt(r.vel.x * r.vel.x + r.vel.y * r.vel.y +
                                            r.vel.z * r.vel.z);
            const int steps = seg_len > 2.0f ? static_cast<int>(seg_len / 2.0f) + 1 : 1;
            for (int st = 1; st <= steps; ++st) {
                const float t = static_cast<float>(st) / static_cast<float>(steps);
                const float sx = p0.x + r.vel.x * t;
                const float sy = p0.y + r.vel.y * t;
                const float sz = p0.z + r.vel.z * t;
                const float ground =
                        terrain::height_field_height_world_bilinear(*terrain, sx, -sy);
                if (sz <= ground) {
                    terrain_t = t;
                    break;
                }
            }
        }

        if (best_target != nullptr && best_t <= terrain_t) {
            // Entity impact -> authority damage [orig: Projectile_HandleEntityImpact
            // @0x4e9390 -> Projectile_ProcessDamageOnTarget @0x4e7fb0]. Damage clamps to
            // the remaining health [orig: @0x4e8064]; health<=0 raises the death event
            // the host session routes [orig: Entity_CheckAndProcessDeath @0x51b550].
            if (ammo != nullptr) {
                int32_t damage = calc_impact_damage(r.vel, *ammo);
                if (damage > best_target->health) damage = best_target->health;
                if (damage > 0) {
                    best_target->health -= damage;
                    if (best_target->health <= 0) {
                        RoundDeath d;
                        d.victim = EntityHandle{best_handle};
                        d.killer = r.owner;
                        d.victim_handle = best_handle;
                        d.killer_handle = r.shooter_handle;
                        d.adm_index = r.adm_index;
                        deaths.push_back(d);
                    }
                }
            }
            r.active = false;
            --active_count;
            continue;
        }
        if (terrain_t <= 1.0f) {
            // Terrain impact [orig: Projectile_HandleTerrainImpact @0x4e9210 — effects
            // only at this altitude].
            r.active = false;
            --active_count;
            continue;
        }

        r.pos = p1;
    }
}

} // namespace opennova::world
