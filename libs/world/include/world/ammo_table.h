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

struct AmmoTableEntry {
    std::string name;               // record +144 [orig: AmmoDef_AllocateSlot copy]
    uint32_t flags = 0;             // +0 `flag` OR-bits [orig: name/bit table @0x813500]
    int32_t velocity = 0;           // +4, units/s (integer)
    int32_t max_age_ticks = 0;      // +8, 62 Hz ticks [orig: sub_40A0F0 = (62*fp16+0x8000)>>16]
    int32_t arm_age_ticks = 0;      // +12 — a hit before arming swaps in notarmmed_ammo
    float spread_error = 0.0f;      // +24 ballistic dispersion (16.16 -> float)
    float drag = 0.0f;              // +28 (16.16 -> float)
    float bullet_radius = 0.0f;     // +32 hit-test radius (16.16 -> float)
    int32_t spread_count = 0;       // +48 shotgun/claymore pellet count
    int32_t kztype = 0;             // word +44: kill-zone class 0..7 [orig: table @0x8133E0]
    int32_t kz_damage = 0;          // word +46
    int32_t weight_in_grains = 0;   // +184 — the kinetic damage mass term [orig: @0x4ecb1a]
    int32_t min_damage = 0;         // +188 damage floor [orig: @0x4ecb3a]
    int32_t max_damage = 0;         // +192 damage cap when > 0 [orig: @0x4ecb42]
    int32_t penetration_impact = 0; // +196 — must reach the target itemDef+400 armor threshold
    int32_t tracer_rate = 0;        // byte +226 (`tracerRate`)
    std::string notarmmed_ammo;     // +241 — the not-armed child ammo name
    // The per-surface impact rows by canonical tag id. Bake rules per the witnessed
    // parser: 'none' columns stay empty, duplicate tags keep the FIRST row (the
    // original warns 'redefining the effect' @ 0x40a502), unknown tags are dropped
    // ('unknown effect tag' @ 0x40a49f), and the authored 4th (count) column is
    // DISCARDED — the original parses then zeroes it [orig: @ 0x40a587].
    AmmoImpactEffectRow impact_effects[kImpactEffectTagCount];
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
