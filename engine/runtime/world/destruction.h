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
// table g_DeathPieceTypes @ 0x8404f0.]
//
// The host presents; this module simulates and RECORDS. Every visual/audible
// leg lands in DestructionEvents for the present pass to drain (the RoundSim
// fired/impacts precedent) — engine/runtime/world stays render-free.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <runtime/world/entity.h>
#include <runtime/world/geom.h>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;
class CollisionWorld;

// ----------------------------------------------------------------------------
// Per-item destruction traits, host-fed from items.def by the item-traits sweep
// (Simulation::resolve_item_traits) — the def fields the death chain reads.
// ----------------------------------------------------------------------------
struct GlassPointTrait {
    Vec3 local_pos;
    Vec3 local_dir;
};

struct DeathEffectPoint {
	Vec3 local_pos;
	Vec3 local_dir;
};
struct DeathEffectBank {
	uint16_t mask = 0;
	std::vector<DeathEffectPoint> points; // first-16 mask, full walk with x86 shift wrapping
};

// The items.def ai_function class row that owns an item's event/death
// callback. Retail resolves the tag by whole-string stricmp against
// g_EntityClassEventCallbackTable @0x813000 (41 x 24-B rows: name[8], the
// event callback, three more), stores the callback in def+0x138 and copies it
// to entity+0x1C8 at spawn; the damage sites invoke it as cb(entity, 1|2, 0),
// the S2C 0x13 client kill as cb(entity, 4, 0), and the pool think walks as
// cb(entity, 0, 0) whenever the entity's +0x2AC countdown expires. An empty
// tag and a tag without a row resolve the "null" row.
// [orig: Entity_LookupRenderCallbacks @0x407dc0 — stricmp @0x407de2, the
//  row-0 miss default @0x407dee, def+0x138 @0x407e2d; EntityDef_InitAllCallbacks
//  @0x4a5aa9 ("Null" for an empty tag); the notify sites @0x4e6f93 / @0x42ebf5;
//  the pool-1 think gate @0x4b8e1b, the pool-2 cohort gate @0x4c2291]
enum class ItemDeathClass : uint8_t {
	// Rows built without a def (hand-built tests) and the class rows whose
	// event callback is dispatched by another system (the throwable rows
	// and the organic/vehicle rows that never
	// reach this notify): the pre-dispatch body, i.e. the tree callback.
	kUnwitnessed = 0,
	kNull, // "null" @0x813000 (and psec @0x813288, pwrp @0x813360, the
	       // callback-less nade @0x813138, an empty/unknown tag) -> 0x406FF0:
	       // +0x2AC = 0x1000000 and nothing else — the item never dies.
	       // The missile rows rokt/stng/hlfr/jvln @0x8132B8..0x813300 ->
	       // sub_443630 @0x443630 and arty @0x813318 -> sub_443640 @0x443640
	       // are byte-identical to that body (IDA 2026-09-14).
	kGnrc, // "gnrc" @0x8130C0 -> 0x407020
	kGnrl, // "gnrl" @0x8130D8 -> 0x407F80
	kGnl2, // "gnl2" @0x8130F0 -> Entity_HandleDeathEvent @0x4070F0
	kTree, // "tree" @0x813258 -> Entity_HandleDestructibleDeathEvent @0x440210
	kEwep, // "ewep" @0x813090 -> Entity_UpdateChildAttachment @0x4409A0
    kBarrel, // brrl @0x407CC0
    kBuilding, // bldg @0x43EE60
    kElevator, // ele0 @0x4A20D0
    kDoor, // door @0x43F370
    kTarget, // target @0x43F880
    kEnvironmentSound, // envs @0x408290
    kFlag, // flag @0x408430
    kCollapsingBuilding, // bld2 @0x43EEE0
    kEmitter, // emit @0x43F8F0
    kCrane, // cran @0x43FC70
    kPalm, // palm @0x53C4C0
    kTower, // towr @0x4406A0
    kSquib, // squib @0x449810
    // The flare rows aflr @0x813330 -> nullsub_65 @0x443650 and gflr
    // @0x813348 -> nullsub_66 @0x443660: a bare `retn` — no +0x2AC write, no
    // body at all. Never the tree body. (IDA 2026-09-14)
    kNone,
};

// The class row for an items.def ai_function tag — the retail table walk
// (whole-string, case-insensitive; a miss and the empty tag are the null row).
ItemDeathClass item_death_class_from_tag(const char *ai_function);

// The render facts of a death piece's model, host-fed from the loaded .3di
// (the ItemDeathTraits carrier precedent): the per-LOD RLOD thresholds and
// section counts the piece draw walks, the COBJ section centres every piece
// pivots on, and the model's GHDR bound radius.
struct DeathPieceModel {
    // Level i's RLOD pixel threshold in Q16.16 (model+0x40+4*i,
    // renderer::rlod_threshold_q16_from_rmdl); one entry per LOD, so the size
    // is the model's LOD count (model+0x10).
    std::vector<int32_t> lod_threshold_q16;
    // Level i's section count (the LOD mesh's +0x34, its render-object
    // count): the bone-matrix count of that level's draw.
    std::vector<int32_t> lod_section_count;
    // Section i's centre: the 108-B runtime COBJ row's +0x38..+0x40 (the
    // model+0xB0 collision block's +0x6C array, Q16.16 model axes) — the
    // spawn offset and the draw pivot [orig: Entity_SpawnDeathPieces
    // @ 0x4938b2..0x4938cc; DeathPiece_RenderSection
    // @ 0x57b6f6..0x57b70b].
    std::vector<std::array<int32_t, 3>> section_origin_q16;
    // The model's bound radius (model+0x14, GHDR's Q16.16 max radius): the
    // death flash spawns at twice it and every piece projects with it
    // [orig: Entity_SpawnDeathPieces @ 0x4934f4..0x493506 and
    // @ 0x4936a7..0x4936ae (piece+0x84)].
    int32_t radius_q16 = 0;

    bool loaded() const { return !lod_threshold_q16.empty(); }
};

struct RegionalItemSound {
    std::string name; // empty when the loaded banks do not resolve this slot
    int32_t base_ticks = 0;
    int32_t range_ticks = 0;
};

struct ItemDeathTraits {
    // The event/death callback row (item_death_class_from_tag on the def's
    // ai_function) — destruction_notify_item_damage dispatches on it.
    ItemDeathClass death_class = ItemDeathClass::kUnwitnessed;
    // Intact MODEL/CMDL carriers used by building class callbacks.
    bool model_loaded = false;
    bool model_bounds_loaded = false;
    std::vector<std::array<int32_t, 3>> model_section_origins_q16; // COBJ+56/60/64
    std::vector<int32_t> model_section_heights_q16; // COBJ+88 minus +84
    // Primary husk COBJ (entity+0x34: the section clone never carries +0x38,
    // Entity_SpawnSectionEntity @0x4402D0 / @0x492B46).
    std::vector<std::array<int32_t, 3>> husk_section_origins_q16;
    std::vector<std::array<int32_t, 3>> model_pivots_q16; // CMDL+116 / CXLT
    int32_t model_radius_q16 = 0; // GPM+20 / GHDR+24
    int32_t model_section0_min_z_q16 = 0;
    int32_t model_section0_max_z_q16 = 0;
    int32_t destroy_timing_ticks[3] = {};
    int32_t physics = 0;
    int32_t squib_distance_q16 = 0;
    std::string squib_ammo;
    bool primary_husk_loaded = false;
    int32_t model_radius_xy_q16 = 0;
    int32_t model_radius_z_q16 = 0;
    int32_t model_min_q16[3] = {};
    int32_t model_max_q16[3] = {};
    std::string graphic_name;
    std::string particlefx;
    bool has_particlefx_point = false;
    int32_t particlefx_point_q16[3] = {};
    int32_t particlefx_direction_q16[3] = {};
    std::array<RegionalItemSound, 4> regional_sounds;
    std::array<std::string, 4> regional_loops;
    bool has_sound_point = false;
    Vec3 sound_point; // intact-model SOUND userpoint, mission-local axes
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
    // The piece model (the LOADED huskFinal model, else the husk model) as the
    // piece spawn and the piece draw read it [orig: Entity_SpawnDeathPieces
    // @ 0x4934af..0x4934c3 huskFinalModel ?: huskModel]. Empty = no model.
    DeathPieceModel piece_model;
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
	std::string sound_profile; // SndProf.def impact slots for falling wrecks
	std::string sound_death; // def soundDeath name ('sounddeath')
	std::string particlespawn;     // +0x506 name [orig: spawn reset @0x4B9767]
	std::string particledeath;      // +0x416 name — the Dead-bone family (above water)
    std::string particleh2odeath;   // +0x44A name — the submerged family
    std::string particlefire;       // +0x47E name — the Fire-bone family
	// Final husk preferred, then the first husk. Dead is shared by water/air.
	// [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @0x522EE0]
	std::array<DeathEffectBank, 3> effect_banks;
	std::string particleother; // +0x4B2 name — the Other-bone family
	// +0x4E4 name — the ground-impact effect the settle transition plays once
    // at the entity position (interned to the +0x4E2 handle at mission start)
    // [orig: Entity_TransitionToGroundDeath @ 0x493080 read @ 0x493088].
    std::string particlefinale;
    // Husk-model "KZ" user points (model-local, mission axes) — each queues a
    // kz_OrganicBlast r=5.0 at its full-Euler world pose; empty -> one blast
    // at the entity position
    // with r = kz ?: bound radius. [orig: Entity_QueueKzBlastAtUserPoints @ 0x4eabf0]
    std::vector<Vec3> kz_points;
    // Bridge husk "DEAD" user points (model-local, mission axes). UnitType 11
    // emits one un-attached Effect_ShockWaterBrdg at each transformed x/y and
    // the raw mission water plane; an empty bank has no origin fallback.
    // [orig: Entity_SpawnDeathEffectsAtBones @0x4944c0]
    std::vector<Vec3> bridge_dead_points;
    // Exact intact-model window userpoint selected by retail's static
    // model/surface table. Positions and directions use mission-local axes;
    // the explosion sweep applies the entity's complete authored pose.
    // [orig: Terrain_SpawnEffectsAtUserPoint @0x5cee20]
    std::vector<GlassPointTrait> glass_points;
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
// The engine debris-type table [orig: g_DeathPieceTypes @ 0x8404f0 — 13 named
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
	bool release = false; // stop the attached family without spawning a replacement
	uint8_t bank_slot = 0;
	Vec3 attach_local_pos; // model-local mission axes; presentation composes the live pose
	// The descriptor's owner tag: set only by the producers whose retail
	// submit stores the spawning entity at descriptor +0x0C, so the presented
	// group takes the building-section gate (particle::EffectSectionGate);
	// every other row spawns with tag 0 [orig: CEffectWorld_SpawnEmitterAtPosition
	// @ 0x5F6DF0, the tag store @ 0x5F6EFB].
	bool section_tagged = false;
	// A slot-held group that stays where it spawned: the slot keeps only its
	// handle for the next release (spawn_victim_hit_emitter).
	bool positioned = false;
};

struct DestructionEvents;
void spawn_death_effect_banks(
		Entity &entity, const ItemDeathTraits &traits, bool underwater, DestructionEvents &events);
void update_dead_wreck_effects(World &world, Entity &entity, const ItemDeathTraits *traits,
		float water_height, DestructionEvents &events);
void release_death_effect_bank(Entity &entity, uint8_t family, DestructionEvents &events);
// A blast victim's hit emitter: the ammo's secondary effect replaces whatever
// the victim's +0x1CC emitter (the death family's slot 0) holds.
void spawn_victim_hit_emitter(Entity &victim, const std::string &effect,
		DestructionEvents &events);

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
	bool restore_intact = false; // respawn reinstalls the intact render model
};

// The death explosion flash for the presenter's light pool
// [orig: Entity_SpawnDeathPieces @ 0x49351a — LightPool_SpawnGlowEffect at the
// entity position, radius = 2x the piece model's bound radius, color 0xFFC080,
// mode 2 / 31 ticks, corona disabled].
struct DeathLightEvent {
    Vec3 pos;
    float radius = 0.0f;
};

struct DestructionEvents {
    std::vector<DestructionEffectEvent> effects;
    std::vector<DestructionSoundEvent> sounds;
    std::vector<HuskSwapEvent> husk_swaps;
    std::vector<DeathLightEvent> death_lights;
    // Diagnostic counters (probes assert the legs actually ran).
    int32_t explosions_processed = 0;
    int32_t items_destroyed = 0;
    int32_t crackles = 0; // wreck-fire crackle rolls that fired (S12b)
    int32_t debris_triangles = 0; // resolved CFAC samples in this drain
    int32_t glass_points = 0;     // newly broken exact userpoints in this drain

    void clear() {
        effects.clear();
        sounds.clear();
        husk_swaps.clear();
        death_lights.clear();
        debris_triangles = 0;
        glass_points = 0;
    }
};

inline constexpr const char *kSectionDebrisFoliageEffect =
        "Effect_TreeFoliageExp"; // [orig: g_FxTreeFoliageExp @0x2C25BF0]
inline constexpr const char *kSectionDebrisWoodEffect =
        "Effect_TreeWoodExp"; // [orig: g_FxTreeWoodExp @0x2C25BF4]

inline constexpr const char *kGlassShatterEffects[4] = {
        "Effect_BldGlassExp", "Effect_BldPaperExp",
        "Effect_BldFireExp", "Effect_BldDustExp"};
// The glass userpoint path rolls each effect's ALT probability column from the
// surface-effect table (40 B rows {name[32], prob_main f32, prob_alt f32}) and
// spawns on rand16 % 100 <= ftol(prob_alt * 100.0f). Glass's alt cell holds
// 0x3EA8F5C3 (exactly 0.33f); the product 33.0000013 truncates to 33 in every
// intermediate precision, so the <= admits rolls 0..33 — 34 of 100.
// [orig: Effect_RollSurfaceEffectProbability @ 0x5CC1F0 scale flt_7C4654=100.0
//  @ 0x5CC25A; g_SurfaceEffectProbTable @ 0x8418B8; useAltProbability=1 pushes
//  @ 0x5CF0B3/0x5CF0F4 in Terrain_SpawnEffectsAtUserPoint]
inline constexpr float kGlassShatterAltProbability[4] = {
        0.33f, 0.05f, 0.05f, 0.1f};
static_assert(static_cast<int>(kGlassShatterAltProbability[0] * 100.0f) == 33,
              "glass gate boundary: ftol(0.33f * 100) is 33, admitting 34 rolls");

// The wreck-fire random crackle, rolled per tick per burning wreck on the
// world's rol-xor PRNG stand-in stream (the same generator as retail's
// PRNG_Next16_C @ 0x6131b0 — our one DestructionRng stream stands in for the
// A/B/C instances, the documented fold). One roll per wreck at the entity
// position collapses retail's per-fire-bone rolls at the bone positions —
// the 4-slot bone banks remain the open D-ITEM-15 residual. The crackle
// SOUND is distance-delay gated at the ENTITY position; the effect spawns
// transient.
// [orig: Entity_UpdateDeadWreckEffects @ 0x493140 (renamed ex
//  Entity_UpdateMuzzleFlashAndEffects 2026-08-07) — per fire bone i:
//  (fire mask & (1<<i)) && PRNG_Next16_C() < 16 && bone Z >= water ->
//  submit g_FxBoatExpSec @ 0x4932d1 +
//  Sound_PlayWithDistanceAttenuation(g_SndExploShipSmB, &entity->Position)
//  @ 0x4932e2]
inline constexpr const char *kFireCrackleEffect =
        "Effect_BoatExpSec"; // [orig: g_FxBoatExpSec @ 0x2C25CB8]
inline constexpr const char *kFireCrackleSound =
        "EXPLO_SHIP_SM"; // [orig: g_SndExploShipSmB @ 0x24E08F4]
inline constexpr uint16_t kFireCrackleThreshold = 16; // [orig: @ 0x4932bf]

// The debris-type trail-effect column, by DeathPiece::type_index
// [orig: g_DeathPieceTypes @ 0x8404f0 +0x2C; "" = the type authors no trail
// (NP rows); out-of-range indexes take no trail]. Every death-piece submit
// carries tag 0, the trail included, so it takes no section gate
// [orig: Entity_ProcessDeathPiecePhysics `xor edi, edi` @ 0x492FC2 ahead of
// the submit @ 0x493014; DeathPiece_UpdateProjectilePhysics `xor ebx, ebx` @ 0x57BA0F
// ahead of the submits @ 0x57BA2A..0x57BB8A; DeathPiece_PhysicsUpdate's
// submits @ 0x48F547 / 0x48F692 push 0].
const char *death_piece_trail_effect(uint8_t type_index);

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
// The death-piece pool. [orig: g_DeathPiecePool @ 0x26BAC58 — 256 x 180-B ring
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
    // The piece orientation, degrees: the wrecked entity's live heading
    // (entity+0x10, the BAM heading), pitch and roll at the spawn, then heading
    // and pitch spin per tick; roll keeps its spawn value
    // [orig: the pose copy @ 0x4936be..0x4936de; Yaw/Pitch += spin
    // @ 0x492db9/0x492dc2].
    float heading = 0.0f;      // [piece+16]
    float pitch = 0.0f;        // [piece+20]
    float roll = 0.0f;         // [piece+24]
    // Every piece-model section except this piece's own: the sections its
    // draw collapses [piece+124, orig: @ 0x493897..0x4938b0].
    uint32_t hidden_mask = 0;
    // The piece model's bound radius, the projection radius of its draw
    // [piece+132, orig: @ 0x4936a7..0x4936ae].
    int32_t radius_q16 = 0;
    int32_t bounces_left = 0;  // [piece+117] — decremented per ground contact
    // False for a silent death's piece: the type's trail effect is never
    // submitted [orig: Entity_SpawnDeathPieces @0x493811..0x49382a].
    bool trail = true;
    uint32_t flags = 0;        // debris-type flags byte [piece+119]
    bool settled = false;      // exhausted with flags bit0: persistent ground debris
};

class DeathPieceSim {
public:
    static constexpr int kCapacity = 256;
    std::array<DeathPiece, kCapacity> pieces{};
    int cursor = 0;            // ring cursor [orig: g_DeathPiecePoolCursor]

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
    // Projectile_ProcessExplosionQueue @ 0x4ead80]. Kill-zone types 2/4/5/6/7
    // route to the weapon-damage applicator, 1 (the knife) to the melee
    // applicator, 3 to the medic revive; a zero resolved radius drops the entry.
    void process(World &world, CollisionWorld *collision,
                 const terrain::TerrainHeightField *terrain, float water_height,
                 DestructionEvents &events);

    void reset() noexcept { queue.clear(); }
};

// ----------------------------------------------------------------------------
// The item death chain.
// ----------------------------------------------------------------------------

// Damage notify for a destructible item — the entity+0x1C8 event callback,
// dispatched on the def's ai_function class row (ItemDeathClass): gnrc
// @0x407020 (the two-step unitType piece death), gnrl @0x407F80 (husk + death
// sound + one effect), gnl2 @0x4070F0 (the delayed detonation), tree
// @0x440210 (the section-debris death), ewep @0x4409A0 (the gunner dismount +
// husk), null @0x406FF0 (never dies). Phase mirrors the witnessed callback
// param (1 bullet hit, 2 explosion hit, 4 net kill); the authority legs run
// under rules.logic_authority, a client acts on phase 4 only.
struct ItemExplosionEvent {
    uint16_t source = 0xFFFF;
    uint8_t count = 0;
    FixedVec3 position;
    int32_t heading = 0;
};
void spawn_item_explosion(World &world, const Entity *source, const FixedVec3 &position,
        int32_t heading, int count, bool broadcast = true);
struct EntityRemoveEvent { uint16_t handle = 0xFFFF; };
struct ItemStateEvent {
    uint16_t handle = 0xFFFF;
    int16_t section = 0;
};
// Class callbacks enqueue the section payload at their original send sites.
// [orig: Server_SendEntityStatePacket @ 0x509D70]
void emit_item_state(World &world, Entity &target, int32_t section);
void update_item_destroy_fade(World &world, Entity &entity);
void update_item_ambient_sound(World &world, const Entity &entity);
void squib_event(World &world, Entity &entity, int phase);
void tick_squib(World &world, Entity &entity);

struct ItemHitContext {
    int32_t section = 0; // hitRecord[14]
    int32_t damage = 0; // hitRecord[12], tower reads its low byte
    int32_t heading = 0, pitch = 0, roll = 0; // hitRecord[3..5]
    // The event callback's THIRD argument, cb(entity, phase, flags): bit 0 is
    // the silent death the gnrc client leg forwards to
    // Entity_UpdateDeathTransforms (no death sound, effect banks, kz blasts or
    // piece trails). Every caller passes 0 but the S2C 0x4E kill.
    // [orig: Entity_KillBySlotId @0x42BD6A; the gnrc leg @0x40703f..0x407045]
    int32_t event_flags = 0;
};
// The client's slot kill (the 0x26 route and the vehicle record's destroyed
// bit): the health clear and Dead guard, then the section into the hit record
// and the class callback with phase four, a brain row's state machine included.
// [orig: Entity_KillBySlotId @ 0x42BCE0]
// `flags` is the callback's third argument: 0 for the 0x26 kill and the
// vehicle record's bit, 1 for the S2C 0x4E join-window kill, whose bit 0 a
// vehicle def (ItemDef+0x5C type 1) clears [orig: @0x42BD5B..0x42BD5D].
void apply_item_state_event(World &world, Entity &target, int16_t section,
        int32_t flags = 0);

void destruction_notify_item_damage(World &world, Entity &target, int phase,
        ItemHitContext hit = {});

// The pool-2/3 cohort walks: cb(entity, 0, 0) at slot&7 with positive-clock
// -8 (pool 2), at slot&63 with -64 (pool 3), then the update callback. Both
// peers run class callbacks; each callback owns its authority gates. Pool 1 is
// World::update_pool1_slot. [orig: Entity_UpdateAllEntities @0x4C2100]
void tick_item_event_pool(World &world, int pool);

// The tree-class destruction [orig: Entity_ProcessDestructibleDeath @ 0x43fbc0 +
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
// Aircraft's immediate death initializer: blast, pieces, sounds, optional
// piece physics, random hull spin. [orig: Entity_InitDeathState @0x48F7C0]
void entity_init_aircraft_death(World &world, Entity &target, bool simulate);

// Death pieces for one entity [orig: Entity_SpawnDeathPieces @ 0x493400]:
// per husk section 1..N roll the debris-type row, spawn into the pool, record
// the spawned-section mask on the entity. Returns the mask.
// `silent` = the callback flags' bit 0: the pieces fly without their type's
// trail effect [orig: @0x493811 — the spawnEffect submit behind (frameFlags & 1)].
uint32_t spawn_death_pieces(World &world, Entity &target, bool silent = false);

// Shared generic falling callback, also called explicitly by vehicle states 21/23.
// [orig: Entity_ProcessFallingDeathPhysics @0x461D30]
void entity_process_falling_death(World &world, Entity &entity,
		const terrain::TerrainHeightField *terrain, float water_height);

// Per-tick settle for entities with an installed death-motion callback [orig:
// Entity_UpdateStaticDeathPhysics @ 0x494230 (buildings) /
// Entity_UpdateFallingDeathPhysics @ 0x493f70 (vehicles, incl. the landing kz
// blast)]. This includes AI-capable pool-1 entities after death dispatch.
// Returns true when a class-specific update owns this row.
bool tick_item_class_motion(World &world, Entity &entity,
        const terrain::TerrainHeightField *terrain);

void tick_item_death_motion(World &world, Entity &entity,
        const terrain::TerrainHeightField *terrain, float water_height, DestructionEvents &events);

// Bullet-vs-item damage gates, shared by RoundSim's item leg [orig:
// Projectile_ProcessDamageOnTarget @ 0x4e7fb0]: indestructible flag, armor
// class (ammo penetration_impact vs def impact armor; -1 = invulnerable),
// NoDie clamp. Returns the damage to apply (0 = fully gated).
int32_t item_bullet_damage_gate(const World &world, const Entity &target,
                                int32_t damage, int32_t penetration_impact);

// The entity Flags bit constants (kEntityFlagDead/Husk/Indestructible and the
// rest) live in world/entity.h — the one home beside the field they describe.

} // namespace opennova::world
