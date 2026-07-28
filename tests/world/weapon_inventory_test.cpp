// Weapon inventory / loadout-rules unit tests [orig: the 2026-07-18 loadout grill —
// Player_InitPlayer @ 0x4e15f0 spawn chain, WeaponSlotTable_LoadAllFromDefs @ 0x5414e0,
// WeaponSlots_SeedAmmoPoolsFromDefs @ 0x541690, WeaponSlots_RecalculateAmmoFromCapacity
// @ 0x542280, Player_SelectWeaponSlot @ 0x4dd680, Player_SwitchToWeaponByHandle
// @ 0x4e0170, Player_CycleWeaponSlot @ 0x4dfe70, Mission_LoadBMSFile loadout filter
// @ 0x40f7ae, build_item_restriction_table @ 0x54ddb0].
#include <cstdio>
#include <cstring>

#include "world/weapon_inventory.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

int add_entry(WeaponTable &t, const char *name, int cat, int rank, int clipsize,
              int startrounds, const char *ammo_class, int units, int wclass,
              int flags2 = 0, int subclasses = 0) {
    WeaponTableEntry e;
    e.name = name;
    e.valid = true;
    e.category = static_cast<uint8_t>(cat);
    e.rank = static_cast<uint8_t>(rank);
    e.clipsize = static_cast<int16_t>(clipsize);
    e.startrounds = static_cast<int16_t>(startrounds);
    e.maxclips = 10;
    e.ammo_class = ammo_class;
    e.ammo_class_count = static_cast<int16_t>(units);
    e.weapon_class_slot = wclass;
    e.flags2 = flags2;
    e.loadout_subclasses = static_cast<uint8_t>(subclasses);
    int id = t.ammo_class_id_of(ammo_class);
    if (id < 0) {
        t.ammo_class_names.emplace_back(ammo_class);
        t.ammo_class_caps.push_back(600);
        id = static_cast<int>(t.ammo_class_names.size()) - 1;
    }
    e.ammo_class_id = static_cast<int16_t>(id);
    t.entries.push_back(std::move(e));
    return static_cast<int>(t.entries.size()) - 1;
}

// A small armory: null@0, knife@1 (cat1, no clip), pistol@2 (cat2, secondary),
// m4@3 (cat3 rank0, primary), m203 sub@4 (m4's sub-variant), ak@5 (cat3 rank1,
// primary), frag@6 (cat5, grenade — needs ammo), parachute@7 (NoSelect, cat10).
struct Fixture {
    WeaponTable t;
    int knife, pistol, m4, m203, ak, frag, chute;
    Fixture() {
        t.ammo_class_names.emplace_back(""); // id 0 = the classless byte-0 pool
        t.ammo_class_caps.push_back(0);
        WeaponTableEntry null_e;
        null_e.name = "null";
        null_e.valid = true;
        null_e.ammo_class_id = 0;
        t.entries.push_back(std::move(null_e));
        knife = add_entry(t, "WPN_KNIFE", 1, 0, -1, -1, "", 0, 0);
        t.entries.back().ammo_class_id = 0;
        pistol = add_entry(t, "WPN_PISTOL", 2, 0, 15, 45, "AMMO_9MM", 1, 2);
        m4 = add_entry(t, "WPN_M4AUTO", 3, 0, 30, 210, "AMMO_556", 1, 1, 0, 1);
        m203 = add_entry(t, "WPN_M203HE", 3, 40, 1, 3, "AMMO_40MM", 1, 0);
        ak = add_entry(t, "WPN_AK47AUTO", 3, 1, 30, 210, "AMMO_762", 1, 1);
        frag = add_entry(t, "WPN_GRENADEFB", 5, 0, 1, 2, "AMMO_FRAG", 1, 3);
        chute = add_entry(t, "WPN_PARACHUTE", 10, 0, -1, -1, "", 0, 0, /*flags2=*/1);
        t.entries[static_cast<size_t>(chute)].ammo_class_id = 0;
        // gunner classrounds override on the M4 [orig: 'classrounds gunner N'
        // -> index 3 @ 0x543ab0]
        t.entries[static_cast<size_t>(m4)].classrounds[3] = 90;
    }
};

void test_availability_pairs() {
    Fixture f;
    WeaponAvailability avail;
    // {name, value} pairs — the .mis item_availability shape; -1 maps to 3 and
    // sub-entries inherit the parent's value [orig: @ 0x54de47 / the skip walk].
    std::vector<std::pair<std::string, int32_t>> pairs = {
            {"wpn_m4auto", 0},        // banned (case-insensitive match)
            {"WPN_GRENADEFB", 2},     // armory-zone-only
            {"WPN_AK47AUTO", -1},     // -> 3 mission-allowed
    };
    weapon_availability_apply_pairs(avail, f.t, pairs);
    CHECK(avail.value_for(f.m4) == 0);
    CHECK(avail.value_for(f.m203) == 0);          // the m4's sub-entry inherits
    CHECK(avail.value_for(f.frag) == 2);
    CHECK(avail.value_for(f.ak) == 3);
    CHECK(avail.value_for(f.knife) == 1);         // untouched default
    CHECK(avail.value_for(f.pistol) == 1);
}

void test_kit_filter() {
    Fixture f;
    WeaponAvailability avail;
    avail.values[static_cast<size_t>(f.m4)] = 0;
    std::vector<WeaponKitEntry> kit = {
            {"WPN_M4AUTO", -1, -1, -1},
            {"WPN_NOSUCH", -1, -1, -1}, // unresolved drops silently [orig: @ 0x40f830]
            {"WPN_PISTOL", 3, -1, -1},
    };
    auto filtered = weapon_kit_filter_by_availability(kit, f.t, avail);
    CHECK(filtered.size() == 1);
    CHECK(filtered[0].name == "WPN_PISTOL");
    CHECK(filtered[0].ammo_primary == 3);
    // Everything filtered -> the knife fallback [orig: @ 0x40f899].
    avail.values[static_cast<size_t>(f.pistol)] = 0;
    auto fallback = weapon_kit_filter_by_availability(
            {{"WPN_M4AUTO", -1, -1, -1}, {"WPN_PISTOL", -1, -1, -1}}, f.t, avail);
    CHECK(fallback.size() == 1);
    CHECK(fallback[0].name == "WPN_KNIFE");
}

void test_display_expand_and_fill() {
    Fixture f;
    std::vector<WeaponKitEntry> kit = {
            {"WPN_KNIFE", -1, -1, -1},
            {"WPN_M4AUTO", -1, -1, -1}, // expands its 1 sub-variant (the M203)
            {"WPN_GRENADEFB", -1, -1, -1},
    };
    auto display = weapon_kit_expand_display_list(kit, f.t);
    CHECK(display.size() == 4);
    CHECK(display[1] == "WPN_M4AUTO");
    CHECK(display[2] == "WPN_M203HE"); // [orig: sub append @ 0x54bb07]

    WeaponInventory inv;
    inv.reset(f.t);
    auto fill = weapon_inventory_load_from_display(f.t, display, inv);
    CHECK(fill.warnings.empty());
    CHECK(inv.slot(1 * 65 + 0)->adm_index == f.knife);
    CHECK(inv.slot(3 * 65 + 0)->adm_index == f.m4);
    CHECK(inv.slot(3 * 65 + 40)->adm_index == f.m203);
    CHECK(inv.slot(5 * 65 + 0)->adm_index == f.frag);
    CHECK(inv.slot(2 * 65 + 0)->adm_index == -1); // no pistol in this kit

    // A different def landing on an occupied combo warns and keeps the incumbent
    // [orig: the "overloading" branch @ 0x5415d6].
    WeaponTable t2 = f.t;
    t2.entries[static_cast<size_t>(f.ak)].rank = 0; // collide with the M4 combo
    WeaponInventory inv2;
    inv2.reset(t2);
    auto fill2 = weapon_inventory_load_from_display(
            t2, {"WPN_M4AUTO", "WPN_AK47AUTO"}, inv2);
    CHECK(fill2.warnings.size() == 1);
    CHECK(inv2.slot(3 * 65 + 0)->adm_index == f.m4);
    // Unresolved display names warn [orig: "couldn't find wpn index"].
    auto fill3 = weapon_inventory_load_from_display(f.t, {"WPN_GHOST"}, inv2);
    CHECK(fill3.warnings.size() == 1);
}

void test_pools_and_recalc() {
    Fixture f;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(
            f.t, {"WPN_KNIFE", "WPN_M4AUTO", "WPN_GRENADEFB", "WPN_PISTOL"}, inv);

    // Rifleman: plain startrounds seed [orig: pools[class] = def+0x5C @ 0x5416b9].
    weapon_inventory_seed_pools(f.t, inv, 8);
    int m4_class = f.t.entries[static_cast<size_t>(f.m4)].ammo_class_id;
    int frag_class = f.t.entries[static_cast<size_t>(f.frag)].ammo_class_id;
    CHECK(weapon_pool_get(inv, m4_class) == 210);
    CHECK(weapon_pool_get(inv, frag_class) == 2);

    // Gunner: the classrounds override wins [orig: @ 0x5416cb..0x5416e6].
    weapon_inventory_seed_pools(f.t, inv, 7);
    CHECK(weapon_pool_get(inv, m4_class) == 90);

    // Recalc draws one clip from the pool [orig: @ 0x542354..0x54239e].
    weapon_inventory_seed_pools(f.t, inv, 8);
    weapon_inventory_recalc_clips(f.t, inv);
    CHECK(inv.slot(3 * 65 + 0)->clip == 30);
    CHECK(weapon_pool_get(inv, m4_class) == 180);
    CHECK(inv.slot(5 * 65 + 0)->clip == 1);
    CHECK(weapon_pool_get(inv, frag_class) == 1);
    // The knife (clipsize -1 / units 0) is untouched and its classless pool
    // keeps the -1 seed — which is exactly what keeps it switch-eligible
    // [orig: nonzero kill-score test @ 0x4e02c3].
    CHECK(inv.slot(1 * 65 + 0)->clip == 0);
    CHECK(weapon_slot_ammo_score(f.t, inv, 1 * 65 + 0) == -1);

    // Reload: refund + redraw [orig: WeaponSlot_ReloadAmmo §5.58].
    inv.slot(3 * 65 + 0)->clip = 5;
    int32_t loaded = weapon_inventory_reload_slot(f.t, inv, 3 * 65 + 0);
    CHECK(loaded == 30);
    CHECK(weapon_pool_get(inv, m4_class) == 155); // 180 + 5 - 30

    // The carry cap clamps pool adds [orig: @ 0x540b26].
    weapon_pool_add(f.t, inv, m4_class, 100000);
    CHECK(weapon_pool_get(inv, m4_class) == 600);
}

void test_select() {
    Fixture f;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(
            f.t, {"WPN_KNIFE", "WPN_M4AUTO", "WPN_AK47AUTO", "WPN_PARACHUTE"}, inv);

    // Category-level select: combo 195 stages the first populated normal slot of
    // category 3 [orig: the group scan @ 0x4dd749].
    CHECK(weapon_select_slot(f.t, inv, 195, true));
    CHECK(inv.equipped_combo == 195);
    // Rank 1 requested -> still the first normal slot (rank 0), NOT the exact rank.
    CHECK(weapon_select_slot(f.t, inv, 196, true));
    CHECK(inv.equipped_combo == 195);
    // The exact leg fires only for NoSelect defs (the parachute) [orig: @ 0x4dd6d8].
    CHECK(weapon_select_slot(f.t, inv, 10 * 65 + 0, true));
    CHECK(inv.equipped_combo == 10 * 65 + 0);
    // Empty category falls through to the global scan (first normal slot overall).
    CHECK(weapon_select_slot(f.t, inv, 7 * 65 + 0, true));
    CHECK(inv.equipped_combo == 1 * 65 + 0);
    // -1 = the global scan directly [orig: @ 0x4dd76d].
    CHECK(weapon_select_slot(f.t, inv, -1, true));
    CHECK(inv.equipped_combo == 1 * 65 + 0);
    // Seat-deferred commit stages pending only [orig: the parentSlot 2/3 gate].
    // No pistol is loaded here, so category 2 falls through to the global scan
    // and stages the knife.
    inv.equipped_combo = 195;
    CHECK(weapon_select_slot(f.t, inv, 130, false));
    CHECK(inv.pending_combo == 1 * 65 + 0);
    CHECK(inv.equipped_combo == 195);
    // An empty inventory selects nothing.
    WeaponInventory empty;
    empty.reset(f.t);
    CHECK(!weapon_select_slot(f.t, empty, -1, true));
}

void test_switch_walks() {
    Fixture f;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(
            f.t,
            {"WPN_KNIFE", "WPN_PISTOL", "WPN_M4AUTO", "WPN_AK47AUTO", "WPN_GRENADEFB"},
            inv);
    weapon_inventory_seed_pools(f.t, inv, 8);
    weapon_inventory_recalc_clips(f.t, inv);
    inv.equipped_combo = 195; // the M4
    WeaponSwitchGates gates;
    gates.equipped_valid = true;
    gates.equipped_action = 0; // idle

    // Cross-category: the exact requested slot mounts [orig: @ 0x4e027d].
    auto r = weapon_switch_to_handle(f.t, inv, 2 * 65, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kMount);
    CHECK(r.combo == 2 * 65);
    CHECK(!r.same_category);
    CHECK(inv.pending_combo == 2 * 65);

    // Same-category press rank-cycles M4 -> AK [orig: @ 0x4e0273 + the wrap loop].
    r = weapon_switch_to_handle(f.t, inv, 195, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kMount);
    CHECK(r.combo == 196);
    CHECK(r.same_category);

    // Empty category: deny [orig: the deny sound @ 0x4e0354].
    r = weapon_switch_to_handle(f.t, inv, 7 * 65, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kDeny);

    // The grenade needs ammo (weapon_class 3): drain its pool + clip -> its
    // category denies; primaries stay eligible with empty pools
    // [orig: the class 1/2 exemption @ 0x4e02c3].
    int frag_class = f.t.entries[static_cast<size_t>(f.frag)].ammo_class_id;
    inv.slot(5 * 65 + 0)->clip = 0;
    inv.pools[static_cast<size_t>(frag_class)] = 0;
    r = weapon_switch_to_handle(f.t, inv, 5 * 65, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kDeny);
    int m4_class = f.t.entries[static_cast<size_t>(f.m4)].ammo_class_id;
    inv.slot(195)->clip = 0;
    inv.pools[static_cast<size_t>(m4_class)] = 0;
    r = weapon_switch_to_handle(f.t, inv, 2 * 65, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kMount); // pistol: class 2, no ammo needed

    // FSM gates [orig: @ 0x4e0192..0x4e0223]: firing blocks everything; reload
    // blocks only the equipped category; recoil blocks only cross-category.
    gates.equipped_action = 2;
    CHECK(weapon_switch_to_handle(f.t, inv, 2 * 65, gates).kind ==
          WeaponSwitchOutcome::kNone);
    gates.equipped_action = 4;
    CHECK(weapon_switch_to_handle(f.t, inv, 195, gates).kind ==
          WeaponSwitchOutcome::kNone);
    CHECK(weapon_switch_to_handle(f.t, inv, 2 * 65, gates).kind ==
          WeaponSwitchOutcome::kMount);
    gates.equipped_action = 3;
    CHECK(weapon_switch_to_handle(f.t, inv, 2 * 65, gates).kind ==
          WeaponSwitchOutcome::kNone);
    CHECK(weapon_switch_to_handle(f.t, inv, 195, gates).kind ==
          WeaponSwitchOutcome::kMount);
    gates.equipped_action = 0;

    // Seat gate [orig: parentSlot 2/3/5 @ 0x4e0192].
    gates.seat_blocked = true;
    CHECK(weapon_switch_to_handle(f.t, inv, 2 * 65, gates).kind ==
          WeaponSwitchOutcome::kNone);
    gates.seat_blocked = false;

    // NoSelect never mounts via the manual walk [orig: the flags2&1 term].
    weapon_inventory_load_from_display(f.t, {"WPN_PARACHUTE"}, inv);
    r = weapon_switch_to_handle(f.t, inv, 10 * 65, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kDeny);

    // No equipped slot: the select fallback stages one first [orig: @ 0x4e0223].
    WeaponInventory inv2;
    inv2.reset(f.t);
    weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO"}, inv2);
    weapon_inventory_seed_pools(f.t, inv2, 8);
    weapon_inventory_recalc_clips(f.t, inv2);
    inv2.equipped_combo = -1;
    WeaponSwitchGates fresh;
    fresh.equipped_valid = false;
    r = weapon_switch_to_handle(f.t, inv2, 195, fresh);
    CHECK(inv2.equipped_combo == 195);
    CHECK(r.kind == WeaponSwitchOutcome::kMount);
    CHECK(r.combo == 195); // the wrap revisits and remounts the eligible equipped rank
}

void test_cycle() {
    Fixture f;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(
            f.t, {"WPN_KNIFE", "WPN_PISTOL", "WPN_M4AUTO", "WPN_AK47AUTO"}, inv);
    weapon_inventory_seed_pools(f.t, inv, 8);
    weapon_inventory_recalc_clips(f.t, inv);
    inv.equipped_combo = 130; // the pistol
    WeaponSwitchGates gates;
    gates.equipped_valid = true;

    // Next: first slot upward with a nonzero ammo score [orig: @ 0x4dff39].
    auto r = weapon_cycle_slot(f.t, inv, +1, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kMount);
    CHECK(r.combo == 195);
    // Prev wraps downward to the knife (-1 pool score counts as nonzero).
    r = weapon_cycle_slot(f.t, inv, -1, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kMount);
    CHECK(r.combo == 65);
    // An ammo-less M4 is SKIPPED by cycle (no class exemption here)
    // [orig: every candidate runs the kill-score gate @ 0x4dff39].
    int m4_class = f.t.entries[static_cast<size_t>(f.m4)].ammo_class_id;
    inv.slot(195)->clip = 0;
    inv.pools[static_cast<size_t>(m4_class)] = 0;
    r = weapon_cycle_slot(f.t, inv, +1, gates);
    CHECK(r.combo == 196); // the AK (its own class pool still holds rounds)
    // Direction 0 is a no-op; a lone-weapon wrap returns silently (no deny).
    CHECK(weapon_cycle_slot(f.t, inv, 0, gates).kind == WeaponSwitchOutcome::kNone);
    WeaponInventory lone;
    lone.reset(f.t);
    weapon_inventory_load_from_display(f.t, {"WPN_PISTOL"}, lone);
    weapon_inventory_seed_pools(f.t, lone, 8);
    weapon_inventory_recalc_clips(f.t, lone);
    lone.equipped_combo = 130;
    CHECK(weapon_cycle_slot(f.t, lone, +1, gates).kind == WeaponSwitchOutcome::kNone);
}

void test_defaults() {
    auto def = weapon_kit_default();
    CHECK(def.size() == 1 && def[0].name == "WPN_M4AUTO");
    auto knife = weapon_kit_knife_fallback();
    CHECK(knife.size() == 1 && knife[0].name == "WPN_KNIFE");
    CHECK(weapon_combo::kDefaultSpawnCombo == 195);
}

void test_kit_damage_classes() {
    Fixture f;
    f.t.entries[static_cast<size_t>(f.pistol)].ammo_index = 2;
    f.t.entries[static_cast<size_t>(f.m4)].ammo_index = 4;
    f.t.entries[static_cast<size_t>(f.ak)].ammo_index = 7;
    std::vector<uint8_t> classes(10, 99);
    weapon_kit_build_damage_classes(
            {{"WPN_M4AUTO", -1, -1, 1}, {"WPN_PISTOL", -1, -1, 2},
             {"WPN_NOSUCH", -1, -1, 2}},
            f.t, 10, classes);
    CHECK(classes.size() == 10);
    CHECK(classes[4] == 1 && classes[2] == 2);
    CHECK(classes[7] == 0 && classes[9] == 0);

    // A new accepted kit clears values from the previous one before stamping.
    weapon_kit_build_damage_classes({{"WPN_AK47AUTO", -1, -1, 2}},
                                    f.t, 10, classes);
    CHECK(classes[4] == 0 && classes[2] == 0 && classes[7] == 2);
}

} // namespace


// REPRO (2026-07-26, reported live): a joiner granted a knife by the host cannot
// equip it. WPN_KNIFE authors no `weapon_class` and no `ammoclass` line at all, so
// weapon_class_slot is 0 and its ammo pool is the classless byte-0 bucket, and it is
// a no-clip weapon (clipsize -1). Retail's manual-switch predicate is
// `weapon_class == 1 || weapon_class == 2 || calculate_kill_score(slot, entity, 0, 0)`
// [orig: Player_SwitchToWeaponByHandle @0x4e0294..0x4e02c3], so the knife rides
// entirely on that score term.
static int test_knife_is_selectable() {
	Fixture f;
	WeaponInventory inv;
	inv.reset(f.t);
	weapon_inventory_load_from_display(f.t, {"WPN_KNIFE", "WPN_M4AUTO"}, inv);
	weapon_inventory_seed_pools(f.t, inv, 8);
	weapon_inventory_recalc_clips(f.t, inv);
	inv.equipped_combo = 3 * 65; // holding the rifle
	WeaponSwitchGates gates;
	gates.equipped_valid = true;
	// The knife occupies category 1 rank 0.
	CHECK(inv.slot(1 * 65) != nullptr);
	CHECK(inv.slot(1 * 65)->adm_index == f.knife);
	const WeaponSwitchOutcome r = weapon_switch_to_handle(f.t, inv, 1 * 65, gates);
	CHECK(r.kind == WeaponSwitchOutcome::kMount);
	CHECK(r.combo == 1 * 65);
	std::printf("PASS knife is manually selectable\n");
	return 0;
}

int main() {
    test_knife_is_selectable();
    test_availability_pairs();
    test_kit_filter();
    test_display_expand_and_fill();
    test_pools_and_recalc();
    test_select();
    test_switch_walks();
    test_defaults();
    test_kit_damage_classes();
    test_cycle();
    if (failures == 0) std::printf("weapon_inventory_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
