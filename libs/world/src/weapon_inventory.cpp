// Structural translations of the witnessed loadout/switching originals — see
// weapon_inventory.h for the model overview and docs/net/novaworld-net-re.md
// §5.57/§5.58 + the 2026-07-18 loadout grill for the witness record.
#include "world/weapon_inventory.h"

#include <cstdio>

#include "io/strutil.h"

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
// [orig: calculate_kill_score @ 0x5407E0 as the slot predicate — nonzero selects].
int32_t ammo_score(const WeaponTable &table, const WeaponInventory &inv, int32_t combo) {
    const WeaponTableEntry *def = entry_at(table, inv, combo);
    const WeaponInventorySlot *s = inv.slot(combo);
    if (def == nullptr || s == nullptr) return 0;
    // The def+0xDC pass-type branch (shared-pool "clip" reads via sub_5405F0) is
    // deferred with the recalc pass leg (D-WPN-20, docs/divergence-ledger.md).
    return weapon_pool_get(inv, def->ammo_class_id) + s->clip;
}

} // namespace

void weapon_availability_apply_pairs(
        WeaponAvailability &avail, const WeaponTable &table,
        const std::vector<std::pair<std::string, int32_t>> &pairs) {
    // The name-list mode of build_item_restriction_table @ 0x54DDB0: default 1,
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
    // [orig: Buffer_CopyUntilDoubleNull(restrictionData, "WPN_M4AUTO", 2048)
    //  @ 0x5246be / @ 0x5519e4 — the profile-less spawn kit]
    return {WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
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
    //  entity+288, others g_localAmmoPools[class]; one array here (see header)]
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

WeaponFillResult weapon_inventory_load_from_display(
        const WeaponTable &table, const std::vector<std::string> &display,
        WeaponInventory &inv) {
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
        // [orig: WeaponSlot_InitFromDef @ 0x53EE70 — fresh slot state, clip 0]
        slot->adm_index = static_cast<int16_t>(adm);
        slot->clip = 0;
    }
    return result;
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

void weapon_inventory_recalc_clips(const WeaponTable &table, WeaponInventory &inv) {
    // [orig: WeaponSlots_RecalculateAmmoFromCapacity @ 0x542280 — return the clip to
    //  the pool, then draw one full clip clamped by what the pool affords. The
    //  arithmetic is ported literally, including the negative-pool degeneration the
    //  record notes for shipped -1 startrounds (§5.57 "the v15 ammo issues"). The
    //  def+0xDC pass-type leg is deferred (D-WPN-20).]
    for (int32_t combo = 0; combo < weapon_combo::kSlotCount; ++combo) {
        const WeaponTableEntry *def = entry_at(table, inv, combo);
        WeaponInventorySlot *slot = inv.slot(combo);
        if (def == nullptr || slot == nullptr) continue;
        if (def->ammo_class_count == 0) continue; // [orig: def[56] gate @ 0x5422ba]
        if (def->clipsize == -1) continue;        // [orig: def+88 != -1 @ 0x542304]
        int32_t units = def->ammo_class_count;
        if (slot->clip != 0)
            weapon_pool_add(table, inv, def->ammo_class_id, slot->clip * units);
        int32_t clamped = static_cast<int32_t>(def->clipsize) * units;
        int32_t pool = weapon_pool_get(inv, def->ammo_class_id);
        if (clamped > pool) clamped = pool; // [orig: @ 0x542364]
        // [orig: WeaponSlot_DecrementAmmo @ 0x540920 call @ 0x542374 — the drawn
        //  amount never exceeds the pool by construction]
        if (def->ammo_class_id >= 0 &&
            def->ammo_class_id < static_cast<int>(inv.pools.size()))
            inv.pools[static_cast<size_t>(def->ammo_class_id)] -= clamped;
        slot->clip = clamped / units;
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
    if (def->clipsize == -1 || def->ammo_class_count == 0) return slot->clip;
    int32_t units = def->ammo_class_count;
    if (slot->clip != 0)
        weapon_pool_add(table, inv, def->ammo_class_id, slot->clip * units);
    int32_t draw = static_cast<int32_t>(def->clipsize) * units;
    int32_t pool = weapon_pool_get(inv, def->ammo_class_id);
    if (draw > pool) draw = pool;
    if (def->ammo_class_id >= 0 &&
        def->ammo_class_id < static_cast<int>(inv.pools.size()))
        inv.pools[static_cast<size_t>(def->ammo_class_id)] -= draw;
    slot->clip = draw / units;
    return slot->clip;
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
    // (The original zeroes g_fireChargeStartTick and resets the camera here — sim-side
    //  presentation, handled by the caller on a mount outcome.)
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
            inv.pending_combo = handle; // [orig: MountWeaponSlot g_pendingWeaponSlot]
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
        // [orig: calculate_kill_score gate @ 0x4dff39].
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

} // namespace opennova::world
