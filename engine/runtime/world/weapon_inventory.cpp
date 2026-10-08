// Structural translations of the witnessed loadout/switching originals — see
// weapon_inventory.h for the model overview and docs/net/novaworld-net-re.md
// §5.57/§5.58 + the 2026-07-18 loadout grill for the witness record.
#include <runtime/world/weapon_inventory.h>

#include <cstdio>

#include <base/io/strutil.h>
#include <formats/playersav/weapon_sav.h>

namespace opennova::world {

namespace {

const WeaponTableEntry *entry_at(const WeaponTable &table, const WeaponInventory &inv,
                                 int32_t combo) {
    const WeaponInventorySlot *s = inv.slot(combo);
    if (s == nullptr || s->adm_index < 0) return nullptr;
    return table.by_index(static_cast<uint8_t>(s->adm_index));
}

// classrounds index for a player class [orig: the char-class value table @ 0x830EE8 —
// medic(5)=1, sniper(6)=2, gunner(7)=3, rifleman(8)=5, engineer(9)=6; the seeder
// switch @ 0x5416c4 maps playerClass to exactly these entries].
int classrounds_index(int player_class) {
    switch (player_class) {
        case 5: return 1;
        case 6: return 2;
        case 7: return 3;
        case 8: return 5;
        case 9: return 6;
        default: return -1;
    }
}

// The switch/cycle eligibility ammo term shared with the deny walks
// [orig: Score_CalculateKillScore @ 0x5407E0 as the slot predicate — nonzero selects].
int32_t ammo_score(const WeaponTable &table, const WeaponInventory &inv, int32_t combo) {
    const WeaponTableEntry *def = entry_at(table, inv, combo);
    const WeaponInventorySlot *s = inv.slot(combo);
    if (def == nullptr || s == nullptr) return 0;
    return weapon_pool_get(inv, def->ammo_class_id) +
            weapon_inventory_loaded_rounds(table, inv, combo);
}

} // namespace

// [orig: sub_5405F0 @0x5405F0 / sub_540670 @0x540670; carried-player legs]
int32_t weapon_inventory_loaded_rounds(const WeaponTable &table,
        const WeaponInventory &inv, int32_t combo) {
    const auto *def = entry_at(table, inv, combo);
    if (def == nullptr) return 0;
    if (def->ammo_bucket == 0) return static_cast<int16_t>(inv.slot(combo)->clip);
    const uint32_t bucket = static_cast<uint32_t>(def->ammo_bucket);
    return bucket < inv.shared_clips.size() ? inv.shared_clips[bucket] : 0;
}

void weapon_inventory_set_loaded_rounds(const WeaponTable &table,
        WeaponInventory &inv, int32_t combo, int32_t rounds) {
    const auto *def = entry_at(table, inv, combo);
    if (def == nullptr) return;
    if (def->ammo_bucket == 0) {
        inv.slot(combo)->clip = static_cast<int16_t>(rounds);
        return;
    }
    const uint32_t bucket = static_cast<uint32_t>(def->ammo_bucket);
    if (bucket < inv.shared_clips.size()) inv.shared_clips[bucket] = rounds;
}

void weapon_availability_apply_pairs(
        WeaponAvailability &avail, const WeaponTable &table,
        const std::vector<std::pair<std::string, int32_t>> &pairs) {
    // The name-list mode of WeaponDef_BuildItemRestrictionTable @ 0x54DDB0: default 1,
    // matched names take the pair value (-1 -> 3), sub-entries inherit the parent's
    // value through the loadout_subclasses skip walk.
    int32_t value = weapon_availability_value::kAllowed;
    int skip_count = 0;
    for (size_t i = 0; i < avail.values.size(); ++i) {
        if (skip_count > 0) {
            --skip_count; // value carries the parent's [orig: @ 0x54de50]
        } else {
            value = weapon_availability_value::kAllowed;
            const WeaponTableEntry *entry =
                    (i < table.entries.size() && table.entries[i].valid)
                            ? &table.entries[i]
                            : nullptr;
            if (entry != nullptr && !entry->name.empty()) {
                skip_count = entry->loadout_subclasses; // [orig: entry+36 @ 0x54ddf4]
                for (const auto &p : pairs) {
                    if (opennova::strutil::iequals(p.first.c_str(),
                                                   entry->name.c_str())) {
                        value = p.second;
                        if (value == -1) value = weapon_availability_value::kMissionAllowed;
                        break; // [orig: first stricmp match @ 0x54de0c]
                    }
                }
            }
        }
        avail.values[i] = value;
    }
}

std::vector<WeaponKitEntry> weapon_kit_default() {
    // [orig: Buffer_CopyUntilDoubleNull(restrictionData, 0x833BF8, 2048)
    //  @ 0x5246be / @ 0x5519e4 — the profile-less spawn kit: the whole static
    //  page blob (eight "<name> -1 -1 -1" rows) the double-NUL copy takes, the
    //  page a fresh profile's single-player page is seeded from]
    std::vector<WeaponKitEntry> kit;
    for (const playersav::KitEntry &e : playersav::default_single_player_page().entries)
        kit.push_back(WeaponKitEntry{e.name, e.ammo_primary, e.ammo_secondary, e.flags});
    return kit;
}

std::vector<WeaponKitEntry> weapon_kit_knife_fallback() {
    // [orig: the {"WPN_KNIFE","-1","-1","-1"} synthesis @ 0x40f899..0x40f94b]
    return {WeaponKitEntry{"WPN_KNIFE", -1, -1, -1}};
}

std::vector<WeaponKitEntry> weapon_kit_filter_by_availability(
        const std::vector<WeaponKitEntry> &kit, const WeaponTable &table,
        const WeaponAvailability &avail) {
    // [orig: Mission_LoadBMSFile @ 0x40f7ae..0x40f95c — the catalog walk drops
    //  unresolved names (@ 0x40f830) and availability-0 entries (@ 0x40f834)]
    std::vector<WeaponKitEntry> out;
    for (const WeaponKitEntry &e : kit) {
        int idx = table.index_of(e.name.c_str());
        if (idx < 0) continue;
        if (avail.value_for(idx) == weapon_availability_value::kBanned) continue;
        out.push_back(e);
    }
    if (out.empty()) return weapon_kit_knife_fallback();
    return out;
}

void weapon_kit_build_damage_classes(const std::vector<WeaponKitEntry> &kit,
                                     const WeaponTable &table, size_t ammo_count,
                                     std::vector<uint8_t> &out) {
    out.assign(ammo_count, 0);
    for (const WeaponKitEntry &entry : kit) {
        const int adm = table.index_of(entry.name.c_str());
        if (adm < 0) continue;
        const WeaponTableEntry *weapon = table.by_index(static_cast<uint8_t>(adm));
        if (weapon == nullptr || weapon->ammo_index < 0) continue;
        const size_t ammo_index = static_cast<size_t>(weapon->ammo_index);
        if (ammo_index < out.size()) {
            const uint8_t raw = static_cast<uint8_t>(entry.flags);
            out[ammo_index] = (raw == 1 || raw == 2) ? raw : 0;
        }
    }
}

std::vector<std::string> weapon_kit_expand_display_list(
        const std::vector<WeaponKitEntry> &kit, const WeaponTable &table) {
    // [orig: AvatarDef_BuildDisplayList @ 0x54B9E0 — 255 x 40-B entries: the kit
    //  name lands verbatim, then the def's loadout_subclasses sub-variants
    //  (parent+1..parent+LSC) append by name]
    std::vector<std::string> out;
    for (const WeaponKitEntry &e : kit) {
        if (out.size() >= 255) break;
        out.push_back(e.name);
        int parent = table.index_of(e.name.c_str());
        if (parent < 0) continue;
        const WeaponTableEntry *pdef = table.by_index(static_cast<uint8_t>(parent));
        if (pdef == nullptr) continue;
        for (int k = 1; k <= pdef->loadout_subclasses && out.size() < 255; ++k) {
            const WeaponTableEntry *sub =
                    (parent + k < 256) ? table.by_index(static_cast<uint8_t>(parent + k))
                                       : nullptr;
            if (sub == nullptr) continue; // [orig: null sub skipped @ 0x54baf8]
            out.push_back(sub->name);
        }
    }
    return out;
}

int32_t weapon_pool_get(const WeaponInventory &inv, int class_id) {
    // [orig: Entity_GetScoreValueBySlotType @ 0x5406E0 pool leg — class 1 reads
    //  entity+288, others g_LocalAmmoPools[class]; one array here (see header)]
    if (class_id < 0 || class_id >= static_cast<int>(inv.pools.size())) return 0;
    return inv.pools[static_cast<size_t>(class_id)];
}

void weapon_pool_set(const WeaponTable &table, WeaponInventory &inv, int class_id,
                     int32_t amount) {
    // [orig: WeaponSlot_SetAmmoCount @ 0x540B50 — write then clamp to the
    //  ammoclass_max_carry cap]
    if (class_id < 0 || class_id >= static_cast<int>(inv.pools.size())) return;
    int32_t cap = (class_id < static_cast<int>(table.ammo_class_caps.size()))
                          ? table.ammo_class_caps[static_cast<size_t>(class_id)]
                          : 0;
    if (amount > cap) amount = cap;
    inv.pools[static_cast<size_t>(class_id)] = amount;
}

void weapon_pool_add(const WeaponTable &table, WeaponInventory &inv, int class_id,
                     int32_t amount) {
    // [orig: WeaponSlot_AddAmmo @ 0x540A20 — add then clamp to the cap]
    if (class_id < 0 || class_id >= static_cast<int>(inv.pools.size())) return;
    int32_t cap = (class_id < static_cast<int>(table.ammo_class_caps.size()))
                          ? table.ammo_class_caps[static_cast<size_t>(class_id)]
                          : 0;
    int32_t v = inv.pools[static_cast<size_t>(class_id)] + amount;
    if (v > cap) v = cap;
    inv.pools[static_cast<size_t>(class_id)] = v;
}

bool weapon_slot_zoom_sniper_lock(int32_t owner_class, int32_t def_category,
                                  bool allow_sniper_scope_zoom) {
    // [orig: WeaponSlot_InitFromDef — `cmp [ebp+294h],6` @ 0x53EEF9, `cmp [ecx],3`
    //  @ 0x53EF02, the permission @ 0x53EF07..0x53EF25]
    return owner_class == 6 && def_category == 3 && !allow_sniper_scope_zoom;
}

int32_t weapon_slot_initial_zoom(int32_t scope_max_mag, int32_t scope_initial_mag,
                                 int32_t scope_min_mag, bool sniper_lock) {
    // [orig: WeaponSlot_InitFromDef — floor = Def+0x98 @ 0x53EEF1, or Def+0x90
    //  under the lock @ 0x53EF27; +0xC = Def+0x94 @ 0x53EF2D..0x53EF35]
    const int32_t floor = sniper_lock ? scope_max_mag : scope_min_mag;
    if (scope_initial_mag < floor) return floor;                   // @ 0x53EF38 -> @ 0x53EF44
    return scope_initial_mag > scope_max_mag ? scope_max_mag       // @ 0x53EF3A..0x53EF44
                                             : scope_initial_mag;
}

WeaponFillResult weapon_inventory_load_from_display(
        const WeaponTable &table, const std::vector<std::string> &display,
        WeaponInventory &inv, int32_t owner_class, bool allow_sniper_scope_zoom) {
    // [orig: WeaponSlotTable_LoadAllFromDefs @ 0x5414E0]
    WeaponFillResult result;
    char msg[192];
    inv.carry_flags &= ~0x18u; // [orig: entity+44 &= 0xFFFFFFE7 @ 0x541503]
    size_t count = display.size() < 255 ? display.size() : 255;
    for (size_t i = 0; i < count; ++i) {
        const std::string &name = display[i];
        if (name.empty()) continue;
        int adm = table.index_of(name.c_str());
        if (adm < 0) {
            std::snprintf(msg, sizeof(msg),
                          "wpnslots_loadall: couldn't find wpn index %s", name.c_str());
            result.warnings.emplace_back(msg);
            continue;
        }
        const WeaponTableEntry *def = table.by_index(static_cast<uint8_t>(adm));
        if (def == nullptr) {
            std::snprintf(msg, sizeof(msg), "wpnslots_loadall: couldn't find wpn %s",
                          name.c_str());
            result.warnings.emplace_back(msg);
            continue;
        }
        if ((def->flags & weapon_flag::kArmor) != 0) inv.carry_flags |= 8u;   // [orig: @ 0x5415aa]
        if ((def->flags2 & 2) != 0) inv.carry_flags |= 0x10u;    // [orig: @ 0x5415ba]
        int32_t combo = def->rank + weapon_combo::kRanksPerCategory * def->category;
        WeaponInventorySlot *slot = inv.slot(combo); // bounds guard only; the
                                                     // original trusts category<12/rank<65
        if (slot == nullptr) continue;
        if (slot->adm_index >= 0 && slot->adm_index != adm) {
            const WeaponTableEntry *incumbent =
                    table.by_index(static_cast<uint8_t>(slot->adm_index));
            std::snprintf(msg, sizeof(msg),
                          "wpnslots_loadall: overloading wpn %s in category %i rank %i, "
                          "with wpn %s",
                          incumbent != nullptr ? incumbent->name.c_str() : "?",
                          def->category, def->rank, name.c_str());
            result.warnings.emplace_back(msg); // incumbent stays [orig: @ 0x5415d6]
            continue;
        }
        weapon_slot_init_from_def(*def, static_cast<int16_t>(adm), *slot, owner_class,
                                  allow_sniper_scope_zoom);
    }
    // The local player's ToSpecial slots: both cleared, then the first slot in
    // combo order whose def carries QuickSwitch is the target
    // [orig: @ 0x541626..0x54166E -- the g_LocalPlayerEntity gate @ 0x541626,
    //  the clears @ 0x541638 / @ 0x54163E, the walk @ 0x541647..0x541668,
    //  `and eax, 8000000h` @ 0x541653]
    inv.quick_switch_combo = -1;
    inv.quick_switch_stash = -1;
    for (int32_t combo = 0; combo < weapon_combo::kSlotCount; ++combo) {
        const WeaponTableEntry *def = entry_at(table, inv, combo);
        if (def != nullptr && (static_cast<uint32_t>(def->flags) & weapon_flag::kQuickSwitch) != 0) {
            inv.quick_switch_combo = combo;
            break;
        }
    }
    return result;
}

void weapon_slot_init_from_def(const WeaponTableEntry &def, int16_t adm_index,
                               WeaponInventorySlot &slot, int32_t owner_class,
                               bool allow_sniper_scope_zoom) {
    // [orig: WeaponSlot_InitFromDef @ 0x53EE70 — the memset @ 0x53EE86 (clip 0),
    //  the def bound @ 0x53EEA1, the zoom seed @ 0x53EF2D..0x53EF44]
    slot = WeaponInventorySlot{};
    slot.adm_index = adm_index;
    slot.clip = 0;
    slot.scope_zero = weapon_scope_zero_initial(def.action_fsm.scope_zero);
    slot.scope_zoom = weapon_slot_initial_zoom(def.scope_max_mag, def.scope_initial_mag,
            def.scope_min_mag,
            weapon_slot_zoom_sniper_lock(owner_class, def.category, allow_sniper_scope_zoom));
}

namespace {

int32_t combo_of(const WeaponTableEntry &def) {
    return def.rank + weapon_combo::kRanksPerCategory * def.category;
}

// The refill both legs of the grant run on a slot whose def carries a clip:
// the loaded rounds zeroed (the shared bucket's when the def names one), the
// class pool set to the def's startrounds, one reload drawn.
// [orig: WeaponSlot_InitFromAvatarDef — the child leg @0x542848..0x542883, the
//  tail @0x5428CE..0x542909: sub_540670(bucket, picker, 0) or slot+0x10 = 0,
//  WeaponSlot_SetAmmoCount(def+0x5C, def+0xD8, picker), WeaponSlot_ReloadAmmo
//  (picker, slotIndex)]
void refill_slot(const WeaponTable &table, WeaponInventory &inv, const WeaponTableEntry &def,
                 int32_t slot_combo, int32_t reload_combo) {
    WeaponInventorySlot *slot = inv.slot(slot_combo);
    if (slot == nullptr) return;
    if (def.ammo_bucket != 0) {
        const uint32_t bucket = static_cast<uint32_t>(def.ammo_bucket);
        if (bucket < inv.shared_clips.size()) inv.shared_clips[bucket] = 0;
    } else {
        slot->clip = 0;
    }
    weapon_pool_set(table, inv, def.ammo_class_id, def.startrounds);
    weapon_inventory_reload_slot(table, inv, reload_combo);
}

} // namespace

WeaponAvatarGrant weapon_inventory_init_from_avatar_def(const WeaponTable &table,
        WeaponInventory &inv, int32_t adm_index, int32_t owner_class,
        bool allow_sniper_scope_zoom) {
    // [orig: WeaponSlot_InitFromAvatarDef @ 0x542730]
    WeaponAvatarGrant out;
    if ((adm_index & 0xFF) == 0xFF) return out;                  // @0x542753
    int32_t cur_adm = adm_index & 0xFF;
    const WeaponTableEntry *cur = table.by_index(static_cast<uint8_t>(cur_adm));
    if (cur == nullptr || cur->name.empty()) return out;         // @0x542762..0x54276B
    // The `sameas` redirect: the named weapon's slot becomes the target, and a
    // HELD one takes the grant as its own def [orig: @0x542779..0x5427C8].
    int32_t slot_combo = -1; // retail's null slot pointer
    if (!cur->sameas.empty()) {
        const int parent = table.index_of(cur->sameas.c_str()); // AvatarDef_FindIndexByName
        const WeaponTableEntry *pdef =
                parent >= 0 && parent != 0xFF ? table.by_index(static_cast<uint8_t>(parent))
                                              : nullptr;
        if (pdef != nullptr && !pdef->name.empty()) {
            slot_combo = combo_of(*pdef);                        // @0x5427BA
            const WeaponInventorySlot *held = inv.slot(slot_combo);
            if (held != nullptr && held->adm_index >= 0) {       // @0x5427BE
                cur = pdef;
                cur_adm = parent;
            }
        }
    }
    // The def's own slot unless it is category 0 rank 0 [orig: @0x5427D1..0x5427E1]
    const int32_t own_combo = combo_of(*cur);
    if (own_combo != 0) slot_combo = own_combo;
    WeaponInventorySlot *slot = inv.slot(slot_combo);
    if (slot == nullptr) return out;                             // @0x5427E7
    if (slot->adm_index < 0) {                                   // @0x5427F1
        // The sub-variants first: the loadout_subclasses entries that follow
        // the def, each into its own slot (initialized even when held). Their
        // reload runs on the MAIN slot, still empty here, so it draws nothing
        // but the window stamp [orig: @0x542800..0x5428A6; ReloadAmmo(picker,
        // slotIndex) @0x542883].
        for (int32_t k = 0; k < cur->loadout_subclasses; ++k) {
            const int32_t child_adm = cur_adm + 1 + k;
            const WeaponTableEntry *child =
                    child_adm < 0xFF ? table.by_index(static_cast<uint8_t>(child_adm)) : nullptr;
            if (child == nullptr) continue;
            WeaponInventorySlot *child_slot = inv.slot(combo_of(*child));
            if (child_slot == nullptr) continue;
            weapon_slot_init_from_def(*child, static_cast<int16_t>(child_adm), *child_slot,
                                      owner_class, allow_sniper_scope_zoom); // @0x542839
            if (child->clipsize != -1) {                         // @0x542848
                refill_slot(table, inv, *child, combo_of(*child), own_combo);
                out.reloaded = true;
            }
        }
        weapon_slot_init_from_def(*cur, static_cast<int16_t>(cur_adm), *slot, owner_class,
                                  allow_sniper_scope_zoom);      // @0x5428B2
    }
    // The refill tail on the slot's def [orig: @0x5428BA..0x542909].
    const WeaponTableEntry *slot_def = slot->adm_index >= 0
            ? table.by_index(static_cast<uint8_t>(slot->adm_index))
            : nullptr;
    if (slot_def != nullptr && slot_def->ammo_class_count != 0 && slot_def->clipsize != -1) {
        refill_slot(table, inv, *slot_def, slot_combo, own_combo);
        out.reloaded = true;
    }
    out.combo = slot_combo;
    return out;
}

bool weapon_inventory_weapon_full(const WeaponTable &table, const WeaponInventory &inv,
                                  int32_t adm_index) {
    // [orig: WeaponSlot_RecalculateScore @ 0x542450, the slot leg]
    if ((adm_index & 0xFF) == 0xFF) return false;                // @0x54249E
    const WeaponTableEntry *def = table.by_index(static_cast<uint8_t>(adm_index & 0xFF));
    if (def == nullptr || def->name.empty()) return false;       // @0x5424AB..0x5424BC
    int32_t combo = combo_of(*def);                              // @0x5424BE..0x5424D0
    if (!def->sameas.empty()) {                                  // @0x5424D2
        // AdmDef_GetEntryByIndex(AvatarDef_FindIndexByName(sameas)): a hit
        // names its slot whether or not it is held [orig: @0x5424D7..0x5424F9]
        const int parent = table.index_of(def->sameas.c_str());
        const WeaponTableEntry *pdef =
                parent >= 0 && parent != 0xFF ? table.by_index(static_cast<uint8_t>(parent))
                                              : nullptr;
        if (pdef != nullptr) combo = combo_of(*pdef);
    }
    const WeaponInventorySlot *slot = inv.slot(combo);
    if (slot == nullptr || slot->adm_index < 0) return false;    // @0x5424FB..0x542506
    const WeaponTableEntry *slot_def = table.by_index(static_cast<uint8_t>(slot->adm_index));
    if (slot_def == nullptr) return false;
    // Score_CalculateKillScore(slot) against the slot def's startrounds, setz
    // [orig: @0x54251E..0x54253A]
    return weapon_slot_ammo_score(table, inv, combo) == slot_def->startrounds;
}

bool weapon_inventory_all_full(const WeaponTable &table, const WeaponInventory &inv,
                               bool picker_has_item_def) {
    // [orig: WeaponSlot_RecalculateScore @ 0x542450, the -1 walk]
    for (int32_t combo = 0; combo < weapon_combo::kSlotCount; ++combo) {
        const WeaponTableEntry *def = entry_at(table, inv, combo);
        if (def == nullptr) continue;                            // @0x542543..0x542547
        // Entity_GetScoreValueBySlotType(def+0xD8) + the loaded rounds (the
        // shared bucket when def+0xDC is set), 0 without an item def
        // [orig: @0x54254D..0x542587]
        const int32_t score = picker_has_item_def ? weapon_slot_ammo_score(table, inv, combo) : 0;
        if (score != def->startrounds) return false;             // @0x542589..0x54258C
    }
    return true;                                                 // @0x54259F
}

void weapon_inventory_seed_pools(const WeaponTable &table, WeaponInventory &inv,
                                 int player_class) {
    // [orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x541690 — for every populated slot:
    //  pools[def ammoclass] = classrounds override (when nonzero) else startrounds;
    //  raw table writes, later slots overwrite, NO cap clamp]
    int cls_idx = classrounds_index(player_class);
    for (int32_t combo = 0; combo < weapon_combo::kSlotCount; ++combo) {
        const WeaponTableEntry *def = entry_at(table, inv, combo);
        if (def == nullptr) continue;
        int32_t value = def->startrounds;
        if (cls_idx >= 0 && def->classrounds[cls_idx] != 0)
            value = def->classrounds[cls_idx];
        if (def->ammo_class_id < 0 ||
            def->ammo_class_id >= static_cast<int>(inv.pools.size()))
            continue;
        inv.pools[static_cast<size_t>(def->ammo_class_id)] = value;
    }
}

int32_t weapon_inventory_total_clips(const WeaponTable &table, const WeaponInventory &inv,
                                     int32_t combo) {
    // [orig: WeaponSlot_GetTotalClips @ 0x5425F0 — null def -> 0 @0x5425fc;
    //  pool = Entity_GetScoreValueBySlotType(def+216) @0x542640 + the loaded
    //  rounds slot+16 @0x54265b; clipsize -1 -> -1 @0x542669; /= clipsize
    //  @0x542673; clamp 127 @0x54267a]
    const WeaponTableEntry *def = entry_at(table, inv, combo);
    const WeaponInventorySlot *slot = inv.slot(combo);
    if (def == nullptr || slot == nullptr) return 0;
    if (static_cast<uint32_t>(def->ammo_bucket) >= table.ammo_class_names.size()) return 0;
    int32_t result = weapon_pool_get(inv, def->ammo_class_id) +
            weapon_inventory_loaded_rounds(table, inv, combo);
    if (def->clipsize == -1) return -1;
    if (def->clipsize != 0) result /= def->clipsize;
    if (result > 127) return 127;
    return result;
}

int32_t weapon_inventory_loadout_weight_fp16(const WeaponTable &table,
                                             const WeaponInventory &inv) {
    // [orig: Terrain_AccumulateSectorScores @ 0x425220 (misnamed): the slot walk
    //  @0x425233..0x425304, weaponweight @0x425252, clips x clipweight @0x42525e..
    //  0x425272, the +0x3AC sub-entry loop @0x425286..0x4252c8 keyed on the
    //  +0xD8 ammo-class byte @0x4252b2, the skip @0x4252ec]
    int32_t weight = 0;
    for (int32_t combo = 0; combo < weapon_combo::kSlotCount; ++combo) {
        const WeaponTableEntry *def = entry_at(table, inv, combo);
        const WeaponInventorySlot *slot = inv.slot(combo);
        if (def == nullptr || slot == nullptr) continue;
        weight += def->weaponweight_fp16 +
                  weapon_inventory_total_clips(table, inv, combo) * def->clipweight_fp16;
        const int32_t subs = def->loadout_subclasses;
        for (int32_t k = 1; k <= subs; ++k) {
            // The sub-variant entries follow the main's adm index
            // [orig: AdmDef_GetEntryByIndex(mainIndex + k) @0x42528e].
            const WeaponTableEntry *sub = table.by_index(
                    static_cast<uint8_t>(static_cast<int32_t>(slot->adm_index) + k));
            if (sub == nullptr) continue;
            const int32_t sub_combo = static_cast<int32_t>(sub->category) * 65 + sub->rank;
            if (sub->ammo_class_id == def->ammo_class_id) continue;
            // The FIRST sub-entry of a different ammo class ends the scan: its
            // clips ride the MAIN def's clipweight, then the loop breaks — a
            // second differing sub-entry never counts [orig: the `jnz` out of
            // the loop @0x4252b8 into the single add @0x4252d1..0x4252e2].
            weight += weapon_inventory_total_clips(table, inv, sub_combo) * def->clipweight_fp16;
            break;
        }
        combo += subs; // the walk skips the sub-variant slots [orig: @0x4252ec]
    }
    return weight;
}

void weapon_inventory_apply_authority_pools(const WeaponTable &table, WeaponInventory &inv,
                                            const std::array<int32_t, 128> &pools) {
    // [orig: NapiNPClientMsg_0x00F @ 0x42e324..0x42e34a copy loop, then the
    //  recalc call @ 0x42e424]
    const size_t count = inv.pools.size() < pools.size() ? inv.pools.size() : pools.size();
    for (size_t i = 0; i < count; ++i) inv.pools[i] = pools[i];
    weapon_inventory_recalc_clips(table, inv);
}

void weapon_inventory_recalc_clips(const WeaponTable &table, WeaponInventory &inv) {
    // [orig: WeaponSlots_RecalculateAmmoFromCapacity @ 0x542280 — return the clip to
    //  the pool, then draw one full clip clamped by what the pool affords. The
    //  arithmetic is ported literally, including the negative-pool degeneration the
    //  record notes for shipped -1 startrounds (§5.57 "the v15 ammo issues"). The
    //  def+0xDC selects the shared loaded-round bucket.]
    for (int32_t combo = 0; combo < weapon_combo::kSlotCount; ++combo) {
        const WeaponTableEntry *def = entry_at(table, inv, combo);
        WeaponInventorySlot *slot = inv.slot(combo);
        if (def == nullptr || slot == nullptr) continue;
        if (def->ammo_class_count == 0) continue; // [orig: def[56] gate @ 0x5422ba]
        if (def->clipsize == -1) continue;        // [orig: def+88 != -1 @ 0x542304]
        int32_t units = def->ammo_class_count;
        const int32_t loaded = weapon_inventory_loaded_rounds(table, inv, combo);
        if (loaded != 0)
            weapon_pool_add(table, inv, def->ammo_class_id, loaded * units);
        int32_t clamped = static_cast<int32_t>(def->clipsize) * units;
        int32_t pool = weapon_pool_get(inv, def->ammo_class_id);
        if (clamped > pool) clamped = pool; // [orig: @ 0x542364]
        // [orig: WeaponSlot_DecrementAmmo @ 0x540920 call @ 0x542374 — the drawn
        //  amount never exceeds the pool by construction]
        if (def->ammo_class_id >= 0 &&
            def->ammo_class_id < static_cast<int>(inv.pools.size()))
            inv.pools[static_cast<size_t>(def->ammo_class_id)] -= clamped;
        weapon_inventory_set_loaded_rounds(table, inv, combo, clamped / units);
    }
}

int32_t weapon_slot_ammo_score(const WeaponTable &table, const WeaponInventory &inv,
                               int32_t combo) {
    return ammo_score(table, inv, combo);
}

int32_t weapon_inventory_reload_slot(const WeaponTable &table, WeaponInventory &inv,
                                     int32_t combo) {
    // [orig: WeaponSlot_ReloadAmmo @ 0x541720 (net-re §5.58) — refund the remaining
    //  clip into the def's ammo-class pool, then refill to clipsize clamped by the
    //  pool]
    const WeaponTableEntry *def = entry_at(table, inv, combo);
    WeaponInventorySlot *slot = inv.slot(combo);
    if (def == nullptr || slot == nullptr) return 0;
    const int32_t loaded = weapon_inventory_loaded_rounds(table, inv, combo);
    if (def->clipsize == -1 || def->ammo_class_count == 0) return loaded;
    int32_t units = def->ammo_class_count;
    if (loaded != 0)
        weapon_pool_add(table, inv, def->ammo_class_id, loaded * units);
    int32_t draw = static_cast<int32_t>(def->clipsize) * units;
    int32_t pool = weapon_pool_get(inv, def->ammo_class_id);
    if (draw > pool) draw = pool;
    if (def->ammo_class_id >= 0 &&
        def->ammo_class_id < static_cast<int>(inv.pools.size()))
        inv.pools[static_cast<size_t>(def->ammo_class_id)] -= draw;
    weapon_inventory_set_loaded_rounds(table, inv, combo, draw / units);
    return weapon_inventory_loaded_rounds(table, inv, combo);
}

bool weapon_select_slot(const WeaponTable &table, WeaponInventory &inv, int32_t combo,
                        bool commit_equip) {
    // [orig: Player_SelectWeaponSlot @ 0x4DD680]
    auto stage = [&](int32_t c) {
        inv.pending_combo = c; // [orig: entity+0x308 @ 0x4dd6da/0x4dd7a8]
        if (commit_equip) inv.equipped_combo = c; // [orig: EquippedSlot @ 0x4dd704,
                                                  //  gated on the seat state]
    };
    if (combo != -1) {
        int32_t group_base =
                weapon_combo::kRanksPerCategory * (combo / weapon_combo::kRanksPerCategory);
        // The exact slot is taken ONLY when its def carries the flags2&1 NoSelect bit
        // (parachute-style forced equips) [orig: @ 0x4dd6d8 requires the bit SET].
        const WeaponTableEntry *direct = entry_at(table, inv, combo);
        if (direct != nullptr && (direct->flags2 & 1) != 0) {
            stage(combo);
            return true;
        }
        // Group scan: first populated NON-NoSelect slot of the category
        // [orig: @ 0x4dd749..0x4dd76b].
        for (int32_t i = 0; i < weapon_combo::kRanksPerCategory; ++i) {
            int32_t c = group_base + i;
            const WeaponTableEntry *def = entry_at(table, inv, c);
            if (def != nullptr && (def->flags2 & 1) == 0) {
                stage(c);
                return true;
            }
        }
    }
    // Global scan [orig: @ 0x4dd76d..0x4dd794].
    for (int32_t c = 0; c < weapon_combo::kSlotCount; ++c) {
        const WeaponTableEntry *def = entry_at(table, inv, c);
        if (def != nullptr && (def->flags2 & 1) == 0) {
            stage(c);
            return true;
        }
    }
    return false;
}

namespace {

// Manual-switch eligibility [orig: Player_SwitchToWeaponByHandle @ 0x4e0294..0x4e02c3 —
// weapon_class 1/2 (primary/secondary) skip the ammo requirement; every candidate
// needs the NoSelect bit clear].
bool switch_eligible(const WeaponTable &table, const WeaponInventory &inv,
                     int32_t combo) {
    const WeaponTableEntry *def = entry_at(table, inv, combo);
    if (def == nullptr) return false;
    if ((def->flags2 & 1) != 0) return false;
    if (def->weapon_class_slot == 1 || def->weapon_class_slot == 2) return true;
    return ammo_score(table, inv, combo) != 0;
}

} // namespace

WeaponSwitchOutcome weapon_switch_to_handle(const WeaponTable &table,
                                            WeaponInventory &inv, int32_t handle,
                                            const WeaponSwitchGates &gates) {
    // [orig: Player_SwitchToWeaponByHandle @ 0x4E0170]
    WeaponSwitchOutcome out;
    if (gates.seat_blocked) return out; // [orig: parentSlot 2/3/5 @ 0x4e0192]
    if (handle < 0) return out;
    int32_t group = handle / weapon_combo::kRanksPerCategory;
    int32_t rank = handle % weapon_combo::kRanksPerCategory;
    (void)rank;
    const WeaponTableEntry *eq = entry_at(table, inv, inv.equipped_combo);
    if (eq != nullptr && gates.equipped_valid) {
        // The FSM gate: blocked while firing; reload blocks same-category presses;
        // recoil blocks cross-category presses [orig: @ 0x4e0192..0x4e0223].
        int32_t a = gates.equipped_action;
        bool blocked = (a == 4 && eq->category == group) || a == 2 ||
                       (a == 3 && eq->category != group);
        if (blocked) return out;
    }
    out.reset_view = true; // reset precedes selection, even on a later deny @0x4E0223
    if (eq == nullptr) {
        if (!weapon_select_slot(table, inv, handle, /*commit_equip=*/true) &&
            !weapon_select_slot(table, inv, -1, /*commit_equip=*/true))
            return out; // [orig: @ 0x4e0223 both selects failed]
        eq = entry_at(table, inv, inv.equipped_combo);
        if (eq == nullptr) return out;
    }
    int32_t current_rank;
    if (group == eq->category) {
        current_rank = eq->rank; // [orig: @ 0x4e0273 — same-category press rank-cycles]
    } else {
        // Direct exact-slot check first [orig: @ 0x4e027d..0x4e02c9].
        if (switch_eligible(table, inv, handle)) {
            out.kind = WeaponSwitchOutcome::kMount;
            out.combo = handle;
            out.same_category = false;
            inv.pending_combo = handle; // [orig: MountWeaponSlot g_PendingWeaponSlot]
            return out;
        }
        current_rank = 0;
    }
    int32_t start_rank = current_rank;
    while (true) {
        current_rank = (current_rank + 1) % weapon_combo::kRanksPerCategory;
        int32_t c = group * weapon_combo::kRanksPerCategory + current_rank;
        if (switch_eligible(table, inv, c)) {
            out.kind = WeaponSwitchOutcome::kMount;
            out.combo = c;
            out.same_category = (eq->category == group);
            inv.pending_combo = c;
            return out;
        }
        if (current_rank == start_rank) {
            out.kind = WeaponSwitchOutcome::kDeny; // [orig: deny sound @ 0x4e0354]
            return out;
        }
    }
}

WeaponSwitchOutcome weapon_cycle_slot(const WeaponTable &table, WeaponInventory &inv,
                                      int32_t direction,
                                      const WeaponSwitchGates &gates) {
    // [orig: Player_CycleWeaponSlot @ 0x4DFE70]
    WeaponSwitchOutcome out;
    if (gates.seat_blocked) return out; // [orig: parentSlot 2/3/5 @ 0x4dfe91]
    const WeaponTableEntry *eq = entry_at(table, inv, inv.equipped_combo);
    if (eq == nullptr || !gates.equipped_valid) return out; // [orig: @ 0x4dfe9f]
    int32_t step;
    if (direction > 0) step = 1;
    else if (direction < 0) step = -1;
    else return out;
    int32_t start = eq->rank + weapon_combo::kRanksPerCategory * eq->category;
    int32_t idx = start;
    // Bounded to one full wrap plus the start revisit — the original's walk has no
    // bound and relies on the start slot terminating it; a NoSelect start would spin
    // it forever, which shipped data never produces (guard, not behavior).
    for (int32_t steps = 0; steps <= weapon_combo::kSlotCount; ++steps) {
        idx += step;
        if (idx >= weapon_combo::kSlotCount) idx = 0;         // [orig: @ 0x4dff00]
        else if (idx < 0) idx = weapon_combo::kSlotCount - 1; // [orig: @ 0x4dff08]
        const WeaponTableEntry *def = entry_at(table, inv, idx);
        bool candidate = false;
        if (def != nullptr) {
            if ((def->flags2 & 1) == 0) candidate = true; // [orig: @ 0x4dff31]
            else continue; // NoSelect: keep scanning [orig: inner-loop continue]
        } else if (idx == start) {
            candidate = true; // [orig: empty-slot wrap check @ 0x4dff21]
        }
        if (!candidate) continue;
        // EVERY cycle candidate needs the ammo score — no weapon_class exemption
        // [orig: Score_CalculateKillScore gate @ 0x4dff39].
        if (ammo_score(table, inv, idx) != 0) {
            if (idx != start) {
                out.kind = WeaponSwitchOutcome::kMount;
                out.combo = idx;
                out.same_category =
                        (def != nullptr && def->category == eq->category);
                inv.pending_combo = idx;
            }
            return out; // reaching start again returns silently [orig: @ 0x4dff47]
        }
    }
    return out;
}


std::array<WeaponSlotBarCategory, 10> weapon_inventory_slot_bar_scan(const WeaponTable &table,
                                                                     const WeaponInventory &inv) {
    // [orig: HUD_DrawWeaponSlotBar @0x599D0A..0x599D69 — `entry_offset` 0..650
    //  step 65 (one category), the 1..5 skip @0x599D24, the def pointer
    //  slot+0x20 @0x599D36, the first-hit record + `i += def[940/4]`
    //  @0x599D40..0x599D4D, the count @0x599D51]. The debug fill under
    //  dword_24C1930 & 0x8000000 @0x599D75 is dead: nothing sets the bit.
    std::array<WeaponSlotBarCategory, 10> out{};
    for (int category = 0; category < 10; ++category) {
        if (category >= 1 && category <= 5) continue;
        WeaponSlotBarCategory &cat = out[static_cast<size_t>(category)];
        for (int i = 0; i < weapon_combo::kRanksPerCategory; ++i) {
            const int32_t combo = category * weapon_combo::kRanksPerCategory + i;
            const WeaponTableEntry *def = entry_at(table, inv, combo);
            if (def == nullptr) continue;
            if (cat.adm_index < 0) {
                cat.adm_index = inv.slot(combo)->adm_index;
                cat.def_category = def->category; // the label's def[0] [orig: @0x599e8f]
                i += def->loadout_subclasses;
            }
            ++cat.count;
        }
    }
    return out;
}

} // namespace opennova::world
