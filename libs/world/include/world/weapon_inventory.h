// The per-player weapon slot pool, spawn-kit chain, map availability rules, and the
// manual weapon-switching walks — structural translations of the witnessed originals.
//
// The original keeps ONE local-player instance of all of this in globals (the 780-slot
// 100-B array weaponSlotArrayBase @ 0xB75FD4, the per-ammo-class pools g_localAmmoPools
// @ 0xB75FE8 + entity+288 for class 1, the 2048-B spawn-kit tuple buffer 'restrictionData'
// @ 0x24D4E00, the availability table g_armoryWeaponAvailability @ 0x24D5600) and one
// per-player copy server-side (slot+464 table / +94408 buffer / +88664 pools). This port
// gathers the local-player instance into value types; the server-side copy stays in
// libs/npruntime (D-NET-152 shape).
// [witness record: docs/net/novaworld-net-re.md §5.57/§5.58 + the loadout grill 2026-07-18]
#ifndef OPENNOVA_WORLD_WEAPON_INVENTORY_H
#define OPENNOVA_WORLD_WEAPON_INVENTORY_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "world/weapon_table.h"

namespace opennova::world {

// Weapon-slot combo = category*65 + rank; 12 categories x 65 ranks = 780 slots
// [orig: slot = table + 100*(rank + 65*category) @ 0x5415D3; scans clamp @ 780].
namespace weapon_combo {
enum : int32_t {
    kRanksPerCategory = 65,
    kCategories = 12,
    kSlotCount = 780,
    // The engine's default spawn selection: category 3 (the Primary key), rank 0
    // [orig: Player_InitPlayer @ 0x4e17ff g_currentWeaponSlot = 195].
    kDefaultSpawnCombo = 195,
};
}

// Availability values [orig: g_armoryWeaponAvailability semantics — server 0x2F gate
// @ 0x515a3f: 0 rejects, 2 additionally requires the requester inside an armory zone
// (entity Flags & 0x400000); every nonzero value lists in the armory populate
// @ 0x566e6b; the S2C 0x66 wire carries only 0/2 entries @ 0x5102e0].
namespace weapon_availability_value {
enum : int32_t {
    kBanned = 0,
    kAllowed = 1,
    kArmoryOnly = 2,
    kMissionAllowed = 3,
};
}

// Per-adm-index availability, dense like the original's 255-int table.
struct WeaponAvailability {
    std::array<int32_t, 255> values;
    WeaponAvailability() { reset(); }
    // All-allowed default [orig: memset32(.., 1, 0xFF) @ 0x42d4d1 / @ 0x551c86].
    void reset() { values.fill(weapon_availability_value::kAllowed); }
    int32_t value_for(int adm_index) const {
        return (adm_index >= 0 && adm_index < static_cast<int>(values.size()))
                       ? values[static_cast<size_t>(adm_index)]
                       : weapon_availability_value::kAllowed;
    }
};

// Apply a name->value pair list over the table order — the name-list mode of
// build_item_restriction_table @ 0x54DDB0: default 1 per entry, matched names take
// their pair value (-1 maps to 3 kMissionAllowed), and a parent's loadout_subclasses
// sub-entries INHERIT the parent's value (the skip_count walk). Pair source: the .mis
// item_availability chunk ({name, status} records).
void weapon_availability_apply_pairs(
        WeaponAvailability &avail, const WeaponTable &table,
        const std::vector<std::pair<std::string, int32_t>> &pairs);

// One spawn-kit / loadout tuple {name, ammoPrimary, ammoSecondary, flags} — the
// 4-string record of the 2048-B loadout buffers ({name\0 ammoPri\0 ammoSec\0 flags\0}*)
// [orig: restrictionData @ 0x24D4E00 and the per-class buffers @ 0x25DD740 share the
// format; values default -1 = "engine default"].
struct WeaponKitEntry {
    std::string name;
    int32_t ammo_primary = -1;
    int32_t ammo_secondary = -1;
    int32_t flags = -1;
};

// The engine defaults: the profile-less spawn kit and the everything-filtered
// fallback [orig: literal "WPN_M4AUTO" @ 0x5246be/@ 0x5519e4; the {"WPN_KNIFE",
// "-1","-1","-1"} synthesis @ 0x40f899].
std::vector<WeaponKitEntry> weapon_kit_default();
std::vector<WeaponKitEntry> weapon_kit_knife_fallback();

// The SP mission-load filter [orig: Mission_LoadBMSFile @ 0x40f7ae..0x40f95c]: keep
// only entries whose weapon resolves in the table AND has nonzero availability; if
// NOTHING survives, return the knife fallback. (Unresolved names drop silently — the
// original's catalog walk just skips them @ 0x40f830.)
std::vector<WeaponKitEntry> weapon_kit_filter_by_availability(
        const std::vector<WeaponKitEntry> &kit, const WeaponTable &table,
        const WeaponAvailability &avail);

// Build the player-slot table consumed by Weapon_CalcImpactDamage: each kit
// row's fourth value is stored at its resolved AmmoDef index. Raw byte 1 means
// x0.9, 2 means x1.1, and every other value is neutral.
void weapon_kit_build_damage_classes(const std::vector<WeaponKitEntry> &kit,
                                     const WeaponTable &table, size_t ammo_count,
                                     std::vector<uint8_t> &out);

// The display-list expansion [orig: AvatarDef_BuildDisplayList @ 0x54B9E0]: one name
// per kit entry, then the def's loadout_subclasses sub-variants (parent+1..parent+LSC)
// appended BY NAME; 255 entries cap. Unresolved kit names still land (the fill re-
// resolves and warns, matching the original's two-stage resolve).
std::vector<std::string> weapon_kit_expand_display_list(
        const std::vector<WeaponKitEntry> &kit, const WeaponTable &table);

// One 100-B weapon slot's port-relevant state [orig: MountSlot — def ptr +0x20,
// loaded rounds u16 +0x10; the FSM fields live in WeaponSlotState].
struct WeaponInventorySlot {
    int16_t adm_index = -1;
    int32_t clip = 0;
};

struct WeaponInventory {
    std::array<WeaponInventorySlot, weapon_combo::kSlotCount> slots;
    // Per-ammo-class carried pools, keyed by WeaponTableEntry::ammo_class_id. The
    // original splits storage (class 1 = entity+288 u16, others = the pool array);
    // the arithmetic (per-class cap clamp) is identical and the split is not
    // observable, so one array carries all classes here (D-WPN-24).
    // [orig: g_localAmmoPools @ 0xB75FE8; entity+288 @ 0x540ba5; caps @ 0x24E7DE0]
    std::vector<int32_t> pools;
    int32_t equipped_combo = -1; // [orig: EquippedSlot +0x118 / g_currentWeaponSlot]
    int32_t pending_combo = -1;  // [orig: entity+0x308 staged slot / g_pendingWeaponSlot]
    // entity+44 carry-presentation bits gathered by the fill (8 = def.flags&0x1000,
    // 0x10 = def.flags2&2) [orig: WeaponSlotTable_LoadAllFromDefs @ 0x5415aa/0x5415bc].
    uint32_t carry_flags = 0;

    void reset(const WeaponTable &table) {
        slots.fill(WeaponInventorySlot{});
        pools.assign(table.ammo_class_names.size(), 0);
        equipped_combo = -1;
        pending_combo = -1;
        carry_flags = 0;
    }
    const WeaponInventorySlot *slot(int32_t combo) const {
        return (combo >= 0 && combo < weapon_combo::kSlotCount)
                       ? &slots[static_cast<size_t>(combo)]
                       : nullptr;
    }
    WeaponInventorySlot *slot(int32_t combo) {
        return (combo >= 0 && combo < weapon_combo::kSlotCount)
                       ? &slots[static_cast<size_t>(combo)]
                       : nullptr;
    }
};

// Pool access with the per-class carry-cap clamp [orig: WeaponSlot_AddAmmo @ 0x540A20 /
// WeaponSlot_SetAmmoCount @ 0x540B50 / the pool leg of Entity_GetScoreValueBySlotType
// @ 0x5406E0]. Ids outside the registry are no-ops / 0.
int32_t weapon_pool_get(const WeaponInventory &inv, int class_id);
void weapon_pool_set(const WeaponTable &table, WeaponInventory &inv, int class_id,
                     int32_t amount);
void weapon_pool_add(const WeaponTable &table, WeaponInventory &inv, int class_id,
                     int32_t amount);

// The slot fill [orig: WeaponSlotTable_LoadAllFromDefs @ 0x5414E0]: resolve each
// display name, land it at slot rank+65*category, keep the FIRST def on a combo
// collision (the original logs "overloading" and keeps the incumbent), gather the
// carry bits. Unresolved names append a warning ("couldn't find wpn %s" shape).
struct WeaponFillResult {
    std::vector<std::string> warnings;
};
WeaponFillResult weapon_inventory_load_from_display(
        const WeaponTable &table, const std::vector<std::string> &display,
        WeaponInventory &inv);

// The spawn pool seeding [orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x541690, ex the
// 'WeaponOverlay_BuildTypeLookup' misnomer]: for every populated slot,
// pools[def.ammo_class_id] = classrounds[value(player_class)] when the def authors one,
// else startrounds. player_class 5..9 -> classrounds indices 1/2/3/5/6; other classes
// take plain startrounds. Walk order is slot order, later slots overwrite (witnessed).
void weapon_inventory_seed_pools(const WeaponTable &table, WeaponInventory &inv,
                                 int player_class);

// The clip normalization [orig: WeaponSlots_RecalculateAmmoFromCapacity @ 0x542280]:
// for each populated slot with ammo_class_count (pool-units/round) nonzero and
// clipsize != -1: return the current clip to the pool, then draw one full clip
// clamped by what the pool affords. The def+0xDC pass-type leg (shared-pool "clip"
// weapons) is deferred — see the RE record's divergence entry.
void weapon_inventory_recalc_clips(const WeaponTable &table, WeaponInventory &inv);

// The eligibility ammo score [orig: calculate_kill_score @ 0x5407E0 in its
// slot-predicate role: pool for the def's ammo class + the slot's loaded rounds].
int32_t weapon_slot_ammo_score(const WeaponTable &table, const WeaponInventory &inv,
                               int32_t combo);

// The reload transfer [orig: WeaponSlot_ReloadAmmo @ 0x541720, net-re §5.58]: refund
// the remaining clip into the pool, then refill to clipsize clamped by the pool.
// Returns the rounds now loaded (unchanged when the def has no clip).
int32_t weapon_inventory_reload_slot(const WeaponTable &table, WeaponInventory &inv,
                                     int32_t combo);

// Player_SelectWeaponSlot @ 0x4DD680 — the category-level select. Stages the pending
// slot; the caller commits pending->equipped unless seated (the original applies to
// EquippedSlot only when !parentEntity || parentSlot not in {2,3}).
//   combo >= 0: the EXACT slot is taken only when its def has the flags2&1 NoSelect
//   bit (the parachute-style forced equips); otherwise the first populated
//   NON-NoSelect slot of the combo's category (rank order), else the first such slot
//   globally. combo == -1: the global scan directly. Returns false when nothing
//   stages (empty pool).
bool weapon_select_slot(const WeaponTable &table, WeaponInventory &inv, int32_t combo,
                        bool commit_equip);

// The FSM/stance gates the switch walks consult. The caller (the sim) supplies its
// live values; pass defaults when no weapon FSM is mounted.
struct WeaponSwitchGates {
    // parentSlot in {2,3,5} blocks manual switching [orig: @ 0x4e0192 / @ 0x4dfe91].
    bool seat_blocked = false;
    // parentEntity && parentSlot in {2,3} defers the equip commit [orig: @ 0x4dd6fc].
    bool equip_blocked = false;
    // The equipped slot's FSM action id (weapon_action::*) and its def category.
    // Blocked when: (RELOAD and same category) or FIRE or (RECOIL and different
    // category) [orig: @ 0x4e0192..0x4e0223 mount-state gate].
    int32_t equipped_action = 0;
    bool equipped_valid = false; // an equipped slot with a def exists
};

struct WeaponSwitchOutcome {
    enum Kind : int32_t {
        kNone = 0,  // gate-blocked or nothing to do
        kMount = 1, // mount the slot at .combo (same-category => switchrank leg)
        kDeny = 2,  // the wrap-around deny sound [orig: PlaySoundOnDedicatedServer
                    //  (dword_24E08C4) @ 0x4e0354]
    };
    Kind kind = kNone;
    int32_t combo = -1;
    bool same_category = false; // MountWeaponSlot leg select [orig: @ 0x4dfb8b]
};

// Player_SwitchToWeaponByHandle @ 0x4E0170 — the category-key walk. handle =
// category*65 + rank (the input cases 200-210 pass (action-200)*65 @ 0x4e1144).
// Same-category presses rank-cycle; eligibility = weapon_class_slot in {1,2} OR
// ammo score nonzero, AND !(flags2 & 1).
WeaponSwitchOutcome weapon_switch_to_handle(const WeaponTable &table,
                                            WeaponInventory &inv, int32_t handle,
                                            const WeaponSwitchGates &gates);

// Player_CycleWeaponSlot @ 0x4DFE70 — next/prev over ALL 780 combos (direction +1/-1),
// every candidate needs the ammo score (no weapon_class exemption), silent stop on
// wrap (no deny).
WeaponSwitchOutcome weapon_cycle_slot(const WeaponTable &table, WeaponInventory &inv,
                                      int32_t direction,
                                      const WeaponSwitchGates &gates);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WEAPON_INVENTORY_H
