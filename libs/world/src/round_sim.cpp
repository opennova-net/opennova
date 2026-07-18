// Authoritative round flight + damage. See round_sim.h for the witness map and the
// tracked reimpl deferrals. docs/net/novaworld-net-re.md §5.60.
#include "world/round_sim.h"

#include <cmath>

#include "terrain/height_field.h"
#include "world/ammo_table.h"
#include "world/angle.h"
#include "world/infantry.h"
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

// Normalized flight direction for the impact descriptor
// [orig: Projectile_SpawnImpactEffect @ 0x4e9b80].
Vec3 flight_direction(const Vec3 &vel) {
    const float len = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    if (len <= 0.0f) return Vec3{0.0f, 0.0f, 1.0f};
    return Vec3{vel.x / len, vel.y / len, vel.z / len};
}

} // namespace

void RoundSim::reset() noexcept {
	rounds = {};
	active_count = 0;
	deaths.clear();
	impacts.clear();
	hits.clear();
	fired.clear();
	next_impact_order = 1;
	trails.reset(); // [orig: the pool memset in CEffectEmitterPool_ResetAndBuildStyles
	                //  @ 0x5db3b0, run from Game_StartMission]
}

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

    // Wire fire direction -> mission-frame unit vector. The 0x06 yaw BAM IS the mission
    // bearing directly — NOT the 0x0A euler_z heading frame (which is 90deg - mission
    // yaw): wire-validated on the v29 duel baselines (wire yaw -122.0/45.6 deg vs true
    // shooter->victim bearings -122.4/44.2 deg; the old 90-minus mapping missed by 26 deg
    // and only coincided on the 45-deg diagonal — the asymmetric-kill bug, D-NET-153).
    // The original spawner builds X=sinYaw*cosPitch, Y=cosYaw*cosPitch, Z=sinPitch in
    // engine axes [orig: RoundData_SpawnRound @0x4ec5e9 / Weapon_SpawnSingleProjectile
    // @0x4ebf51]; in this mission frame that lands as (cos yaw, sin yaw, sin pitch).
    const double bearing = double(params.dir_yaw_bam) * kRadPerBam;
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

    // The tracer decision [orig: RoundData_SpawnRound @0x4ec184-0x4ec1e5]: every
    // tracer_rate-th round per shooter is a tracer (the counter lives on the weapon
    // slot +0x80 in the original — ours rides the shooter entity, one weapon per NPC
    // today); rate 0 = never; no shooter = every round; the FORCETRACER ammo flag
    // (0x8000) rides every round. Team = the shooter team byte [orig: round+0x162
    // copy @0x4ec705; the slot+4 & 0x200 0xFF override is unmodeled].
    bool tracer = true;                    // [orig: var init @0x4ec15a]
    Entity *owner_ent = world.registry.get(params.owner);
    if (ammo->tracer_rate == 0) {
        tracer = false;                    // [orig: @0x4ec18a]
    } else if (owner_ent != nullptr) {
        // [orig: @0x4ec199-0x4ec1bb: ++counter, wrap to 0 at >= rate, tracer on wrap]
        if (++owner_ent->tracer_shot_counter >= ammo->tracer_rate)
            owner_ent->tracer_shot_counter = 0;
        tracer = (owner_ent->tracer_shot_counter == 0);
    }                                      // [orig: @0x4ec1cf no slot + rate != 0 -> stays true]
    if ((ammo->flags & 0x8000u) != 0) tracer = true; // [orig: forcetracer @0x4ec1db]
    r.tracer = tracer;
    // No shooter -> team 0xFF (always the ENEMY style on every client) [orig: the
    // !sourceEntity arm @ 0x4ec721; the slot+4 & 0x200 0xFF override is unmodeled].
    r.team = owner_ent != nullptr ? static_cast<uint8_t>(owner_ent->team) : 0xFF;

    // The tracer VISUAL — a trail channel allocated at spawn, styled friendly/enemy
    // against the presenting client's team [orig: RoundData_SpawnRound @ 0x4ec740:
    // gate (tracer && !(NoTracers rules & 1)) || FORCETRACER; style id = ammo
    // tracer_type friendly (+232) when round team == local team or shooter == local
    // player, else enemy (+236); id 0 = no channel; -> round+0x2B4].
    r.trail_slot = -1;
    const bool forcetracer = (ammo->flags & 0x8000u) != 0;
    if ((tracer && !no_tracers_rule) || forcetracer) {
        const bool friendly = (local_player.valid() && params.owner.valid() &&
                               params.owner.packed == local_player.packed) ||
                              r.team == local_team;
        const int32_t style = friendly ? ammo->tracer_type_friendly : ammo->tracer_type_enemy;
        if (style != 0) r.trail_slot = trails.alloc(style);
    }

    // Record the fire for the host present layer (sound + muzzle effect) — the
    // inline-presentation moment of the original [orig: WeaponSlot_FireAndSpawnEffects
    // @0x53f440 runs its presentation right after Entity_FireWeaponAndSendPacket].
    FireEvent fe;
    fe.shooter = params.owner;
    fe.shooter_handle = params.shooter_handle;
    fe.ammo_index = params.ammo_index;
    fe.origin = params.origin;
    fe.yaw_bam = params.dir_yaw_bam;
    fe.pitch_bam = params.dir_pitch_bam;
    fired.push_back(fe);

    ++active_count;
    return slot;
}

void RoundSim::tick(World &world, const terrain::TerrainHeightField *terrain) {
    // The trail pool drains once per logic tick even with no live rounds — dead
    // rounds' streaks fade out over cap + count ticks [orig: CEffectEmitterPool_Tick
    // runs unconditionally per frame from Game_ProcessMainFrame @ 0x526758].
    if (active_count <= 0) {
        trails.tick();
        return;
    }

    const size_t pool0 = world.registry.pool_capacity(0);

    for (int i = 0; i < kCapacity; ++i) {
        LiveRound &r = rounds[static_cast<size_t>(i)];
        if (!r.active) continue;

        // Lifetime [orig: projectile+684 remaining-age check @0x4e9dae].
        if (++r.age_ticks > r.max_age_ticks) {
            // Round death: final trail point + drain request [orig:
            // Projectile_ReleaseEffects @ 0x4e8280 — append the current anchor,
            // CEffectChannel_RequestKill].
            if (r.trail_slot >= 0) {
                trails.append(r.trail_slot, r.pos);
                trails.request_kill(r.trail_slot);
            }
            r.active = false;
            --active_count;
            continue;
        }

        // One trail point per tick at the PRE-move position — the streak head trails
        // one tick behind the round [orig: Projectile_UpdatePhysics appends via
        // Projectile_GetTrailAnchorPos before the position update, guided @ 0x4ea04f
        // and ballistic @ 0x4ea97a; the rocket-spiral anchor offset (+0x2AC block)
        // waits on the guided-round port].
        if (r.trail_slot >= 0) trails.append(r.trail_slot, r.pos);

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
            float prev_t = 0.0f;
            float prev_above = terrain::height_field_height_world_bilinear(*terrain, p0.x, -p0.y);
            prev_above = p0.z - prev_above; // clearance at the segment start
            for (int st = 1; st <= steps; ++st) {
                const float t = static_cast<float>(st) / static_cast<float>(steps);
                const float sx = p0.x + r.vel.x * t;
                const float sy = p0.y + r.vel.y * t;
                const float sz = p0.z + r.vel.z * t;
                const float ground =
                        terrain::height_field_height_world_bilinear(*terrain, sx, -sy);
                const float above = sz - ground;
                if (above <= 0.0f) {
                    // Refine the crossing between the last above-ground sample and this
                    // one, so the stop (and the impact-effect point) sits ON the surface
                    // rather than up to a whole 2-unit sub-step under it — the original
                    // stops at the exact heightmap raycast hit
                    // [orig: Terrain_RaycastHeightmapHiRes @ 0x610890 from
                    //  Projectile_UpdatePhysics].
                    terrain_t = t;
                    if (prev_above > 0.0f && prev_above - above > 0.0001f)
                        terrain_t = prev_t + (t - prev_t) * (prev_above / (prev_above - above));
                    break;
                }
                prev_t = t;
                prev_above = above;
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
                    // Sticky SHOT relations, written only when damage is actually
                    // processed (a discarded friendly-fire hit writes nothing)
                    // [orig: Projectile_ProcessDamageOnTarget @ 0x4e80ae..0x4e80ef;
                    //  FF gate Server_IsEntityValidForUpdate @ 0x4e74f0].
                    if (const Entity *shooter = world.registry.get(r.owner)) {
                        auto &rel = world.relations;
                        const int sg = shooter->group_id, ss = shooter->net_id;
                        const int vg = best_target->group_id, vs = best_target->net_id;
                        rel.set_group_group(TriggerRelations::kShot, sg, vg);
                        rel.set_single_group(TriggerRelations::kShot, ss, vg);
                        rel.set_group_single(TriggerRelations::kShot, sg, vs);
                        rel.set_single_single(TriggerRelations::kShot, ss, vs);
                    }
                    best_target->health -= damage;
                    // Publish the processed hit for the AI reaction stamps (wasHit /
                    // lastAttacker / the SM damage event) — drained by AiSystem::tick.
                    hits.push_back(RoundHit{EntityHandle{best_handle}, r.owner, damage});
                    if (best_target->health <= 0) {
                        // The kill selects the death anim at DAMAGE time [orig:
                        // Entity_HandleDamageTrigger @0x407478/@0x407483 — bone from
                        // the hit record, quadrant from the round's horizontal
                        // velocity vs the victim's heading, cause 1 bullet]. The
                        // body-cylinder hit model has no bone zones yet, so the bone
                        // stands in as 1 (torso) — the same bone the explosive path
                        // hardcodes for persons [orig: @0x4e6ac7] (D-AI-9).
                        const int32_t heading_bam =
                                bam_heading_from_mission_yaw_deg(best_target->yaw);
                        const int quadrant =
                                death_quadrant_from_round(heading_bam, r.vel.x, r.vel.y);
                        best_target->death_anim_state =
                                compute_death_anim_state(1, quadrant, death_cause::kBullet);
                        // A member's death stamps its group alert red [orig: the
                        // type-1 head @0x4073db-0x4073ea SetAlertRed(commandGroup)].
                        world.relations.group(best_target->group_id).alert =
                                TriggerRelations::kAlertRed;
                    }
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
            // The entity impact effect: pool-0 organics only at this altitude, so the
            // tag is always 2 'player' [orig: entity material + 4 selection @ 0x4e8867;
            // the vehicle/static material plumb waits on that hit-test port].
            RoundImpact imp;
            imp.position = Vec3{p0.x + r.vel.x * best_t, p0.y + r.vel.y * best_t,
                                p0.z + r.vel.z * best_t};
            imp.direction = flight_direction(r.vel);
            imp.ammo_index = r.ammo_index;
            imp.effect_tag = 2; // 'player' [orig: g_AmmoEffectTagTable @ 0x813420]
            imp.tick = world.logic_tick;
            imp.source_order = next_impact_order++;
            if (impacts.size() < kMaxPendingImpacts) impacts.push_back(imp);
            // [orig: the impact handlers run Projectile_ReleaseEffects @ 0x4e8280 —
            // the trail's final point is the PRE-move anchor (position updates after
            // the handlers @ 0x4eaa3f), so the streak ends a tick short of the hit
            // and the impact flash covers the gap.]
            if (r.trail_slot >= 0) {
                trails.append(r.trail_slot, r.pos);
                trails.request_kill(r.trail_slot);
            }
            r.active = false;
            --active_count;
            continue;
        }
        if (terrain_t <= 1.0f) {
            // Terrain impact [orig: Projectile_HandleTerrainImpact @0x4e9210 — effects
            // only at this altitude].
            // The terrain impact effect: surface type + 4. The surface-map (charmap)
            // sampler is a tracked deferral, so every terrain hit takes the original's
            // own no-surface-map default (type 1 -> tag 5 'dirt')
            // [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510 returns 1 when no map
            //  is loaded; +4 shift @ 0x4e8862].
            RoundImpact imp;
            imp.position = Vec3{p0.x + r.vel.x * terrain_t, p0.y + r.vel.y * terrain_t,
                                p0.z + r.vel.z * terrain_t};
            imp.direction = flight_direction(r.vel);
            imp.ammo_index = r.ammo_index;
            imp.effect_tag = 1 + 4;
            imp.tick = world.logic_tick;
            imp.source_order = next_impact_order++;
            if (impacts.size() < kMaxPendingImpacts) impacts.push_back(imp);
            if (r.trail_slot >= 0) { // [orig: Projectile_ReleaseEffects @ 0x4e8280]
                trails.append(r.trail_slot, r.pos);
                trails.request_kill(r.trail_slot);
            }
            r.active = false;
            --active_count;
            continue;
        }

        r.pos = p1;
    }

    // Pool drain after the round updates: appends this tick reset their channels'
    // age, so live streaks hold shape and only dead ones shrink
    // [orig: CEffectEmitterPool_Tick @ 0x5db830, once per 62 Hz frame].
    trails.tick();
}

} // namespace opennova::world
