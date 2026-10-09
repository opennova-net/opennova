// The ammo.def table — the fired-round ballistics + damage knowledge the authoritative
// round sim consumes. POD and def-parser-free: ammo_table_build.h (this directory) builds it
// from a parsed DefAmmoFile; the engine feeds it beside the weapon table
// (Simulation::load_ammo_table). [orig: g_AmmoDefTable @ 0xA2ECE8 — 276-B records,
// loaded per mission from literally "ammo.def" by AmmoDef_LoadAll @ 0x40B0B0 (same
// encrypted-ASCII parse as weapon.def), token map AmmoDef_ParseProperty @ 0x40A2D0;
// docs/net/novaworld-net-re.md §5.60]
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/fixed.h>

namespace opennova::world {

// The canonical effects_table tag order — the array index IS the impact effect id the
// round-impact selection uses [orig: g_AmmoEffectTagTable @ 0x813420, 28 {name, id}
// pairs scanned from index 1 ('null' at 0 is never matchable, scan @ 0x40a46a);
// consumers: the ballistic impact handlers (terrain Projectile_HandleTerrainImpact
// @ 0x4e9210, entity @ 0x4e9390, person @ 0x4e98f0, water @ 0x4e9b80) and the
// Knife-only Weapon_RaycastAndSpawnImpact @ 0x4e8460 leaf — terrain surface + 4,
// entity/building material + 4 unconditionally on the bullet path
// (AmmoDef_ProcessImpactEffect @ 0x40a170 only clamps >= 28 to 4 @ 0x40a1bf);
// the material 1 -> 23 flesh remap exists ONLY on the knife's PERSON leg
// (case 3 @ 0x4e8880..0x4e8888), water = 11].
inline constexpr int kImpactEffectTagCount = 28;
inline const char *const kImpactEffectTagNames[kImpactEffectTagCount] = {
    "null", "move", "player", "zip", "obj", "dirt", "grass", "snow", "cement", "sand",
    "packeddirt", "water", "railroad", "mud", "ice", "quicksand", "stone", "wood",
    "metal", "glass", "cloth", "foliage", "hmetal", "flesh", "bodyarmor", "uwaterdeep",
    "uwatershallow", "uwatersurface",
};
// The tags in a modder's words, for the tools that name a bullet face's surface by its row (a face
// byte b plays row b + 4: the editor's surface picker, `opennova-3di catalog`'s surface lines, the
// Blender add-on through it). Tooling words, not the game's: the game reads the tags above.
inline const char *const kImpactEffectTagWords[kImpactEffectTagCount] = {
    "None", "Move", "Player", "Zip", "Object", "Dirt", "Grass", "Snow", "Cement", "Sand",
    "Packed dirt", "Water", "Railroad", "Mud", "Ice", "Quicksand", "Stone", "Wood",
    "Metal", "Glass", "Cloth", "Foliage", "Heavy metal", "Flesh", "Body armor", "Deep water",
    "Shallow water", "Water surface",
};

// The row a round plays where it strikes the terrain: the surface class the char map (or a placed
// tile) gives there, shifted into the tag table, a class past it the dirt row (no char map reads 1 ->
// 5 dirt; an unmapped sector 7 -> 11 water) [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510 result
// + 4; the terrain leg of the @ 0x4ea6a7 hit switch in Projectile_UpdatePhysics @ 0x4e9d70].
inline int terrain_impact_effect_tag(int32_t surface) {
    return (surface >= 0 && surface + 4 < kImpactEffectTagCount) ? surface + 4 : 5;
}
// The row a round plays where it crosses the water plane first (the water handler's)
// [orig: the water impact handler @ 0x4e9b80].
inline constexpr int kWaterImpactEffectTag = 11;

// Case-insensitive tag lookup, matching the original's scan from index 1
// [orig: @ 0x40a46a..0x40a48e]; -1 = unknown tag.
inline int impact_effect_tag_index(const char *name) {
    if (name == nullptr) return -1;
    for (int i = 1; i < kImpactEffectTagCount; ++i) {
        const char *t = kImpactEffectTagNames[i];
        size_t j = 0;
        while (t[j] != '\0' && name[j] != '\0' &&
               std::tolower(static_cast<unsigned char>(t[j])) ==
                       std::tolower(static_cast<unsigned char>(name[j])))
            ++j;
        if (t[j] == '\0' && name[j] == '\0') return i;
    }
    return -1;
}

// The energy a round pays to go on through an entity face of `material` (the
// face byte), in per-tick Q16 units; 0 for every other material, which the
// round sim reads as an absorbing face. The entity-face material table has its own
// energy law: the decompiler's "ClampKineticEnergy" name is misleading, the
// material cost is SUBTRACTED, and a round unable to pay it is released.
// [orig: table @0x82D034; Entity_ClampKineticEnergy @0x4E9070..0x4E9200]
inline int32_t material_energy_cost(uint8_t material) {
    switch (material) {
        case 19: case 15: return 10 * io::kFp16OneInt;
        case 16: case 7: return 4 * io::kFp16OneInt;
        case 17: return 8 * io::kFp16OneInt;
        default: return 0;
    }
}

// One baked per-tag impact row: the .ptl effect + soundset spawned on a hit of that
// class ('' = none / row absent). [orig: the staged row = {tag id, interned effect
// handle +4, soundset id +8, zeroed word +12} — 16 B stride, COMPACTED into the ammo
// record's table at block end by AmmoDef_InitEffectsTable @ 0x409f20 (16*(count+1)
// bytes, authored tags in ascending order). The impact presenter SEARCHES it by tag id
// (impact_effect_row below); the knife's ray and the squib's travel read it by tag
// POSITION (*(ammoDef+104) + 16*tag @ 0x4e88c3, @ 0x449569) — sound only because
// every shipped table authors a contiguous prefix of tags from 1 (row index == tag
// id). This 28-slot tag-addressed model serves both: the search is the `authored`
// flag, and for the two position readers it is byte-equivalent on shipped data and
// resolves correctly on sparse tables where the original would misindex — an
// intentional bounded divergence from an original indexing assumption.]
struct AmmoImpactEffectRow {
    std::string effect;
    std::string sound;
    // The tag was authored (a row exists in the compacted bank even when both
    // columns are 'none'): the readers that overwrite a seeded default with the
    // row's value [orig: AmmoDef_GetExplosionRadius @0x409770, `radius =
    // effectEntry[2]` on every tag-5 row @0x4097ab] see the row's zero, not the default.
    bool authored = false;
};

// A row of ammo def 0's static bank (AmmoTable::null_bank): the tag the compaction put
// at its place (0: the null slot, or a place past def 0's rows) and its two columns.
struct AmmoBankRow {
    int tag = 0;
    std::string effect;
    std::string sound;
};

// Kill-zone classes (record word +44) and the item-class damage exclusions the
// explosion sweep gates its pool walks on — engine/runtime/world stays def-parser-free,
// so the witnessed values live here beside their consumer. [orig: the kztype
// 8-name table @0x8133E0; the flag OR-bit table @0x813500 — NoOItems 0x80000
// (no organic/pool-0 damage), NoMItems 0x100000 (movable/pool-1), NoDItems
// 0x200000 (dumb-static/pool-2); gates @0x4eaece/@0x4eb378/@0x4eb5b8]
namespace ammo_kz {
enum : int32_t {
    kNull = 0,
    kKnife = 1,
    kStandard = 2,
    kMedic = 3,
    kRadiusBlast = 4,
    kC4 = 5,
    kBullets = 6,
    kSlash = 7,
};
}
inline constexpr uint32_t kAmmoFlagIgnore = 0x2u;
inline constexpr uint32_t kAmmoFlagDetonateSatchels = 0x20u;
inline constexpr uint32_t kAmmoFlagNoGravity = 0x100u;
inline constexpr uint32_t kAmmoFlagHasItem = 0x200u;
inline constexpr uint32_t kAmmoFlagInstantKillZone = 0x400u;
inline constexpr uint32_t kAmmoFlagUseOwnMove = 0x2000u;
inline constexpr uint32_t kAmmoFlagNoAge = 0x4000u;
inline constexpr uint32_t kAmmoFlagForceTracer = 0x8000u;
inline constexpr uint32_t kAmmoFlagShotgun = 0x10000u;
inline constexpr uint32_t kAmmoFlagClaymore = 0x20000u;
inline constexpr uint32_t kAmmoFlagNoOItems = 0x80000u;
inline constexpr uint32_t kAmmoFlagNoMItems = 0x100000u;
inline constexpr uint32_t kAmmoFlagNoDItems = 0x200000u;
// [orig: the flag OR-bit table off_813500 @0x813500 — the ClipWater entry @0x8135b0, the ClipWaterFx entry @0x8135b8]
inline constexpr uint32_t kAmmoFlagClipWater = 0x1000000u;
inline constexpr uint32_t kAmmoFlagDesignateTarget = 0x2000000u;
inline constexpr uint32_t kAmmoFlagIgnorFoilage = 0x4000000u; // sic — the witnessed token spelling
// The in-flight `move` emitter is RELEASED (not re-posed) while the round sits
// at or below the water plane [orig: the +0x114 & 0x20000000 test @0x4EA02B in
// Projectile_UpdatePhysics; world/round_move_effect.h].
inline constexpr uint32_t kAmmoFlagClipWaterFx = 0x20000000u;
// (parity static_asserts against DEF_AMMO_FLAG_* live in world/weapon_table_build.cpp)

struct AmmoTableEntry {
    std::string name;               // record +144 [orig: AmmoDef_AllocateSlot copy]
    uint32_t flags = 0;             // +0 `flag` OR-bits [orig: name/bit table @0x813500]
    int32_t velocity = 0;           // +4, units/s (integer)
    int32_t max_age_ticks = 0;      // +8, 62 Hz ticks [orig: AmmoDef_ParseSecondsToTicks (ex sub_40A0F0) = (62*fp16+0x8000)>>16]
    int32_t arm_age_ticks = 0;      // +12 — a hit before arming swaps in notarmmed_ammo
    float spread_error = 0.0f;      // +24 ballistic dispersion (16.16 -> float)
    int32_t spread_error_fp16 = 0;  // +24 exact source value; spread uses this carrier
                                    // [orig: AmmoDef_ParseProperty @0x40A2D0]
    uint8_t recoil[3] = {0, 0, 0};  // bytes +227..+229, prone/crouch/standing impulse
                                    // [orig: AmmoDef_ParseProperty @0x40A2D0]
    float drag = 0.0f;              // +28 (16.16 -> float)
    int32_t drag_fp16 = 0;          // +28 exact source value; flight uses this carrier
    float bullet_radius = 0.0f;     // +32 hit-test radius (16.16 -> float)
    int32_t bullet_radius_fp16 = 0; // +32 exact source value; collision uses this carrier
    int32_t spread_count = 0;       // +48 shotgun/claymore pellet count
    int32_t kztype = 0;             // word +44: kill-zone class 0..7 [orig: table @0x8133E0]
    int32_t kz_damage = 0;          // word +46
    int16_t heat_det_range = 0;     // +100, heat seeker range in world units
    int32_t boresight_maxang = 0;    // +88, heat seeker cone in BAM
    int32_t armor_density[3] = {}; // armor energy loss by shooter ammo class
    int32_t weight_in_grains = 0;   // +184 — the kinetic damage mass term [orig: @0x4ecb1a]
    int32_t min_stable_velocity = 0; // +176 speed-table index threshold
    int32_t tumble_error_fp16 = 0;  // +180; exact kick size, random frame still deferred
    int32_t min_damage = 0;         // +188 damage floor [orig: @0x4ecb3a]
    int32_t max_damage = 0;         // +192 damage cap when > 0 [orig: @0x4ecb42]
    int32_t penetration_impact = 0; // +196 — must reach the target itemDef+400 armor threshold
    int32_t penetration_kz = 0;     // +200 — must reach the target's blast armor (def+0x192)
    uint8_t secondary_anim = 0;     // +224 `secondary_anim`; collision-force selector
    uint8_t kz_physics = 0;         // +225 `kz_physics`; collision-force selector
    float kz_minradius = 0.0f;      // +52 (fp16 -> units) — linear-falloff start
    float kz_maxradius = 0.0f;      // +56 (fp16 -> units) — the blast radius when the
                                    // queue entry carries no float override
    int32_t kz_pieslice_bam = 0;    // +60 (deg -> BAM) — the cone's half-angle: 0
                                    // skips the gate, the allocator's unauthored
                                    // 0x7FFFFFFF passes every bearing
    int32_t tracer_rate = 0;        // byte +226 (`tracerRate`)
    std::string notarmmed_ammo;     // +241 — the not-armed child ammo name
    // The per-surface impact rows by canonical tag id. Bake rules per the witnessed
    // parser: 'none' columns stay empty, duplicate tags keep the FIRST row (the
    // original warns 'redefining the effect' @ 0x40a502), unknown tags are dropped
    // ('unknown effect tag' @ 0x40a49f), and the authored 4th (count) column is
    // DISCARDED — the original parses then zeroes it [orig: @ 0x40a587].
    AmmoImpactEffectRow impact_effects[kImpactEffectTagCount];
    // Host fire-presentation fields (world-wac-ai-re §17.4): the original stores the
    // RESOLVED sound-set pointer / interned effect handle [orig: AmmoDef_ParseProperty
    // @0x40a8c8/@0x40a8f6]; we carry the names and the host resolves at play time.
    std::string ai_launch_set;      // +64 `ai_launch` fire sound-set name
    std::string ai_launch_effect;   // +68 `ai_launcheffect` muzzle effect name
    // The blast's per-victim presentation, names as above (the original
    // resolves the handle / set pointer at parse) [orig: AmmoDef_ParseProperty
    // @0x40aa15 / @0x40a92a; consumer Projectile_ProcessExplosionQueue
    // @0x4EB1A3 / @0x4EB1DD].
    std::string secondary_effect;   // +72 `secondary_effect`
    std::string kz_sound;           // +76 `kz_sound`
    int32_t mf_light = 0;           // +36 `MF_Light` presence flag [orig: @0x40a81b]
    int32_t mf_light_value = 0;     // +40 `MF_Light` value
    int32_t tracer_type_friendly = 0; // +232 `tracer_type` first style id
    int32_t tracer_type_enemy = 0;    // +236 second style id (defaults to the first)
    // The tracer round's visible item models (`frndlyTrcrID`/`foeTrcrID`, ITEMS.DEF
    // type ids; the original resolves to item indexes at parse [orig: @0x40a5f8 ->
    // +16/+20], we resolve at use) and the in-flight glow (`light_move` [orig:
    // +120 radius / +124 RGB -> LightPool_SpawnGlowEffect @0x4ec8da, round+0x1B4]).
    int32_t tracer_item_friendly = 0; // +16 (raw type id; 0 = none)
    int32_t tracer_item_enemy = 0;    // +20 (raw type id; 0 = none)
    float light_move_radius = 0.0f;   // +120 (16.16 -> float units; 0 = no glow)
    uint32_t light_move_color = 0;    // +124 packed 0xRRGGBB
    // The impact flash light (`light_impact` [orig: +132 radius / +128 RGB /
    // +136 62 Hz ticks -> LightPool_SpawnGlowEffect @0x40a2b3, mode 2 fade]).
    float light_impact_radius = 0.0f; // +132 (16.16 -> float units; 0 = no light)
    uint32_t light_impact_color = 0;  // +128 packed 0xRRGGBB
    int32_t light_impact_ticks = 0;   // +136 fade duration in 62 Hz ticks
    // The guided-pursuit turn clamps in BAM/tick (`turnrate_maxpit`/`maxyaw`
    // deg/s, (192426 * fp16 + 0x8000) >> 16 [orig: AmmoDef_ParseTurnRate -> +0x50/+0x54];
    // unauthored is the allocator's -1, and the integrator takes any value <= 0
    // as its default).
    int32_t turnrate_maxpit = 0;      // +80
    int32_t turnrate_maxyaw = 0;      // +84
    // Permanent terrain-cache scorch selector (`scorch_id`, word +0x74).
    // The highest-quality terrain impact path resolves ids 1/2/7/8 through
    // world::TerrainScorchEvents before presenting the ordinary impact.
    int32_t scorch_id = 0;
    // The impact scar kind (`scar_type`): 0 = no mark, 1 = the ring scar,
    // 2 = glass-only [orig: word +0x76 -> Impact_SpawnGlassEffectsOrScar
    // @0x5cf1b0's kind argument; world/impact_scar.h].
    int32_t scar_type = 0;            // word +0x76
    // The tracer-whiz radius, 16.16: the `move` row's (tag 1) then the `zip`
    // row's (tag 3) sound set cull range << 16, capped at 50 units, the later
    // row winning [orig: +0x8C from set+72 by AmmoDef_InitEffectsTable
    // @0x40a04b..0x40a07f]. Resolved against the loaded banks by
    // resolve_ammo_whiz_radii (world/radar_contacts.h); 0 = no whiz.
    int32_t whiz_radius_q16 = 0;      // +0x8C
    bool valid = false;
};

// Dense, ammo.def file order (the file's own first block is the null AT_NULL entry, so
// index 0 is naturally the null ammo). [orig: two-pass count -> AmmoDef_AllocateBuffer ->
// sequential AmmoDef_AllocateSlot per `ammo <NAME>` block @0x40b0b0]
struct AmmoTable {
    std::vector<AmmoTableEntry> entries;

    // Ammo def 0's effect bank: the static 448-byte `word_A2EB28` its block end fills
    // instead of allocating, 28 rows of 16 bytes {tag id, effect, sound, 0}: row 0 the
    // always-copied null slot, then def 0's authored tags in ascending tag order, the
    // rest zero [orig: AmmoDef_InitEffectsTable @0x409F20 — `ammoDef == g_AmmoDefTable`
    // -> word_A2EB28 @0x409f62, the `*srcEffect || entryIndex <= 0` copy gate
    // @0x409fe8]. The bank is COMPACTED: row r is def 0's r-th authored tag, the tag r
    // itself only where def 0 authors every tag below it (JO:CA's AT_NULL authors
    // 1..23). Two readers take a row of it by PLACE: the impact presenter for a tag the
    // struck ammo authors no row of (impact_effect_row), and the explosion-sound
    // lookup, which seeds with row 5's sound (`dword_A2EB80` = bank + 0x58) and keeps
    // it unless the ammo authors a tag-5 row [orig: AmmoDef_GetExplosionRadius
    // @0x409770 — `radius = dword_A2EB80` @0x40978c]. Always its 28 rows; held off the
    // table's own footprint (every World carries a table, and test Worlds stand on the stack).
    std::vector<AmmoBankRow> null_bank = std::vector<AmmoBankRow>(kImpactEffectTagCount);
    int null_bank_rows = 0; // the rows the compaction wrote, the null slot's included

    bool empty() const { return entries.empty(); }

    const AmmoTableEntry *by_index(int idx) const {
        return (idx >= 0 && static_cast<size_t>(idx) < entries.size() && entries[idx].valid)
                       ? &entries[idx]
                       : nullptr;
    }

    // Case-insensitive first-match walk [orig: AmmoDef_LookupByName @0x409870 stricmp].
    int index_of(const char *ammo_name) const {
        if (ammo_name == nullptr) return -1;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].valid) continue;
            const std::string &n = entries[i].name;
            size_t j = 0;
            while (j < n.size() && ammo_name[j] != '\0' &&
                   std::tolower(static_cast<unsigned char>(n[j])) ==
                           std::tolower(static_cast<unsigned char>(ammo_name[j])))
                ++j;
            if (j == n.size() && ammo_name[j] == '\0') return static_cast<int>(i);
        }
        return -1;
    }
};

// Where the row an impact plays comes from.
enum class ImpactRowFrom : uint8_t {
    Own,      // the struck ammo's own row of the tag (authored; a 'none' column plays nothing)
    NullBank, // the ammo authors no row of the tag: ammo def 0's static bank at the tag's PLACE
};

// The row an impact plays: the tag looked for, where its row came from, the tag ammo def
// 0's bank row at that place carries (NullBank; 0 past def 0's rows, where nothing plays),
// and its two columns ('' none).
struct ImpactRowPick {
    int tag = 0;
    ImpactRowFrom from = ImpactRowFrom::Own;
    int bank_tag = 0;
    std::string effect;
    std::string sound;
};

// The row the impact presenter plays for `tag` of `ammo` [orig: AmmoDef_ProcessImpactEffect
// @0x40a170]: a tag past the table is the obj row (`cmp ebx, 1Ch; jl; mov ebx, 4`
// @0x40a1bc..0x40a1c1); the ammo's own compacted rows are searched for the tag (`cmp [ecx],
// ebx` over its +0x6C count @0x40a1d6..0x40a1f3, a hit taken @0x40a1f7..0x40a1fd); with
// none, the row at the tag's PLACE in ammo def 0's static bank (`shl edi, 4; add edi,
// offset word_A2EB28` @0x40a1cb..0x40a1d0), whatever tag def 0 put there. The bullet's
// terrain, entity, person and water handlers, the throwable motors and the tracer zip all
// present through it. The ammo's own rows are carried tag-addressed
// (AmmoTableEntry::impact_effects), so the search is the row's `authored` flag.
inline ImpactRowPick impact_effect_row(const AmmoTable &table, const AmmoTableEntry &ammo, int tag) {
    ImpactRowPick pick;
    pick.tag = tag >= kImpactEffectTagCount ? 4 : tag;
    if (pick.tag < 0) return pick;
    const AmmoImpactEffectRow &own = ammo.impact_effects[pick.tag];
    if (own.authored) {
        pick.effect = own.effect;
        pick.sound = own.sound;
        return pick;
    }
    pick.from = ImpactRowFrom::NullBank;
    const AmmoBankRow &bank = table.null_bank[static_cast<size_t>(pick.tag)];
    pick.bank_tag = bank.tag;
    pick.effect = bank.effect;
    pick.sound = bank.sound;
    return pick;
}

// The row the knife's ray and the squib's travel play: each reads the ammo's OWN table at
// the tag's place, with no search and no fallback [orig: Weapon_RaycastAndSpawnImpact
// @0x4e88c3..0x4e88dc, @0x4e8958; Entity_ProcessProjectileTravel @0x449562..0x449579,
// @0x4495f5..0x4495ff]. Carried tag-addressed, the tag's own row, as the shipped tables
// (each a contiguous prefix of tags from 1) read it.
inline ImpactRowPick impact_effect_own_row(const AmmoTableEntry &ammo, int tag) {
    ImpactRowPick pick;
    pick.tag = tag;
    if (tag < 0 || tag >= kImpactEffectTagCount) return pick;
    pick.effect = ammo.impact_effects[tag].effect;
    pick.sound = ammo.impact_effects[tag].sound;
    return pick;
}

} // namespace opennova::world
