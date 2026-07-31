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
#include <world/weapon_fsm.h>

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
    // The floating attach-label text key (emplaced guns/turrets). The original resolves
    // it against the Gametext "Overlays" section at parse and keeps the char* at
    // AdmDef+0x3A0; we keep the key and the HUD resolves at draw. Empty = key absent ->
    // the STROVER_USEGUN default label. [orig: @0x544d6c parse; consumer
    // draw_vehicle_seat_and_armory_labels @0x5a3538]
    std::string attach_text_id;
    // The two FLAGS dwords [orig: AdmDef+8 / AdmDef+12; token table @0x830bf0].
    // The switch/select paths read: flags bit 0x8000000 = the binoculars slot marker
    // [orig: WeaponSlotTable_LoadAllFromDefs tail @0x54165a]; flags2 bit 1 = NoSelect
    // (excluded from manual switching, but the ONLY defs the exact-slot select leg
    // takes — the parachute-style forced equips) [orig: Player_SwitchToWeaponByHandle
    // @0x4e02c3; Player_SelectWeaponSlot @0x4dd6d8].
    int32_t flags = 0;
    int32_t flags2 = 0;
    // Two generic stance triplets from ERROR, retained in the original 16.16
    // representation. The projectile selector is `verticalSpread ? 3 : stance`,
    // while the HUD selector is `stance + 3 * aimed_shot_available`; the triplets
    // are therefore context-dependent, not horizontal/vertical labels. [orig:
    // WeaponDefs_ParseLineCallback @0x543B21-0x543C19]
    int32_t error_fp16[6] = {0, 0, 0, 0, 0, 0};
    int32_t error_hip_theta_fp16 = 0; // AdmDef+0xCC
    int32_t error_up_theta_fp16 = 0;  // AdmDef+0xD0
    // The body updater sums these exact weights to build aim instability.
    // [orig: clipweight store @0x5440DB; weaponweight store @0x54410D]
    int32_t weaponweight_fp16 = 0;
    int32_t clipweight_fp16 = 0;
    // Whether the definition authors a nonempty first-person-model reference.
    // Resource resolution is host-side; the renderer requires both this
    // candidate and the current host resolution result before suppressing the
    // duplicate third-person emplacement for a local first-person gunner.
    // [orig: Entity_RenderVehicleModel @0x440824/@0x440833]
    bool has_first_person_model_reference = false;
    // The THIRD-PERSON world model this weapon is drawn as in a soldier's hands
    // (weapon.def `gfx3`). Kept HERE, on the table the runtime already indexes by ADM
    // index, so a held weapon resolves through the one catalog the wire's index actually
    // refers to. NOTE the IDB locals in WeaponDef_ResolveAllReferences @0x54042c are
    // swapped: `model_1p` there reads +0x170, which is this field.
    // [orig: WeaponDef.tpModel +0x170, read @ 0x4e3cd3]
    std::string third_person_model;
    // weapon_class routing slot (0=accessory 1=primary 2=secondary 3=grenade). The
    // switch eligibility exempts 1/2 from the has-ammo requirement [orig: AdmDef+0x3A4
    // read @0x4e0294; keyword 'weapon_class' -> +0x3A4].
    int32_t weapon_class_slot = 0;
    // The THIRD-PERSON body-channel triple. The original keeps no per-player copy of
    // these: the body updater indexes the AdmDefs table by the entity's OWN equipped
    // index every selection pass (`dword_24E8084[280 * entityData->equippedAdmIndex]`),
    // which is what lets every observer re-derive any player's upper-body pose from the
    // one wire byte at entity+0x2B0. Keeping them here rather than on a per-player
    // scalar is what makes a REMOTE player's hold pose resolvable at all.
    // [orig: special_hold AdmDef+0xA4 read @ 0x4b5dba; attack_anim +0xA8 read
    //  @ 0x542bbc; run_anim +0xAC read @ 0x4b72cf]
    int32_t special_hold = 0; // 1..8 selects the hold-pose ladder; 0 = rifles (mirror)
    int32_t attack_anim = 0;  // 1 knife_attack 62 / 2 grenade_attack 63; else no stamp
    int32_t run_anim = 0;     // run-gait class
    // Per-char-class startrounds overrides at the original's raw value-table indices
    // (medic=1 sniper=2 gunner=3 rifleman=5 engineer=6; 0/4 unused; 0 = absent)
    // [orig: 'classrounds' handler @0x543ab0 -> AdmDef+0x60+value*4].
    int32_t classrounds[7] = {0, 0, 0, 0, 0, 0, 0};
    // Post-recoil auto-switch: when has_switchcategory, the RECOIL action's completion
    // switches to switchcategory*65 [orig: AdmDef+0x164/+0x168; consumer @0x543062].
    int32_t switchcategory = 0;
    bool has_switchcategory = false;
    // The resolved ammo-class id for the per-class carried pools. The original resolves
    // the 'ammoclass' name to a byte id at parse (builtins @0x830F10) and keys the pool
    // arrays by it [orig: AdmDef+0xD8; pools g_localAmmoPools @0xB75FE8 / serverPlayer
    // +88664]. We assign ids by first-appearance registry order at table build — the
    // arithmetic is identical; only the id VALUES may differ from retail bytes (never
    // wire-visible; pools are entity-local).
    int16_t ammo_class_id = -1;
    // Runtime action descriptors baked from this weapon.def block's ACTION rows.
    // Auto clip durations remain zero until a host with the ADM duration ring rebakes
    // them; explicit authored timings and all state transitions are retained.
    // [orig: Anim_InitActions @0x541fa0]
    WeaponFsmDef action_fsm;
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
    // Ammo-class registry backing WeaponTableEntry::ammo_class_id: names in
    // first-appearance order, and the per-class carry caps from the top-level
    // `ammoclass_max_carry <class> <n>` weapon.def lines (0 = no cap line; the
    // original defaults the table to 0 and clamps pools against it)
    // [orig: cap table @0x24E7DE0, parse @0x543873; clamp @0x540b26].
    std::vector<std::string> ammo_class_names;
    std::vector<int32_t> ammo_class_caps;

    bool empty() const { return entries.empty(); }

    // Case-insensitive registry lookup; -1 when absent.
    int ammo_class_id_of(const char *name) const {
        if (name == nullptr || *name == '\0') return -1;
        for (size_t i = 0; i < ammo_class_names.size(); ++i) {
            const std::string &n = ammo_class_names[i];
            size_t j = 0;
            while (j < n.size() && name[j] != '\0' &&
                   std::tolower(static_cast<unsigned char>(n[j])) ==
                           std::tolower(static_cast<unsigned char>(name[j])))
                ++j;
            if (j == n.size() && name[j] == '\0') return static_cast<int>(i);
        }
        return -1;
    }

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
