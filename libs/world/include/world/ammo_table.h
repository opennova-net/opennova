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
