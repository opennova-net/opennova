// Item destruction — see world/destruction.h for the witness map.
#include "world/destruction.h"

#include <cmath>
#include <cstdlib>

#include "terrain/height_field.h"
#include "world/angle.h"
#include "world/collision.h"
#include "world/infantry.h"
#include "world/world.h"

namespace opennova::world {

namespace {

constexpr double kBamPerRadian = 683565275.5764316; // 2^32 / 2pi

// The witnessed rol-xor PRNG stream the death paths roll [orig: the inline
// dword_31BFBB8 form — v = rol4(state + rol11(state)); state = v ^ 1; the
// low 16 bits are the draw. PRNG_Next16/_B/_C @ 0x6130a0/0x6130f0/0x6131b0 are
// per-module instances of the same generator; one stream stands in for the
// three (piece cosmetics only — tracked in §24).]
uint32_t g_death_rng = 0x1234567u;

inline uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

uint16_t death_rand16() {
    const uint32_t v = rol32(g_death_rng + rol32(g_death_rng, 11), 4);
    g_death_rng = v ^ 1u;
    return static_cast<uint16_t>(v ^ 1u);
}

// [orig: g_death_piece_types @ 0x8404f0 — the 13 named rows, effect/sound slots
// resolved to their interning names ({name, slot} pair tables @ 0x849150 /
// @ 0x82F640). Field decode in world/destruction.h.]
const DeathPieceType kDeathPieceTypes[kDeathPieceTypeCount] = {
    // name        vel   launch spinMn spinMx prob  life bounce trail             bounce_fx             bounce_snd         splash_fx            splash_snd          final_fx          final_snd flags
    {"HULL",       1.0f, 0.0f,  0.0f,  0.0f,  1.0f,  1,  0.0f,  nullptr,          nullptr,              nullptr,           nullptr,             nullptr,            nullptr,          nullptr,  0},
    {"WHEEL",      0.3f, 0.0f,  3.5f,  14.0f, 0.5f,  48, 0.45f, "Effect_VexpM",   "Effect_DustBounceF", "IMP_DEBMED_LAND", "Effect_SmlSplash",  "IMP_DEBMED_WATER", "Effect_BurnScar", nullptr, 1},
    {"CHUNK_S",    0.5f, 0.75f, 2.5f,  16.0f, 1.0f,  12, 0.35f, "Effect_VexpS",   "Effect_DustBounceS", "IMP_DEBSML_LAND", "Effect_SmlSplash",  "IMP_DEBSML_WATER", nullptr,          nullptr,  0},
    {"CHUNK_M",    0.4f, 0.5f,  1.5f,  4.0f,  1.0f,  6,  0.25f, "Effect_VexpM",   "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
    {"CHUNK_L",    0.15f, 0.25f, 0.5f, 1.5f,  1.0f,  1,  0.1f,  "Effect_VexpL",   "Effect_DustBounceF", "IMP_DEBLRG_LAND", "Effect_LargeSplash", "IMP_DEBLRG_WATER", nullptr,         nullptr,  1},
    {"ROCK_S",     0.2f, 0.5f,  1.5f,  4.0f,  1.0f,  12, 0.2f,  "Effect_PDust_S", "Effect_DustBounce",  "IMP_DEBSML_LAND", "Effect_SmlSplash",  "IMP_DEBSML_WATER", nullptr,          nullptr,  0},
    {"ROCK_M",     0.1f, 0.25f, 1.5f,  3.0f,  1.0f,  6,  0.15f, "Effect_PDust_M", "Effect_DustBounce",  "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
    {"ROCK_L",     0.05f, 0.1f, 0.5f,  1.5f,  1.0f,  2,  0.1f,  nullptr,          "Effect_DustBounce",  "IMP_DEBLRG_LAND", "Effect_LargeSplash", "IMP_DEBLRG_WATER", nullptr,         nullptr,  1},
    {"CHUNKNP_S",  0.5f, 0.75f, 2.5f,  16.0f, 1.0f,  12, 0.35f, nullptr,          "Effect_DustBounceS", "IMP_DEBSML_LAND", "Effect_SmlSplash",  "IMP_DEBSML_WATER", nullptr,          nullptr,  0},
    {"CHUNKNP_M",  0.4f, 0.5f,  1.5f,  4.0f,  1.0f,  6,  0.25f, nullptr,          "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
    {"CHUNKNP_L",  0.15f, 0.25f, 0.5f, 1.5f,  1.0f,  1,  0.1f,  nullptr,          "Effect_DustBounceS", "IMP_DEBLRG_LAND", "Effect_LargeSplash", "IMP_DEBLRG_WATER", nullptr,         nullptr,  1},
    {"CACTUS_",    0.15f, 0.25f, 0.5f, 1.5f,  1.0f,  1,  0.1f,  nullptr,          "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  2},
    {"CHUNKSF_M",  0.4f, 0.5f,  1.5f,  4.0f,  1.0f,  6,  0.25f, "Effect_VexpSL",  "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
};

// The interned kz ammo names [orig: WeaponDef_ResolveAllReferences @ 0x540270 —
// g_ammo_kz_OrganicBlast @ 0x24E7DBC etc.]. Resolved per queue push against
// World::ammo (the host loads ammo.def before missions run).
constexpr const char *kAmmoKzOrganicBlast = "kz_OrganicBlast";

float vec_len(const Vec3 &v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 vec_sub(const Vec3 &a, const Vec3 &b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }

// LOS between two points: collision-world walk when available (terrain +
// pools), else the terrain leg alone; no data -> clear. Endpoints carry the
// witnessed +0.25 u lift [orig: the +0x4000 z adds @ 0x4eb4ca..0x4eb4f6].
bool blast_los_clear(World &world, CollisionWorld *collision,
                     const terrain::TerrainHeightField *terrain,
                     const Vec3 &from, const Vec3 &to) {
    const int32_t a[3] = {to_fixed(from.x), to_fixed(from.y), to_fixed(from.z + 0.25)};
    const int32_t b[3] = {to_fixed(to.x), to_fixed(to.y), to_fixed(to.z + 0.25)};
    if (collision != nullptr)
        return collision->raycast_clear(world, a, b, EntityHandle{}, EntityHandle{});
    if (terrain != nullptr && terrain->valid())
        return !los_terrain_blocked(*terrain, a, b);
    return true;
}

// The dead-attacker kill-credit walk [orig: @ 0x4eae95..0x4eaece — while the
// candidate is dead and not player-controlled (Flags & 0x100), follow its own
// lastAttacker; two hops witnessed].
EntityHandle resolve_attacker_chain(World &world, EntityHandle owner) {
    EntityHandle resolved = owner;
    for (int hop = 0; hop < 2; ++hop) {
        const Entity *e = world.registry.get(resolved);
        if (e == nullptr) break;
        if (e->health > 0 || (e->engine_flags & 0x100u) != 0) break;
        if (!e->last_attacker.valid()) break;
        resolved = e->last_attacker;
    }
    return resolved;
}

// The cone gate [orig: @ 0x4eafe0..0x4eaffa — atan2(dy, dx) in BAM vs the entry
// direction, |delta| <= the ammo kz_pieslice half-angle].
bool cone_gate(const ExplosionEntry &e, int32_t cone_half_bam, const Vec3 &to_target) {
    if (cone_half_bam == 0) return true;
    const int32_t ang = static_cast<int32_t>(
            std::atan2(static_cast<double>(to_target.y), static_cast<double>(to_target.x)) *
            kBamPerRadian);
    const int32_t delta = ang - e.dir_bam; // wrapping subtract, |.| as the original's abs
    return std::abs(delta) <= cone_half_bam;
}

// Shared health drain for a non-organic victim + the item death notify.
// [orig: the Entity_ApplyWeaponDamage non-person tail @ 0x4e6f0d-0x4e6fc1]
void apply_item_blast_damage(World &world, Entity &target, int32_t damage,
                             EntityHandle attacker, int32_t ammo_index) {
    if ((target.engine_flags & kEntityFlagDead) != 0 || target.health <= 0) return;
    const int32_t before = target.health;
    target.last_attacker = attacker; // [orig: the +0x178 chain store @ 0x4e6f18]
    if (damage < before)
        target.health = before - damage;
    else
        target.health = 0;
    // deathCallback(entity, 2, 0) — the explosion-hit notify [orig: @ 0x4e6f93].
    destruction_notify_item_damage(world, target, 2);
    if (target.health <= 0 && before > 0) {
        // Score_ProcessKillEvent equivalence: stage the death for the host's
        // kill routing (scoring + broadcasts) [orig: @ 0x4e6fb4].
        RoundDeath d;
        d.victim = target.handle;
        d.killer = attacker;
        d.victim_handle = target.handle.packed;
        d.killer_handle = attacker.packed;
        (void)ammo_index;
        world.round_sim.deaths.push_back(d);
    }
}

// The AoE damage applicator for one victim [orig: Entity_ApplyWeaponDamage
// @ 0x4e6820]. `distance` is the surface distance (center distance minus the
// victim's bound radius, clamped at 0 by the caller), `blast_radius` the
// resolved radius.
void entity_apply_weapon_damage(World &world, Entity &target, const ExplosionEntry &e,
                                float distance, float blast_radius) {
    if ((target.engine_flags & kEntityFlagDead) != 0) return; // [orig: Flags & 2 @ 0x4e682e]
    const ItemDeathTraits *traits = world.item_death_traits.get(target.item_id);
    // In-session building gate (g_destroy_buildings) — SP offline skips it
    // [orig: the is_in_session && type==Building && !g_destroy_buildings leg
    // @ 0x4e6860]. Our SP listen-server runs offline semantics; the MP rules
    // bit is a net seam (tracked §24).
    const Entity *owner = world.registry.get(e.owner);
    // Same-team blast immunity when the def authors attrib 0x8000
    // [orig: @ 0x4e688d].
    if (owner != nullptr && traits != nullptr && traits->team_protect &&
        owner->team == target.team)
        return;
    // Indestructible / invulnerable armor word [orig: @ 0x4e68aa].
    if ((target.engine_flags & kEntityFlagIndestructible) != 0) return;
    if (traits != nullptr && traits->armor_blast == -1) return;

    const AmmoTableEntry *ammo = world.ammo.by_index(e.ammo_index);
    if (ammo == nullptr) return;
    // Authority-only base damage [orig: @ 0x4e68cf — non-authority reads 0].
    int32_t damage = ammo->kz_damage; // [orig: ammoDef word +46 @ 0x4e68d5]
    // Linear falloff from kz_minradius to the blast radius; type 4 (radius
    // blast / direct hit) skips it [orig: @ 0x4e695a-0x4e699c].
    if (e.type != ammo_kz::kRadiusBlast && distance > ammo->kz_minradius &&
        blast_radius - ammo->kz_minradius > 0.0f) {
        const float t = (distance - ammo->kz_minradius) /
                        (blast_radius - ammo->kz_minradius);
        damage = static_cast<int32_t>(damage * (1.0f - t) + 0.5f);
    }
    // Blast armor class gate [orig: @ 0x4e69b0 — ammo penetration_kz (+200)
    // must reach def+0x192].
    if (traits != nullptr && ammo->penetration_kz < traits->armor_blast) damage = 0;
    // Already dying [orig: the +0x124 gate @ 0x4e69b8].
    if (target.health <= 0) damage = 0;
    // Occupant damage scale for vehicles (Entity_ApplyOccupantDamageScale
    // @ 0x4e5a50) — internals unwitnessed; occupants take their own pool-0
    // damage from the same sweep (tracked §24).
    // NoDie clamps to health-1 [orig: @ 0x4e69e9].
    if (traits != nullptr && traits->no_die && damage >= target.health)
        damage = target.health - 1;
    if (damage <= 0) return;

    if (target.kind == EntityKind::Organic) {
        // The person path [orig: @ 0x4e6a01-0x4e6c5d]: the death-anim selection
        // at damage time — bone hardcoded 1 (torso @ 0x4e6ac7), quadrant from
        // the blast direction vs the victim's heading, cause 2 or 3 (a ~25%
        // roll @ 0x4e6a84), 4 when the source kz is Slash (type 7 @ 0x4e6abb).
        const int32_t heading_bam = bam_heading_from_mission_yaw_deg(target.yaw);
        const Vec3 from_blast = vec_sub(target.position, e.pos);
        const int quadrant =
                death_quadrant_from_round(heading_bam, -from_blast.x, -from_blast.y);
        int cause = (death_rand16() < 0x4000) ? 3 : 2;
        if (e.type == ammo_kz::kSlash) cause = 4;
        const int32_t before = target.health;
        if ((target.engine_flags & kEntityFlagDead) == 0 && before > 0) {
            target.last_attacker = resolve_attacker_chain(world, e.owner);
            if (damage < before)
                target.health = before - damage;
            else
                target.health = 0;
            target.death_anim_state = compute_death_anim_state(1, quadrant, cause);
            // The processed hit feeds the AI reaction stamps, like a round hit
            // [orig: the deathCallback(2) notify @ 0x4e6b72].
            world.round_sim.hits.push_back(RoundHit{target.handle, e.owner, damage});
            if (target.health <= 0 && before > 0) {
                world.relations.group(target.group_id).alert = TriggerRelations::kAlertRed;
                RoundDeath d;
                d.victim = target.handle;
                d.killer = e.owner;
                d.victim_handle = target.handle.packed;
                d.killer_handle = e.owner.packed;
                world.round_sim.deaths.push_back(d);
            }
        }
        // The person-blast impact effect (AmmoDef_ProcessImpactEffect tag 23
        // "flesh" @ 0x4e6c4b) rides the ammo impact rows the host already
        // presents; the world stays effect-name-free here.
        return;
    }

    // Non-person: the breakable-section sweep (sectionMask marking
    // @ 0x4e6c5e-0x4e6e6b) rides the collision-model section flags — not yet
    // carried by our CollisionModel build (tracked §24/D-ITEM-3).
    apply_item_blast_damage(world, target, damage, resolve_attacker_chain(world, e.owner),
                            e.ammo_index);
}

} // namespace

const DeathPieceType &death_piece_type(int index) {
    if (index < 0 || index >= kDeathPieceTypeCount) index = 0;
    return kDeathPieceTypes[index];
}

// ----------------------------------------------------------------------------
// ExplosionSim
// ----------------------------------------------------------------------------

void ExplosionSim::queue_explosion(World &world, const ExplosionEntry &e) {
    (void)world;
    // [orig: WeaponEffect_QueueExplosion @ 0x4e8330 — authority-only (our sim
    // runs on the authority by construction, the RoundSim rule), 64-entry cap,
    // silent drop when full.]
    if (queue.size() >= static_cast<size_t>(kCapacity)) return;
    queue.push_back(e);
}

void ExplosionSim::process(World &world, CollisionWorld *collision,
                           const terrain::TerrainHeightField *terrain,
                           float water_height, DestructionEvents &events) {
    (void)water_height;
    if (queue.empty()) return;
    // The drain consumes the whole queue and resets it [orig: the queue_index
    // loop @ 0x4ead97 + the count reset @ 0x4eb8a5]. Damage callbacks may push
    // NEW entries (the kz chain) — they land next tick, exactly like the
    // original's post-reset writes.
    std::vector<ExplosionEntry> batch;
    batch.swap(queue);
    for (const ExplosionEntry &e : batch) {
        ++events.explosions_processed;
        const AmmoTableEntry *ammo = world.ammo.by_index(e.ammo_index);
        if (ammo == nullptr) continue;
        // Type dispatch [orig: the switch @ 0x4eadc6]: 2/5/6/7 -> weapon
        // damage; 1 (vehicle ram) and 3 (medic heal) are cited stubs at this
        // altitude (the ram rides the vehicle pass, the medic the revive port).
        if (e.type == ammo_kz::kKnife || e.type == ammo_kz::kMedic) continue;
        float blast_radius = ammo->kz_maxradius; // [orig: E+28 -> +56 @ 0x4eadcd]
        if (e.radius_override != 0.0f)
            blast_radius = e.radius_override;    // [orig: the E+0x2C float @ 0x4eae64]
        if (blast_radius <= 0.0f || ammo->kz_damage == 0) continue;
        const int32_t cone_half = ammo->kz_pieslice_bam; // [orig: E+28 -> +60 @ 0x4eadad]
        const EntityHandle resolved = resolve_attacker_chain(world, e.owner);

        // --- pool 0, organics [orig: @ 0x4eaeda-0x4eb32c] — skipped when the
        // ammo flags NoOItems [orig: the & 0x80000 gate @ 0x4eaece]. ---
        if ((ammo->flags & kAmmoFlagNoOItems) == 0) {
            const size_t pool0 = world.registry.pool_capacity(0);
            for (size_t s = 0; s < pool0; ++s) {
                Entity *t = world.registry.get(EntityHandle::make(0, static_cast<int>(s)));
                if (t == nullptr || (t->engine_flags & 0x1u) != 0) continue;
                const float bound = t->bound_radius > 0.0f ? t->bound_radius : 0.6f;
                // The organic reaction band reaches 2x the blast radius
                // [orig: max_check_range = radius + 2*blast @ 0x4eaf2b].
                const Vec3 d = vec_sub(t->position, e.pos);
                const float reach = bound + 2.0f * blast_radius;
                if (std::abs(d.x) > reach || std::abs(d.y) > reach || std::abs(d.z) > reach)
                    continue;
                const float dist = vec_len(d);
                if (dist > reach) continue;
                if (t->health <= 0 && (t->engine_flags & kEntityFlagDead) != 0) continue;
                if (!cone_gate(e, cone_half, d)) continue;
                float surface = dist - bound;
                if (surface < 0.0f) surface = 0.0f;
                if (surface > blast_radius) continue; // reaction-only band: the
                // flinch/knockback/burn legs (Entity_OnDamageReceived
                // @ 0x4eb05c, Entity_ApplyCollisionForce @ 0x4eb1d2, the
                // attached hit emitter @ 0x4eb292) are tracked stubs — §24.
                // The LOS gate [orig: @ 0x4eb162 — type 4 direct hits skip it].
                if (e.type != ammo_kz::kRadiusBlast &&
                    !blast_los_clear(world, collision, terrain, t->position, e.pos))
                    continue;
                entity_apply_weapon_damage(world, *t, e, surface, blast_radius);
                if (!t->last_attacker.valid()) t->last_attacker = resolved;
            }
        }

        // --- pool 1, movable items [orig: @ 0x4eb334-0x4eb5a8] — NoMItems
        // gate [orig: & 0x100000 @ 0x4eb378]. Types 1/3 never sweep items
        // [orig: @ 0x4eb343]. ---
        if ((ammo->flags & kAmmoFlagNoMItems) == 0) {
            const size_t pool1 = world.registry.pool_capacity(1);
            for (size_t s = 0; s < pool1; ++s) {
                Entity *t = world.registry.get(EntityHandle::make(1, static_cast<int>(s)));
                if (t == nullptr) continue;
                if ((t->engine_flags & 0x1u) != 0 ||
                    (t->engine_flags & kEntityFlagIndestructible) != 0)
                    continue; // [orig: @ 0x4eb3cb]
                const float bound = t->bound_radius > 0.0f ? t->bound_radius : 1.0f;
                const Vec3 d = vec_sub(t->position, e.pos);
                const float reach = bound + blast_radius;
                if (std::abs(d.x) > reach || std::abs(d.y) > reach || std::abs(d.z) > reach)
                    continue;
                const float dist = vec_len(d);
                if (dist > reach) continue;
                // LOS with the witnessed +0.25 lift [orig: @ 0x4eb4ca].
                if (!blast_los_clear(world, collision, terrain, t->position, e.pos))
                    continue;
                if (!cone_gate(e, cone_half, d)) continue;
                // Destructible-class targets record the blast center as the
                // debris launch origin [orig: the deathCallback ==
                // Entity_HandleDestructibleDeathEvent check @ 0x4eb553].
                if (t->health > 0 && !t->is_ai_capable) t->death_blast_center = e.pos;
                float surface = dist - bound;
                if (surface < 0.0f) surface = 0.0f;
                entity_apply_weapon_damage(world, *t, e, surface, blast_radius);
                if (!t->last_attacker.valid()) t->last_attacker = resolved;
            }
        }

        // --- pool 2, static items/buildings [orig: @ 0x4eb5b8-0x4eb88c] —
        // NoDItems gate [orig: & 0x200000 @ 0x4eb5b8]. ---
        if ((ammo->flags & kAmmoFlagNoDItems) == 0) {
            const size_t pool2 = world.registry.pool_capacity(2);
            for (size_t s = 0; s < pool2; ++s) {
                Entity *t = world.registry.get(EntityHandle::make(2, static_cast<int>(s)));
                if (t == nullptr || (t->engine_flags & 0x1u) != 0) continue;
                const float bound = t->bound_radius > 0.0f ? t->bound_radius : 1.0f;
                const Vec3 d = vec_sub(t->position, e.pos);
                const float reach = bound + blast_radius;
                if (std::abs(d.x) > reach || std::abs(d.y) > reach || std::abs(d.z) > reach)
                    continue;
                if (vec_len(d) > reach) continue;
                if (!cone_gate(e, cone_half, d)) continue;
                // The witnessed AABB-face distance refinement rides the model
                // bounds [orig: @ 0x4eb700-0x4eb7f6]; the bound-sphere reach
                // stands in until per-model AABBs reach the world (tracked).
                float surface = vec_len(d) - bound;
                if (surface < 0.0f) surface = 0.0f;
                if (surface > blast_radius) continue;
                // Window shatter at the GLASS1..4 user points [orig:
                // Terrain_SpawnEffectsAtUserPoint x4 @ 0x4eb814-0x4eb85d] —
                // presented by the host from the model user points.
                events.glass_breaks.push_back(GlassBreakEvent{
                        t->net_id, t->bms_id, t->spawn_origin, t->item_id, e.pos,
                        blast_radius});
                if (t->health > 0 && !t->is_ai_capable) t->death_blast_center = e.pos;
                entity_apply_weapon_damage(world, *t, e, surface, blast_radius);
                if (!t->last_attacker.valid()) t->last_attacker = resolved;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// The item death chain
// ----------------------------------------------------------------------------

void destruction_notify_item_damage(World &world, Entity &target, int phase) {
    // [orig: Entity_HandleDestructibleDeathEvent @ 0x440210 — the authority
    // destroys at health <= 0; a client destroys on the net kill phase 4. Our
    // world is the authority by construction; AI-driven vehicles die through
    // their state machine (rows 21/23), not this notify.]
    (void)phase;
    if (target.kind == EntityKind::Organic || target.is_ai_capable) return;
    if ((target.engine_flags & kEntityFlagHusk) != 0) return; // already husked
    if (target.health > 0) return;
    process_destructible_death(world, target);
}

namespace {

// The death sound + effect families + the kz blasts — the shared presentation
// tail every husked death runs [orig: Entity_InitDeathSounds @ 0x4939b0].
void emit_death_sounds_and_effects(World &world, Entity &target, bool silent) {
    const ItemDeathTraits *traits = world.item_death_traits.get(target.item_id);
    DestructionEvents &ev = world.destruction;
    if (!silent && traits != nullptr && !traits->sound_death.empty())
        ev.sounds.push_back(DestructionSoundEvent{traits->sound_death, target.position});
    if (silent || traits == nullptr) return;
    // Fully submerged -> the water death family, else particledeath; both are
    // bone-attached 4-slot banks on the husk (the present pass owns the bones)
    // [orig: the boundRadius + Z < water gate @ 0x493a88].
    const bool submerged =
            target.position.z + target.bound_radius < world.water_height;
    const std::string &family =
            submerged ? traits->particleh2odeath : traits->particledeath;
    if (!family.empty())
        ev.effects.push_back(DestructionEffectEvent{
                family, target.position, Vec3{}, target.net_id, target.bms_id, 1});
    // The fire + other families [orig: the +0x47A / +0x4AE banks @ 0x493b6c/
    // @ 0x493bba]; the present pass runs the per-tick wreck-fire behavior
    // (random crackle, underwater steam-out) on these.
    if (!traits->particlefire.empty())
        ev.effects.push_back(DestructionEffectEvent{traits->particlefire,
                target.position, Vec3{}, target.net_id, target.bms_id, 2});
    if (!traits->particleother.empty())
        ev.effects.push_back(DestructionEffectEvent{traits->particleother,
                target.position, Vec3{}, target.net_id, target.bms_id, 3});
    // The kz blasts: one kz_OrganicBlast r=5.0 per husk KZ user point, else one
    // at the entity with r = kz ?: bound radius [orig:
    // Entity_QueueKzBlastAtUserPoints(g_ammo_kz_OrganicBlast, ..., "KZ", 1, ...)
    // @ 0x493b57; the fallback radius legs @ 0x4ead12-0x4ead68].
    const int kz_ammo = world.ammo.index_of(kAmmoKzOrganicBlast);
    if (kz_ammo >= 0) {
        ExplosionEntry blast;
        blast.type = ammo_kz::kStandard; // kz_OrganicBlast kztype (word +44)
        if (const AmmoTableEntry *a = world.ammo.by_index(kz_ammo))
            blast.type = a->kztype;
        blast.ammo_index = kz_ammo;
        blast.owner = target.handle;
        blast.hit_word = 1;
        if (traits->kz_points.empty()) {
            blast.pos = target.position;
            blast.radius_override =
                    traits->kz > 0.0f ? traits->kz
                                      : (target.bound_radius > 0.0f ? target.bound_radius
                                                                    : 1.0f);
            world.explosions.queue_explosion(world, blast);
        } else {
            const double yaw_rad =
                    (90.0 - static_cast<double>(target.yaw)) * (3.14159265358979 / 180.0);
            const float cy = static_cast<float>(std::cos(yaw_rad));
            const float sy = static_cast<float>(std::sin(yaw_rad));
            for (const Vec3 &p : traits->kz_points) {
                blast.pos = Vec3{target.position.x + p.x * cy - p.y * sy,
                                 target.position.y + p.x * sy + p.y * cy,
                                 target.position.z + p.z};
                blast.radius_override = 5.0f; // [orig: the 5.0 at @ 0x4eace7]
                world.explosions.queue_explosion(world, blast);
            }
        }
    }
}

} // namespace

void process_destructible_death(World &world, Entity &target) {
    // [orig: Entity_ProcessDestructibleDeath @ 0x43fbc0]
    DestructionEvents &ev = world.destruction;
    // Per-section debris burst — sampled by the present pass over the model's
    // collision faces [orig: the Entity_SpawnSectionDebris loop @ 0x43fbd9].
    ev.debris_bursts.push_back(SectionDebrisEvent{target.net_id, target.bms_id,
                                                  target.spawn_origin, target.item_id,
                                                  target.death_blast_center});
    // Scar/decal clear (Scar_ClearEntriesByEntity @ 0x5ccec0) — no decal
    // system yet; tracked §24.
    target.engine_flags |= (kEntityFlagDead | kEntityFlagHusk); // [orig: Flags |= 6 @ 0x43fbf6]
    target.alive = false;
    target.health = 0;
    if (target.death_tick == 0) target.death_tick = world.logic_tick; // [orig: @ 0x43fbfd]
    ++ev.items_destroyed;
    ev.husk_swaps.push_back(HuskSwapEvent{target.net_id, target.bms_id,
                                          target.spawn_origin, target.item_id,
                                          target.spawned_piece_mask});
    // The S2C 0x26 entity-state broadcast (Server_SendEntityStatePacket
    // @ 0x509d70) is the net track's emit — staged with the other MP legs
    // (tracked §24).
    emit_death_sounds_and_effects(world, target, /*silent=*/false);
}

uint32_t spawn_death_pieces(World &world, Entity &target) {
    // [orig: Entity_SpawnDeathPieces @ 0x493400] Gate: not already husked, a
    // husk model exists, not fully underwater.
    const ItemDeathTraits *traits = world.item_death_traits.get(target.item_id);
    if (traits == nullptr || !traits->has_husk) return 0;
    if ((target.engine_flags & kEntityFlagHusk) != 0) return 0;
    if (target.position.z + target.bound_radius < world.water_height) return 0;
    // The explosion glow light (LightPool_SpawnGlowEffect @ 0x49351a, 2x model
    // radius, non-decorations) — no light-pool port (tracked, the D-AI-8d
    // family).
    uint32_t mask = 0;
    const int sections = traits->husk_sub_part_count;
    // The wreck's own motion carries into the launch base at 2x
    // [orig: the velocity fold @ 0x493589-0x4935ff, scale flt 2.0 @ 0x7C3B90].
    const Vec3 base{target.veh.vel_x / 65536.0f * 2.0f,
                    target.veh.vel_y / 65536.0f * 2.0f, 0.0f};
    for (int s = 1; s < sections && s <= 16; ++s) {
        const int type_idx =
                traits->husk_sub_part_types[s <= 15 ? s : 15]; // [orig: clamp @ 0x49362f]
        const DeathPieceType &tp = death_piece_type(type_idx);
        if (tp.probability < 1.0f) {
            // [orig: the probability roll @ 0x49365f — rand16 vs prob*65536]
            if (death_rand16() >= static_cast<uint16_t>(tp.probability * 65536.0f))
                continue;
        }
        DeathPiece &p = world.death_pieces.alloc();
        p.active = true;
        p.item_id = target.item_id;
        p.section = static_cast<uint8_t>(s);
        p.type_index = static_cast<uint8_t>(type_idx);
        p.render_scale = traits->debris_scale > 0.0f ? traits->debris_scale : 1.0f;
        p.pos = target.position; // + the section center, applied by the present
                                 // pass from the husk model [orig: @ 0x4938bf]
        // Launch direction: the wreck-motion base plus a +-0.5 random spread,
        // normalized, scaled by the type velocity with the 1.25 vertical lift
        // [orig: @ 0x493718-0x49380e; flt 1.25 @ 0x7C6F18].
        Vec3 dir{base.x + tp.launch_add +
                         (static_cast<int32_t>(death_rand16()) - 0x8000) / 65536.0f,
                 base.y + tp.launch_add +
                         (static_cast<int32_t>(death_rand16()) - 0x8000) / 65536.0f,
                 static_cast<int32_t>(death_rand16()) / 65536.0f};
        const float len = vec_len(dir);
        if (len > 1.0e-6f) {
            dir.x /= len; dir.y /= len; dir.z /= len;
        } else {
            dir = Vec3{0.0f, 0.0f, 1.0f};
        }
        p.vel = Vec3{dir.x * tp.vel_scale, dir.y * tp.vel_scale,
                     dir.z * tp.vel_scale * 1.25f};
        // Spin rates random-in-range [orig: the sub_57B940 pair @ 0x4937b0].
        const float spin_span = tp.spin_max - tp.spin_min;
        p.spin_a = tp.spin_min + spin_span * (death_rand16() / 65536.0f);
        p.spin_b = tp.spin_min + spin_span * (death_rand16() / 65536.0f);
        p.heading = 0.0f;
        p.pitch = 0.0f;
        // Bounce budget: rand % lifetime + 1, floored at lifetime/8
        // [orig: @ 0x49385b-0x493885].
        int bounces = tp.lifetime > 0 ? (death_rand16() % tp.lifetime) + 1 : 1;
        const int floor_b = tp.lifetime >> 3;
        if (bounces < floor_b) bounces = floor_b;
        if (bounces < 1) bounces = 1;
        p.bounces_left = bounces;
        p.flags = tp.flags;
        p.settled = false;
        // The per-piece trail effect/looped sound attach [orig: @ 0x493813] is
        // presented by the host from the type row (trail_fx).
        mask |= (1u << s);
    }
    target.spawned_piece_mask = mask; // [orig: entity+0x138 @ 0x493983]
    // The post-death update-callback swap (falling/static physics) is the
    // destruction_tick_dead_items pass; the vertical kick:
    // helicopters drop (-0.247), others pop (+0.0625 + rand/16)
    // [orig: @ 0x493969-0x493983 — def type 2 -> slideDecay -= 16182, else
    // += (rand16 >> 4) + 4096].
    return mask;
}

void entity_update_death_transforms(World &world, Entity &target, bool silent) {
    // [orig: Entity_UpdateDeathTransforms @ 0x494660 — pose snapshot (the AI
    // rows already snapshot net_saved_live_pose), then the unitType dispatch,
    // then the death sounds.]
    const ItemDeathTraits *traits = world.item_death_traits.get(target.item_id);
    const int unit_type = traits != nullptr ? traits->unit_type : 0;
    const bool was_husked = (target.engine_flags & kEntityFlagHusk) != 0;
    uint32_t mask = 0;
    // The dispatch table @ 0x815410: every row spawns pieces and ORs Flags 6;
    // buildings (5-8) additionally play the collapse sound and require a husk
    // [orig: Entity_ProcessBuildingDeath @ 0x494420]; bridges (11) add the
    // water shock at DEAD points (present-pass leg); the no-row default also
    // clears 0x20000 [orig: Flags & ~0x20006 | 6 @ 0x493f4b].
    switch (unit_type) {
    case 5: case 6: case 7: case 8:
        if (traits == nullptr || !traits->has_husk) return; // [orig: @ 0x49442c]
        mask = spawn_death_pieces(world, target);
        world.destruction.sounds.push_back(
                DestructionSoundEvent{"EXPLO_SHIP_TINY", target.position});
        break;
    case 11:
        mask = spawn_death_pieces(world, target);
        // Effect_ShockWaterBrdg at each husk DEAD user point at water height
        // [orig: Entity_SpawnDeathEffectsAtBones @ 0x4944c0] — present-pass leg
        // keyed off the husk swap event (bridge husks are rare; tracked §24).
        break;
    default:
        mask = spawn_death_pieces(world, target);
        break;
    }
    target.engine_flags &= ~0x20000u;
    target.engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    target.alive = false;
    if (target.death_tick == 0) target.death_tick = world.logic_tick;
    if (!was_husked) {
        world.destruction.husk_swaps.push_back(
                HuskSwapEvent{target.net_id, target.bms_id, target.spawn_origin,
                              target.item_id, mask});
        ++world.destruction.items_destroyed;
    }
    // The death vertical kick [orig: @ 0x493969 — def type 2 (helicopter
    // family) drops, others pop].
    if (traits != nullptr) {
        if (unit_type == 2)
            target.veh.slide_z -= 16182;
        else
            target.veh.slide_z += (death_rand16() >> 4) + 4096;
    }
    emit_death_sounds_and_effects(world, target, silent);
}

int32_t item_bullet_damage_gate(const World &world, const Entity &target,
                                int32_t damage, int32_t penetration_impact) {
    // [orig: Projectile_ProcessDamageOnTarget @ 0x4e7fb0 zeroing gates]
    if ((target.engine_flags & kEntityFlagIndestructible) != 0) return 0; // @ 0x4e7ff6
    const ItemDeathTraits *traits = world.item_death_traits.get(target.item_id);
    if (traits != nullptr) {
        if (traits->armor_impact == -1) return 0;                 // @ 0x4e8019
        if (penetration_impact < traits->armor_impact) return 0;  // @ 0x4e802a
    }
    if (target.health <= 0) return 0;                             // @ 0x4e8032
    if (damage > target.health) damage = target.health;           // @ 0x4e8064
    if (traits != nullptr && traits->no_die && damage >= target.health)
        damage = target.health - 1;                               // @ 0x4e8074
    return damage;
}

void destruction_tick_dead_items(World &world,
                                 const terrain::TerrainHeightField *terrain,
                                 float water_height,
                                 DestructionEvents &events) {
    (void)events;
    // The dead-item settle [orig: Entity_UpdateStaticDeathPhysics @ 0x494230 /
    // Entity_UpdateFallingDeathPhysics @ 0x493f70 as the post-death update
    // callbacks]. AI vehicles settle in their SM rows (ai.cpp); this pass runs
    // dead NON-AI items with pending vertical motion.
    for (int pool = 1; pool <= 2; ++pool) {
        const size_t cap = world.registry.pool_capacity(pool);
        for (size_t s = 0; s < cap; ++s) {
            Entity *e = world.registry.get(EntityHandle::make(pool, static_cast<int>(s)));
            if (e == nullptr || e->is_ai_capable) continue;
            if ((e->engine_flags & kEntityFlagHusk) == 0) continue;
            if (e->veh.slide_z == 0 && e->veh.vel_x == 0 && e->veh.vel_y == 0) continue;
            // Gravity [orig: -334/tick above water, terminal -4096 + halved
            // horizontal below @ 0x493fe5/@ 0x461ddf].
            if (e->position.z + e->bound_radius >= water_height) {
                e->veh.slide_z -= 334;
            } else {
                e->veh.vel_x >>= 1;
                e->veh.vel_y >>= 1;
                e->veh.slide_z = -4096;
            }
            float ground = -1.0e9f;
            if (terrain != nullptr && terrain->valid())
                ground = terrain::height_field_height_world_bilinear(
                        *terrain, e->position.x, -e->position.y);
            const float new_z = e->position.z + e->veh.slide_z / 65536.0f;
            e->position.x += e->veh.vel_x / 65536.0f;
            e->position.y += e->veh.vel_y / 65536.0f;
            // Horizontal damp x0.97, zero under 8/65536 [orig: @ 0x4942f7-0x494336].
            e->veh.vel_x = static_cast<int32_t>(e->veh.vel_x * 0.97f);
            e->veh.vel_y = static_cast<int32_t>(e->veh.vel_y * 0.97f);
            if (std::abs(e->veh.vel_x) < 8) e->veh.vel_x = 0;
            if (std::abs(e->veh.vel_y) < 8) e->veh.vel_y = 0;
            if (new_z <= ground) {
                // Ground contact [orig: Entity_TransitionToGroundDeath
                // @ 0x493080 + the landing legs @ 0x494113-0x494209].
                e->position.z = ground;
                e->veh.slide_z = 0;
                world.destruction.sounds.push_back(
                        DestructionSoundEvent{"IMP_VCL_DROP", e->position});
                // The landing kz blast for vehicle-family wrecks
                // [orig: @ 0x4941be — r = def kz ?: boundRadius].
                const ItemDeathTraits *traits =
                        world.item_death_traits.get(e->item_id);
                const int unit_type = traits != nullptr ? traits->unit_type : 0;
                if (unit_type == 1 || unit_type == 2 || unit_type == 10 ||
                    unit_type == 12) {
                    const int kz_ammo = world.ammo.index_of(kAmmoKzOrganicBlast);
                    if (kz_ammo >= 0) {
                        ExplosionEntry blast;
                        if (const AmmoTableEntry *a = world.ammo.by_index(kz_ammo))
                            blast.type = a->kztype;
                        blast.ammo_index = kz_ammo;
                        blast.owner = e->handle;
                        blast.hit_word = 1;
                        blast.pos = e->position;
                        blast.radius_override =
                                (traits != nullptr && traits->kz > 0.0f)
                                        ? traits->kz
                                        : (e->bound_radius > 0.0f ? e->bound_radius
                                                                  : 1.0f);
                        world.explosions.queue_explosion(world, blast);
                    }
                }
            } else {
                e->position.z = new_z;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// DeathPieceSim
// ----------------------------------------------------------------------------

DeathPiece &DeathPieceSim::alloc() {
    // [orig: DeathPiece_AllocSlot @ 0x57b4f0 — a plain ring; the next slot is
    // reused even if still live.]
    DeathPiece &p = pieces[static_cast<size_t>(cursor)];
    cursor = (cursor + 1) % kCapacity;
    p = DeathPiece{};
    return p;
}

void DeathPieceSim::tick(World &world, const terrain::TerrainHeightField *terrain,
                         float water_height, DestructionEvents &events) {
    (void)world;
    // [orig: DeathPiece_TickAll @ 0x57b900 -> Entity_ProcessDeathPiecePhysics
    // @ 0x492dd0]
    constexpr float kGravity = 334.0f / 65536.0f; // the falling-death gravity
    for (DeathPiece &p : pieces) {
        if (!p.active || p.settled) continue;
        const DeathPieceType &tp = death_piece_type(p.type_index);
        p.vel.z -= kGravity;
        p.pos.x += p.vel.x;
        p.pos.y += p.vel.y;
        p.pos.z += p.vel.z;
        p.heading += p.spin_a / 62.0f;
        p.pitch += p.spin_b / 62.0f;
        float ground = -1.0e9f;
        if (terrain != nullptr && terrain->valid())
            ground = terrain::height_field_height_world_bilinear(*terrain, p.pos.x,
                                                                 -p.pos.y);
        ground += 1024.0f / 65536.0f; // [orig: the +1024 rest offset @ 0x492e0d]
        if (p.pos.z < water_height) {
            // Water: splash at the surface crossing, sink, free at ground
            // [orig: @ 0x492e1b-0x492ebe].
            if (p.pos.z - p.vel.z >= water_height) {
                const Vec3 at{p.pos.x, p.pos.y, water_height};
                if (tp.splash_fx != nullptr)
                    events.effects.push_back(
                            DestructionEffectEvent{tp.splash_fx, at, Vec3{}, 0, 0});
                if (tp.splash_snd != nullptr)
                    events.sounds.push_back(DestructionSoundEvent{tp.splash_snd, at});
            }
            if (p.pos.z <= ground) p.active = false;
            continue;
        }
        if (p.pos.z >= ground) continue; // still airborne
        // Ground contact.
        if (p.bounces_left > 0) {
            // [orig: @ 0x492ed6-0x492fb2 — spin halves, velocity x0.95, the
            // vertical negates through the type bounce factor, dust + the
            // speed-gated bounce sound.]
            --p.bounces_left;
            p.spin_a *= 0.5f;
            p.spin_b *= 0.5f;
            p.vel.x *= 0.95f;
            p.vel.y *= 0.95f;
            p.vel.z = -p.vel.z * tp.bounce;
            p.pos.z = ground;
            if (tp.bounce_fx != nullptr)
                events.effects.push_back(
                        DestructionEffectEvent{tp.bounce_fx, p.pos, Vec3{}, 0, 0});
            const float speed = vec_len(p.vel);
            if (speed > 20480.0f / 65536.0f && tp.bounce_snd != nullptr)
                events.sounds.push_back(DestructionSoundEvent{tp.bounce_snd, p.pos});
        } else {
            // Exhausted [orig: @ 0x492edb-0x493048]: final effect/sound, then
            // persist as ground debris (flags bit 0) or free.
            p.pos.z = ground;
            if (tp.final_fx != nullptr)
                events.effects.push_back(
                        DestructionEffectEvent{tp.final_fx, p.pos, Vec3{}, 0, 0});
            if (tp.final_snd != nullptr)
                events.sounds.push_back(DestructionSoundEvent{tp.final_snd, p.pos});
            if ((p.flags & 0x1u) != 0) {
                p.settled = true; // stays visible where it landed
                p.vel = Vec3{};
            } else {
                p.active = false; // [orig: the memset free @ 0x493029]
            }
        }
    }
}

void DeathPieceSim::reset() noexcept {
    for (DeathPiece &p : pieces) p = DeathPiece{};
    cursor = 0;
}

} // namespace opennova::world
