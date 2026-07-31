// The ammo.def table — the fired-round ballistics + damage knowledge the authoritative
// round sim consumes. POD and def-parser-free: libs/npruntime builds it from a parsed
// DefAmmoFile (npruntime/ammo_table_build.h); the engine feeds it beside the weapon table
// (NovaSimulation::load_ammo_table). [orig: g_ammoDefTable @ 0xA2ECE8 — 276-B records,
// loaded per mission from literally "ammo.def" by AmmoDef_LoadAll @ 0x40B0B0 (same
// encrypted-ASCII parse as weapon.def), token map AmmoDef_ParseProperty @ 0x40A2D0;
// docs/net/novaworld-net-re.md §5.60]
#ifndef OPENNOVA_WORLD_AMMO_TABLE_H
#define OPENNOVA_WORLD_AMMO_TABLE_H

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

// The canonical effects_table tag order — the array index IS the impact effect id the
// round-impact selection uses [orig: g_AmmoEffectTagTable @ 0x813420, 28 {name, id}
// pairs scanned from index 1 ('null' at 0 is never matchable, scan @ 0x40a46a);
// consumers: Projectile_SpawnImpactEffect @ 0x4e9b80 for ballistic impacts and the
// Knife-only Weapon_RaycastAndSpawnImpact @ 0x4e8460 leaf — terrain surface + 4,
// entity/building material + 4 (building material 1 -> 23 flesh), water = 11].
inline constexpr int kImpactEffectTagCount = 28;
inline const char *const kImpactEffectTagNames[kImpactEffectTagCount] = {
    "null", "move", "player", "zip", "obj", "dirt", "grass", "snow", "cement", "sand",
    "packeddirt", "water", "railroad", "mud", "ice", "quicksand", "stone", "wood",
    "metal", "glass", "cloth", "foliage", "hmetal", "flesh", "bodyarmor", "uwaterdeep",
    "uwatershallow", "uwatersurface",
};

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

// One baked per-tag impact row: the .ptl effect + soundset spawned on a hit of that
// class ('' = none / row absent). [orig: the staged row = {tag id, interned effect
// handle +4, soundset id +8, zeroed word +12} — 16 B stride, COMPACTED into the ammo
// record's table at block end by AmmoDef_InitEffectsTable @ 0x409f20 (16*(count+1)
// bytes, authored tags in ascending order), yet consumed by tag POSITION
// (*(ammoDef+104) + 16*tag @ 0x4e88c3) — sound only because every shipped table
// authors the full contiguous 1..24 tag prefix (row index == tag id). This 28-slot
// tag-addressed model is byte-equivalent on shipped data and resolves correctly on
// sparse tables where the original would misindex — an intentional bounded
// divergence from an original indexing assumption.]
struct AmmoImpactEffectRow {
    std::string effect;
    std::string sound;
};

// Kill-zone classes (record word +44) and the item-class damage exclusions the
// explosion sweep gates its pool walks on — libs/world stays def-parser-free,
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
inline constexpr uint32_t kAmmoFlagDesignateTarget = 0x2000000u;
inline constexpr uint32_t kAmmoFlagIgnorFoilage = 0x4000000u; // sic — the witnessed token spelling
// (parity static_asserts against DEF_AMMO_FLAG_* live in npruntime/src/weapon_table_build.cpp)

struct AmmoTableEntry {
    std::string name;               // record +144 [orig: AmmoDef_AllocateSlot copy]
    uint32_t flags = 0;             // +0 `flag` OR-bits [orig: name/bit table @0x813500]
    int32_t velocity = 0;           // +4, units/s (integer)
    int32_t max_age_ticks = 0;      // +8, 62 Hz ticks [orig: sub_40A0F0 = (62*fp16+0x8000)>>16]
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
    int32_t weight_in_grains = 0;   // +184 — the kinetic damage mass term [orig: @0x4ecb1a]
    int32_t min_stable_velocity = 0; // +176 speed-table index threshold
    int32_t tumble_error_fp16 = 0;  // +180; exact kick size, random frame still deferred
    int32_t min_damage = 0;         // +188 damage floor [orig: @0x4ecb3a]
    int32_t max_damage = 0;         // +192 damage cap when > 0 [orig: @0x4ecb42]
    int32_t penetration_impact = 0; // +196 — must reach the target itemDef+400 armor threshold
    int32_t penetration_kz = 0;     // +200 — must reach the target's blast armor (def+0x192)
    float kz_minradius = 0.0f;      // +52 (fp16 -> units) — linear-falloff start
    float kz_maxradius = 0.0f;      // +56 (fp16 -> units) — the blast radius when the
                                    // queue entry carries no float override
    int32_t kz_pieslice_bam = 0;    // +60 (deg -> BAM) — nonzero = cone blast
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
    bool valid = false;
};

// Dense, ammo.def file order (the file's own first block is the null AT_NULL entry, so
// index 0 is naturally the null ammo). [orig: two-pass count -> AmmoDef_AllocateBuffer ->
// sequential AmmoDef_AllocateSlot per `ammo <NAME>` block @0x40b0b0]
struct AmmoTable {
    std::vector<AmmoTableEntry> entries;

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

} // namespace opennova::world

#endif // OPENNOVA_WORLD_AMMO_TABLE_H
