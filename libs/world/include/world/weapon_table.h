// The armory / AdmDef weapon table resolved from weapon.def — the server-side weapon knowledge
// the loadout service (C2S 0x2F -> S2C 0x5A), the extended-uplink equipped-weapon gate, and the
// player spawn default read. POD and def-parser-free: libs/npruntime builds it from a parsed
// DefWeaponsFile (npruntime/weapon_table_build.h); the engine feeds it from the resource root
// (NovaSimulation::load_weapon_table). [orig: the AdmDefs table @0x24E7FE0, 255 x 1120 B;
// docs/net/novaworld-net-re.md §5.57]
#ifndef OPENNOVA_WORLD_WEAPON_TABLE_H
#define OPENNOVA_WORLD_WEAPON_TABLE_H

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

struct WeaponTableEntry {
    std::string name;               // entry+20 [orig: strncpy @0x543737]
    uint8_t category = 0;           // +0x00, 0..11 [orig: @0x5439C6]; the uplink ingest gate is category < 11
    uint8_t rank = 0;               // +0x10, 0..64; weapon-slot combo = category*65 + rank [orig: @0x543A1B]
    int16_t clipsize = 1;           // +0x58, engine default 1; -1 = no-clip weapon (knife/medpack)
                                    // [orig: AdmDef_InitEntryDefaults @0x53ff13]
    int16_t startrounds = -1;       // +0x5C, engine default -1 [orig: @0x53ff19]
    int16_t maxclips = 0;           // +0x14C [orig: @0x5440A9]
    uint8_t charfilter = 0;         // +0x7C OR-mask: medic=1 sniper=2 gunner=4 rifleman=8 engineer=0x10
                                    // [orig: @0x543F6E, token table @0x830EB0]
    uint8_t teamfilter = 0;         // +0x80 OR-mask: red=1 blue=2 [orig: @0x543FE3, token table @0x830ED8]
    uint8_t loadout_selectable = 0; // +0x3A8
    uint8_t loadout_subclasses = 0; // +0x3AC — data ONLY; sub-variant blocks allocate their own slots
                                    // [orig: @0x544E43 is a plain scalar store]
    std::string ammo_class;         // `ammoclass <name> <n>` [orig: @0x5441CB]
    int16_t ammo_class_count = 0;
    int16_t ammo_bucket = 0;        // [orig: @0x544045]
    // The fired round: `round_type "AMMO_X"`, resolved to an AmmoTable index at load —
    // the original stores the resolved index pair at adm+84 (RoundData_AddRound reads
    // adm dword 21) [orig: §5.60; resolve = AmmoDef_LookupByName]. -1 = unresolved.
    std::string round_type;
    int16_t ammo_index = -1;
    bool valid = false;
};

// Dense-by-adm-index. Entry 0 is the engine-created "null" [orig: AnimDef_InitAll @0x543615
// wipes the table and names slot 0 "null" right before weapon.def parses, Game_StartMission
// @0x5254b3/@0x5254bd]. Each `weapon "NAME"` block reuses an existing same-name entry, else
// takes the LOWEST free slot [orig: WeaponDefs_ParseLineCallback @0x5436e1 ->
// AdmDef_FindFreeSlot @0x53FC50] — so one parse into a fresh table is pure file order,
// 1-based. A parent's loadout_subclasses sub-variants sit at parent+1..parent+LSC (their
// blocks directly follow it in the file), which the 0x5A alt-ammo walk depends on
// [orig: @0x5027c8].
struct WeaponTable {
    std::vector<WeaponTableEntry> entries;

    bool empty() const { return entries.empty(); }

    const WeaponTableEntry *by_index(uint8_t idx) const {
        return (idx < entries.size() && entries[idx].valid) ? &entries[idx] : nullptr;
    }

    // Case-insensitive, first match [orig: AvatarDef_FindIndexByName @0x53FD80 stricmp walk].
    int index_of(const char *weapon_name) const {
        if (weapon_name == nullptr) return -1;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].valid) continue;
            const std::string &n = entries[i].name;
            size_t j = 0;
            while (j < n.size() && weapon_name[j] != '\0' &&
                   std::tolower(static_cast<unsigned char>(n[j])) ==
                           std::tolower(static_cast<unsigned char>(weapon_name[j])))
                ++j;
            if (j == n.size() && weapon_name[j] == '\0') return static_cast<int>(i);
        }
        return -1;
    }
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WEAPON_TABLE_H
