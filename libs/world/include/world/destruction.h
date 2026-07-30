// Item destruction: explosion AoE damage, the destructible death chain (husk
// swap + death tick), the kz death blasts, and the death-piece pool — the
// witnessed path from "health reaches 0" to "the husk stands, pieces fly, the
// explosion goes off". Witness record: docs/world/world-wac-ai-re.md §24
// (grilled 2026-07-17 against Jointops.exe).
//
// [orig anchors: WeaponEffect_QueueExplosion @ 0x4e8330 (the 64x52-B authority
// queue @ 0xB7C688), Projectile_ProcessExplosionQueue @ 0x4ead80 (the O/M/D
// pool sweeps), Entity_ApplyWeaponDamage @ 0x4e6820 (the AoE applicator),
// Entity_HandleDestructibleDeathEvent @ 0x440210 / Entity_ProcessDestructibleDeath
// @ 0x43fbc0 (the item death), Entity_DispatchDeathCallback @ 0x493ef0 + the
// unitType table @ 0x815410, Entity_SpawnDeathPieces @ 0x493400 +
// Entity_SpawnSectionDebris @ 0x43f580 (the pieces), Entity_InitDeathSounds
// @ 0x4939b0 (death sound + effect families + the KZ blasts via
// Entity_QueueKzBlastAtUserPoints @ 0x4eabf0), Entity_ProcessDeathPiecePhysics
// @ 0x492dd0 + DeathPiece_TickAll @ 0x57b900 (piece physics), the debris-type
// table g_death_piece_types @ 0x8404f0.]
//
// The host presents; this module simulates and RECORDS. Every visual/audible
// leg lands in DestructionEvents for the present pass to drain (the RoundSim
// fired/impacts precedent) — libs/world stays render-free.
#ifndef OPENNOVA_WORLD_DESTRUCTION_H
#define OPENNOVA_WORLD_DESTRUCTION_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "world/entity.h"
#include "world/geom.h"

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;
class CollisionWorld;

// ----------------------------------------------------------------------------
// Per-item destruction traits, host-fed from items.def by the item-traits sweep
// (NovaSimulation::resolve_item_traits) — the def fields the death chain reads.
// ----------------------------------------------------------------------------
struct ItemDeathTraits {
    bool static_death = false;  // attrib2 & 0x100; generic death motion freezes
    int32_t unit_type = 0;      // def+0x196 — the death-dispatch row key
    float kz = 0.0f;            // def+0x198 — death-blast radius (units); 0 = none
    int32_t armor_impact = 0;   // def+0x190 word (-1 = invulnerable 0xFFFF)
    int32_t armor_blast = 0;    // def+0x192 word
    bool team_protect = false;  // attrib & 0x8000 — same-team blast immunity
    bool no_die = false;        // attrib & 0x40000000 — damage clamps to health-1
    bool has_husk = false;      // husk/huskfinal name authored in items.def
    // At least one authored husk/final model resolved to a live render object.
    // Building callbacks gate on this runtime state, not the authored name.
    // [orig: Entity_ProcessBuildingDeath @ 0x49442c: huskFinalModel||huskModel]
    bool husk_model_loaded = false;
    // The husk MODEL's section count — the death-piece loop bound. Retail
    // reads it off the husk RENDER object (renderObj[8]+52 @ 0x49361a), NOT
    // the items.def husk_sub_parts token (most defs author none) — the host
    // feeds it from the loaded husk model. 0 = unknown -> the authored count.
    // The piece model is huskFINAL first [orig: @ 0x4934af huskFinalModel ?:
    // huskModel], unlike the collision husk pick (@ 0x538720 husk first).
    int32_t husk_section_count = 0;
    // Per-section centers of the piece model (model-local, mission axes) —
    // baked into the piece spawn position through the complete authored pose
    // Rz(90-yaw) * Ry(-pitch) * Rx(roll)
    // [orig: the section-row center @ 0x4938bf-0x493900].
    std::vector<Vec3> husk_section_centers;
    // Section 0's z extents (units) — the dead-wreck ground rest offset
    // [orig: ground -= |sec0 z min| upright / += |sec0 z max| inverted
    // @ 0x461e23-0x461e4b / @ 0x494034-0x49405e].
    float husk_rest_min_z = 0.0f;
    float husk_rest_max_z = 0.0f;
    bool is_decoration = false; // def type 2 (+0x5C) — no death glow light
                                // [orig: @ 0x4934ee], and the death kick DROPS
                                // instead of popping [orig: @ 0x493969]
    uint8_t husk_sub_part_count = 0;      // def+0x100
    // def+0x101[] — debris-type table indexes. Retail's array holds 23 bytes
    // and the piece loop clamps its index at 16 [orig: @ 0x49362f]; slot 16 is
    // the reachable clamp target (unauthored slots read 0 = WHEEL).
    uint8_t husk_sub_part_types[17] = {};
    float debris_scale = 0.0f;  // def+0x1BC (0 -> pieces render at 1.0)
    std::string sound_death;    // def soundDeath name ('sounddeath')
    std::string particledeath;      // +0x416 name — the Dead-bone family (above water)
    std::string particleh2odeath;   // +0x44A name — the submerged family
    std::string particlefire;       // +0x47E name — the Fire-bone family
    std::string particleother;      // +0x4B2 name — the Other-bone family
    // Husk-model "KZ" user points (model-local, mission axes) — each queues a
    // kz_OrganicBlast r=5.0 at its full-Euler world pose; empty -> one blast
    // at the entity position
    // with r = kz ?: bound radius. [orig: Entity_QueueKzBlastAtUserPoints @ 0x4eabf0]
    std::vector<Vec3> kz_points;
};

struct ItemDeathTraitsTable {
    // Keyed by Entity::item_id (items.def type id). Small missions: linear is fine.
    std::vector<std::pair<int32_t, ItemDeathTraits>> rows;

    const ItemDeathTraits *get(int32_t item_id) const {
        for (const auto &r : rows)
            if (r.first == item_id) return &r.second;
        return nullptr;
    }
    void set(int32_t item_id, ItemDeathTraits t) {
        for (auto &r : rows)
            if (r.first == item_id) { r.second = std::move(t); return; }
        rows.emplace_back(item_id, std::move(t));
    }
    ItemDeathTraits *get_mutable(int32_t item_id) {
        for (auto &r : rows)
            if (r.first == item_id) return &r.second;
        return nullptr;
    }
    void clear() { rows.clear(); }
};

// ----------------------------------------------------------------------------
// The engine debris-type table [orig: g_death_piece_types @ 0x8404f0 — 13 named
// 80-B rows; sub_57B350/DeathPieceType_FindByName index it by the items.def
// husk_sub_part_types byte]. Effect/sound slot pointers resolved to their
// interning-table names (the {name, slot} pair tables @ 0x849150 / @ 0x82F640).
// ----------------------------------------------------------------------------
struct DeathPieceType {
    const char *name;
    float vel_scale;      // +0x10 — launch velocity scale
    float launch_add;     // +0x14 — base-direction contribution
    float spin_min;       // +0x18 — spin floor, degrees per tick
    float spin_max;       // +0x1C — spin cap, degrees per tick (retail stores
                          // deg * 2^32/360 = BAM32/tick [orig: @ 0x57b940])
    float probability;    // +0x20 — spawn chance (>= 1.0 = always)
    int32_t lifetime;     // +0x24 — bounce-count range (piece rolls rand%life+1)
    float bounce;         // +0x28 — ground restitution on the vertical axis
    const char *trail_fx;     // +0x2C
    const char *bounce_fx;    // +0x34
    const char *bounce_snd;   // +0x38
    const char *splash_fx;    // +0x3C
    const char *splash_snd;   // +0x40
    const char *final_fx;     // +0x44
    const char *final_snd;    // +0x48
    uint32_t flags;       // +0x4C — bit0 = persist as ground debris, bit1 = cactus
};

inline constexpr int kDeathPieceTypeCount = 13;
const DeathPieceType &death_piece_type(int index); // clamped [orig: index > 16 -> 16]

// ----------------------------------------------------------------------------
// The explosion queue. [orig: 64 x 52-B entries @ 0xB7C688, count @ 0xB7C680;
// writer WeaponEffect_QueueExplosion @ 0x4e8330 (authority-only), drained once
// per tick by Projectile_ProcessExplosionQueue @ 0x4ead80 which resets the
// count.] Entry field map: +0 pos xyz, +12 direction BAM, +24 type (the ammo
// kztype word +44), +28 ammoDef, +32 owner entity, +40 hit word, +44 radius
// override float (0 = the ammo's kz_maxradius).
// ----------------------------------------------------------------------------
struct ExplosionEntry {
    Vec3 pos;
    int32_t dir_bam = 0;
    int32_t type = 0;          // ammo kztype: 1 knife/ram, 2 standard, 3 medic,
                               // 4 radius-blast (direct hit), 5 c4, 6 bullets, 7 slash
    int32_t ammo_index = -1;   // into World::ammo
    EntityHandle owner;        // kill credit / cone origin
    uint16_t hit_word = 0;     // copied to victim+0x1BA
    float radius_override = 0.0f;
};

// ----------------------------------------------------------------------------
// Host-presentation events (drained per tick by the destruction present pass).
// ----------------------------------------------------------------------------
struct DestructionEffectEvent {
    std::string effect;        // .ptl effect name ('' = none)
    Vec3 pos;
    Vec3 dir;                  // zero = unoriented
    uint16_t attach_net_id = 0; // nonzero = bone/entity-attached family (the
                               // present pass keys the emitter to the entity)
    int32_t attach_bms_id = 0; // the placed-node resolve key for the anchor
    uint8_t family = 0;        // 0 transient, 1 death-family slot, 2 fire-family
                               // slot, 3 other-family slot (the 4-slot banks)
    uint16_t attach_wire_handle = EntityHandle::kInvalid; // packed pool/slot
                                                          // identity for
                                                          // runtime-only owners
    uint32_t attach_spawn_origin = 0; // distinguishes authored zero-BMS origins
                                      // from the synthetic promotion sentinel
};

struct DestructionSoundEvent {
    std::string sound;         // sound/set name ('' = none)
    Vec3 pos;
};

// One husked entity — the present pass swaps its render model to the husk and
// (with the collision feed) its ray/contact model. Emitted once per death.
struct HuskSwapEvent {
    uint16_t net_id = 0;
    uint16_t wire_handle = EntityHandle::kInvalid; // packed pool/slot identity;
                                                  // unlike net/BMS/origin this
                                                  // is distinct for synthetic
                                                  // attachment siblings
    int32_t bms_id = 0;
    uint32_t spawn_origin = 0;
    int32_t item_id = 0;
    uint32_t spawned_piece_mask = 0; // sections that left as pieces [entity+0x138]
    Vec3 pos;                        // the wreck position (batched statics resolve
                                     // no node — the present pass grafts here)
};

// A section-debris burst [orig: Entity_SpawnSectionDebris @ 0x43f580] — the
// per-triangle sampling runs in the PRESENT pass (it needs the render model's
// collision faces); the world emits the witnessed inputs.
struct SectionDebrisEvent {
    uint16_t net_id = 0;
    int32_t bms_id = 0;
    uint32_t spawn_origin = 0;
    int32_t item_id = 0;
    Vec3 pos;                  // the dying entity position (node-less fallback)
    Vec3 blast_center;         // entity+0x80 (zero = radial fallback pitch 63.3°)
};

// Window shatter on a building in blast range [orig: the GLASS1..GLASS4
// user-point effect spawns @ 0x4eb814-0x4eb85d]. The present pass resolves the
// model user points and spawns the glass effects within range.
struct GlassBreakEvent {
    uint16_t net_id = 0;
    int32_t bms_id = 0;
    uint32_t spawn_origin = 0;
    int32_t item_id = 0;
    Vec3 blast_pos;
    float radius = 0.0f;
};

struct DestructionEvents {
    std::vector<DestructionEffectEvent> effects;
    std::vector<DestructionSoundEvent> sounds;
    std::vector<HuskSwapEvent> husk_swaps;
    std::vector<SectionDebrisEvent> debris_bursts;
    std::vector<GlassBreakEvent> glass_breaks;
    // Diagnostic counters (probes assert the legs actually ran).
    int32_t explosions_processed = 0;
    int32_t items_destroyed = 0;

    void clear() {
        effects.clear();
        sounds.clear();
        husk_swaps.clear();
        debris_bursts.clear();
        glass_breaks.clear();
    }
};

// World-local stand-in for the destruction paths' witnessed rol-xor PRNG
// streams. Keeping the state on World makes independent simulations and
// mission restores deterministic instead of coupling them through process
// global state.
struct DestructionRng {
    static constexpr uint32_t kInitialState = 0x01234567u;
    uint32_t state = kInitialState;

    uint16_t next16() noexcept {
        auto rol32 = [](uint32_t value, int bits) {
            return (value << bits) | (value >> (32 - bits));
        };
        const uint32_t value = rol32(state + rol32(state, 11), 4);
        state = value ^ 1u;
        return static_cast<uint16_t>(state);
    }

    void reset() noexcept { state = kInitialState; }
};

// ----------------------------------------------------------------------------
// The death-piece pool. [orig: g_death_piece_pool @ 0x26BAC58 — 256 x 180-B ring
// (DeathPiece_AllocSlot @ 0x57b4f0), ticked by DeathPiece_TickAll @ 0x57b900 ->
// Entity_ProcessDeathPiecePhysics @ 0x492dd0.] A piece renders ONLY its own
// husk-model section (the render mask excludes every other section).
// ----------------------------------------------------------------------------
struct DeathPiece {
    bool active = false;
    // Reimplementation-only allocation incarnation. The retail pool embeds its
    // effect handle in the slot; our polled present pass needs (slot,generation)
    // to distinguish a blind ring overwrite from the same live piece.
    uint64_t generation = 0;
    int32_t item_id = 0;       // husk model source (the present pass resolves it)
    uint8_t section = 0;       // the ONE section this piece renders [piece+116]
    uint8_t type_index = 0;    // debris-type table row
    float render_scale = 1.0f; // def debrisScale ?: 1.0 [piece+136]
    Vec3 pos;                  // [piece+4..12]
    Vec3 vel;                  // units/tick [piece+28..36]
    float spin_a = 0.0f;       // [piece+40] spin rates, degrees per tick
    float spin_b = 0.0f;       // [piece+44] (max*(rand%100)/100 clamped >= min
                               // [orig: sub @ 0x57b940])
    float heading = 0.0f;      // integrated orientation (degrees; += spin/tick
    float pitch = 0.0f;        //  [orig: Yaw/Pitch += spin @ 0x492db9/0x492dc2])
    int32_t bounces_left = 0;  // [piece+117] — decremented per ground contact
    uint32_t flags = 0;        // debris-type flags byte [piece+119]
    bool settled = false;      // exhausted with flags bit0: persistent ground debris
};

class DeathPieceSim {
public:
    static constexpr int kCapacity = 256;
    std::array<DeathPiece, kCapacity> pieces{};
    int cursor = 0;            // ring cursor [orig: g_death_piece_pool_cursor]

    DeathPiece &alloc();       // [orig: DeathPiece_AllocSlot @ 0x57b4f0]
    // One 62 Hz step for every live piece [orig: Entity_ProcessDeathPiecePhysics
    // @ 0x492dd0]: gravity, ground bounce (restitution + spin halving + bounce
    // fx/sound), water splash + sink, final fx/sound + persist-or-free.
    void tick(World &world, const terrain::TerrainHeightField *terrain,
              float water_height, DestructionEvents &events);
    void reset() noexcept;
};

// ----------------------------------------------------------------------------
// The explosion sim: queue + per-tick drain.
// ----------------------------------------------------------------------------
class ExplosionSim {
public:
    static constexpr int kCapacity = 64;
    std::vector<ExplosionEntry> queue;

    // Authority-only push [orig: WeaponEffect_QueueExplosion @ 0x4e8330 —
    // non-authority calls and a full queue drop silently].
    void queue_explosion(World &world, const ExplosionEntry &e);

    // Drain the queue against the entity pools [orig:
    // Projectile_ProcessExplosionQueue @ 0x4ead80]. Kill-zone types 2/5/6/7
    // route to the weapon-damage applicator; 1 (vehicle ram) and 3 (medic) are
    // cited stubs at this altitude.
    void process(World &world, CollisionWorld *collision,
                 const terrain::TerrainHeightField *terrain, float water_height,
                 DestructionEvents &events);

    void reset() noexcept { queue.clear(); }
};

// ----------------------------------------------------------------------------
// The item death chain.
// ----------------------------------------------------------------------------

// Damage notify for a destructible item — the deathCallback equivalence
// [orig: Entity_HandleDestructibleDeathEvent @ 0x440210]: on the authority,
// health <= 0 and not yet husked -> process the destruction. Phase mirrors the
// witnessed callback param (1 bullet hit, 2 explosion hit, 4 net kill).
void destruction_notify_item_damage(World &world, Entity &target, int phase);

// The destruction itself [orig: Entity_ProcessDestructibleDeath @ 0x43fbc0 +
// the Entity_InitDeathSounds presentation leg]: Flags |= 6 (dead + husk swap),
// death tick, section-debris burst, scar clear (no decal system — tracked),
// death sound + particledeath family + the kz KZ-point blasts.
void process_destructible_death(World &world, Entity &target);

// The unitType death dispatch for vehicle/AI deaths — replaces the D-AI-9
// husk/pieces/sounds stubs. Runs the witnessed Entity_UpdateDeathTransforms
// order: pose snapshot, dispatch-by-unitType (pieces + flags |= 6), death
// sounds/effects. `silent` = the phase bit-0 variant (no death sound).
// [orig: Entity_UpdateDeathTransforms @ 0x494660 -> Entity_DispatchDeathCallback
// @ 0x493ef0 (table @ 0x815410) -> Entity_InitDeathSounds @ 0x4939b0]
void entity_update_death_transforms(World &world, Entity &target, bool silent);

// Death pieces for one entity [orig: Entity_SpawnDeathPieces @ 0x493400]:
// per husk section 1..N roll the debris-type row, spawn into the pool, record
// the spawned-section mask on the entity. Returns the mask.
uint32_t spawn_death_pieces(World &world, Entity &target);

// Per-tick settle for entities with an installed death-motion callback [orig:
// Entity_UpdateStaticDeathPhysics @ 0x494230 (buildings) /
// Entity_UpdateFallingDeathPhysics @ 0x493f70 (vehicles, incl. the landing kz
// blast)]. This includes AI-capable pool-1 entities after death dispatch.
void destruction_tick_dead_items(World &world,
                                 const terrain::TerrainHeightField *terrain,
                                 float water_height, DestructionEvents &events);

// Bullet-vs-item damage gates, shared by RoundSim's item leg [orig:
// Projectile_ProcessDamageOnTarget @ 0x4e7fb0]: indestructible flag, armor
// class (ammo penetration_impact vs def impact armor; -1 = invulnerable),
// NoDie clamp. Returns the damage to apply (0 = fully gated).
int32_t item_bullet_damage_gate(const World &world, const Entity &target,
                                int32_t damage, int32_t penetration_impact);

// The entity Flags bit constants (kEntityFlagDead/Husk/Indestructible and the
// rest) live in world/entity.h — the one home beside the field they describe.

} // namespace opennova::world

#endif // OPENNOVA_WORLD_DESTRUCTION_H
