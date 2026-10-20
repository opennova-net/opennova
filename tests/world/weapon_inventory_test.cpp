// Weapon inventory / loadout-rules unit tests [orig: the 2026-07-18 loadout grill —
// Player_InitPlayer @ 0x4e15f0 spawn chain, WeaponSlotTable_LoadAllFromDefs @ 0x5414e0,
// WeaponSlots_SeedAmmoPoolsFromDefs @ 0x541690, WeaponSlots_RecalculateAmmoFromCapacity
// @ 0x542280, Player_SelectWeaponSlot @ 0x4dd680, Player_SwitchToWeaponByHandle
// @ 0x4e0170, Player_CycleWeaponSlot @ 0x4dfe70, Mission_LoadBMSFile loadout filter
// @ 0x40f7ae, WeaponDef_BuildItemRestrictionTable @ 0x54ddb0].
#include <cstdio>
#include <cstring>

#include <formats/def/def.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

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

    // A sub-variant index past 254 wraps to the table's start, a byte add; the
    // 255-slot table holds no index 255, and the wrapped "null" row (entry 0) is
    // listed but names no weapon, so the fill skips it with the loader's warning.
    // [orig: AvatarDef_BuildDisplayList @ 0x54bae6..0x54bae9; AdmDef_GetEntryByIndex
    //  @ 0x53fc84 (0xFF is none); AdmDef_FindIndexByName @ 0x53fd85..0x53fd97;
    //  WeaponSlotTable_LoadAllFromDefs @ 0x541532 / @ 0x541553]
    {
        WeaponTable wrap;
        wrap.entries.resize(255);
        auto def = [&](size_t i, const char *name) -> WeaponTableEntry & {
            wrap.entries[i].name = name;
            wrap.entries[i].valid = true;
            return wrap.entries[i];
        };
        WeaponTableEntry &edge = def(254, "WPN_EDGE");
        edge.loadout_subclasses = 3;
        edge.category = 2;
        def(0, "null");
        WeaponTableEntry &sub = def(1, "WPN_SUB1");
        sub.category = 3;
        sub.rank = 1;
        const auto wrapped =
                weapon_kit_expand_display_list({{"WPN_EDGE", -1, -1, -1}}, wrap);
        CHECK(wrapped.size() == 3);
        CHECK(wrapped.size() == 3 && wrapped[1] == "null" && wrapped[2] == "WPN_SUB1");
        CHECK(wrap.index_of("null") == -1 && wrap.index_of("NULL") == -1 &&
                wrap.index_of("WPN_SUB1") == 1);
        WeaponInventory winv;
        winv.reset(wrap);
        const auto wfill = weapon_inventory_load_from_display(wrap, wrapped, winv, 8, false);
        CHECK(winv.slot(0)->adm_index == -1);
        CHECK(winv.slot(3 * 65 + 1)->adm_index == 1);
        CHECK(wfill.warnings.size() == 1);
    }

    WeaponInventory inv;
    inv.reset(f.t);
    auto fill = weapon_inventory_load_from_display(f.t, display, inv, 8, false);
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
            t2, {"WPN_M4AUTO", "WPN_AK47AUTO"}, inv2, 8, false);
    CHECK(fill2.warnings.size() == 1);
    CHECK(inv2.slot(3 * 65 + 0)->adm_index == f.m4);
    // Unresolved display names warn [orig: "couldn't find wpn index"].
    auto fill3 = weapon_inventory_load_from_display(f.t, {"WPN_GHOST"}, inv2, 8, false);
    CHECK(fill3.warnings.size() == 1);
}

void test_pools_and_recalc() {
    Fixture f;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(
            f.t, {"WPN_KNIFE", "WPN_M4AUTO", "WPN_GRENADEFB", "WPN_PISTOL"}, inv, 8, false);

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
    int32_t loaded = weapon_inventory_reload_slot(f.t, inv, 3 * 65 + 0, WeaponAmmoSeat{});
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
            f.t, {"WPN_KNIFE", "WPN_M4AUTO", "WPN_AK47AUTO", "WPN_PARACHUTE"}, inv, 8, false);

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
    // Seated, the select stores entity+0x308 alone [orig: the parentSlot 2/3 gate
    // @0x4dd6fc; the global scan's +0x308 store @0x4dd81c]. No pistol is loaded here, so
    // category 2 falls through to the global scan and selects the knife; the
    // pending slot (a mount's) is untouched.
    inv.equipped_combo = 195;
    inv.pending_combo = -1;
    CHECK(weapon_select_slot(f.t, inv, 130, false));
    CHECK(inv.selected_combo == 1 * 65 + 0);
    CHECK(inv.pending_combo == -1);
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
            inv, 8, false);
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
    weapon_inventory_load_from_display(f.t, {"WPN_PARACHUTE"}, inv, 8, false);
    r = weapon_switch_to_handle(f.t, inv, 10 * 65, gates);
    CHECK(r.kind == WeaponSwitchOutcome::kDeny);

    // No equipped slot: the select fallback stages one first [orig: @ 0x4e0223].
    WeaponInventory inv2;
    inv2.reset(f.t);
    weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO"}, inv2, 8, false);
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

// Drive the request, outgoing action, commit event, and target install as the
// live player does. SWITCHRANK changes modes in place; only SWITCHFROM draws
// the target. [orig: WeaponAction_SwitchRank @0x543500 vs SwitchFrom @0x5433b0]
void test_committed_switch_only_draws_after_holstering() {
    for (const bool same_category : {true, false}) {
        Fixture f;
        World world;
        world.tables.weapons = f.t;
        WeaponInventory inv;
        inv.reset(f.t);
        weapon_inventory_load_from_display(f.t,
                {"WPN_M4AUTO", "WPN_AK47AUTO", "WPN_PISTOL"}, inv, 8, false);
        weapon_inventory_seed_pools(f.t, inv, 8);
        weapon_inventory_recalc_clips(f.t, inv);
        inv.equipped_combo = 3 * 65;
        world.registry.configure_pool(0, 1);
        Entity seed;
        seed.alive = true;
        seed.health = 100;
        world.cached.local_player = world.registry.spawn(0, seed);

        WeaponInstallData data;
        data.name = "WPN_M4AUTO";
        data.clipsize = 30;
        data.rows.resize(2);
        std::snprintf(data.rows[0].name, sizeof(data.rows[0].name), "switchrank");
        std::snprintf(data.rows[1].name, sizeof(data.rows[1].name), "switchfrom");
        for (WeaponFsmActionRow &row : data.rows) {
            row.delaystart = 1;
            row.delayend = 1;
        }
        LocalPlayerWeapon weapon;
        PlayerViewState view;
        local_weapon_install(world, weapon, data, false, false, &inv, view);
        const WeaponSwitchOutcome outcome = weapon_switch_to_handle(f.t, inv,
                (same_category ? 3 : 2) * 65,
                local_weapon_switch_gates(world, weapon, &inv));
        CHECK(outcome.kind == WeaponSwitchOutcome::kMount);
        CHECK(outcome.same_category == same_category);
        handle_weapon_switch_outcome(world, weapon, &inv, outcome, view);
        LocalWeaponPumpIO io;
        io.view = &view;
        io.inventory = &inv;
        for (int tick = 0; tick < 80 && weapon.switch_in_flight; ++tick) {
            ++world.logic_tick;
            local_weapon_pump_tick(world, weapon, io);
        }
        CHECK(!weapon.switch_in_flight);
        CHECK(inv.equipped_combo == outcome.combo);
        CHECK(!weapon.events.empty());
        data.name = same_category ? "WPN_AK47AUTO" : "WPN_PISTOL";
        CHECK(!weapon.events.empty() && weapon.events.back().switch_to_weapon == data.name);
        local_weapon_install(world, weapon, data, false, false, &inv, view);
        CHECK(weapon.slot.next == (same_category ? weapon_action::kIdle
                                                : weapon_action::kSwitchTo));
    }
}

// ToSpecial / QuickSwitch (input case 220): the local fill takes the first
// QuickSwitch slot as the hold-swap target and clears the stash; the press
// stashes the equipped slot and mounts the target, the release mounts the
// stash again; a reload defers either dispatch (a draw defers the release), a
// gunner seat refuses both legs' swaps, and a successful swap drops the
// binoculars request.
// [orig: WeaponSlotTable_LoadAllFromDefs @ 0x541626..0x54166E;
//  Input_HandleActionBinding_0 case 0xDC @ 0x4E115B..0x4E1273;
//  Player_EquipWeaponByEntity @ 0x4E0370; Player_MountWeaponSlot @ 0x4DFA40]
void test_to_special_hold_swap() {
    Fixture f;
    const int mag = add_entry(f.t, "WPN_MAG58", 4, 0, 100, 400, "AMMO_762", 1, 1);
    const int point_aim = add_entry(f.t, "WPN_MAG58_POINTAIM", 4, 1, 100, 400, "AMMO_762", 1, 1);
    f.t.entries[static_cast<size_t>(point_aim)].flags = weapon_flag::kQuickSwitch;
    (void)mag;
    World world;
    world.tables.weapons = f.t;
    WeaponInventory inv;
    inv.reset(f.t);
    inv.quick_switch_stash = 7; // a stale stash the fill clears
    weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO", "WPN_MAG58", "WPN_MAG58_POINTAIM"},
                                       inv, 8, false);
    const int32_t m4_combo = 3 * 65;
    const int32_t target = 4 * 65 + 1;
    CHECK(inv.quick_switch_combo == target);
    CHECK(inv.quick_switch_stash == -1);
    {
        WeaponInventory plain;
        plain.reset(f.t);
        weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO", "WPN_MAG58"}, plain, 8, false);
        CHECK(plain.quick_switch_combo == -1);
    }
    weapon_inventory_seed_pools(f.t, inv, 8);
    weapon_inventory_recalc_clips(f.t, inv);
    inv.equipped_combo = m4_combo;
    world.registry.configure_pool(0, 1);
    Entity seed;
    seed.alive = true;
    seed.health = 100;
    world.cached.local_player = world.registry.spawn(0, seed);
    WeaponInstallData data;
    data.name = "WPN_M4AUTO";
    data.clipsize = 30;
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    local_weapon_install(world, weapon, data, false, false, &inv, view);

    // A reload defers the press; nothing changes.
    weapon.slot.current = weapon_action::kReload;
    CHECK(local_player_to_special(world, weapon, inv, view, true) == ToSpecialResult::kDeferred);
    CHECK(inv.quick_switch_stash == -1 && inv.pending_combo != target);
    weapon.slot.current = weapon_action::kIdle;
    // A gunner seat refuses the press.
    world.registry.get(world.cached.local_player)->mounted = true;
    world.registry.get(world.cached.local_player)->mount_type = SeatType::Gunner;
    CHECK(local_player_to_special(world, weapon, inv, view, true) == ToSpecialResult::kHandled);
    CHECK(inv.quick_switch_stash == -1);
    world.registry.get(world.cached.local_player)->mounted = false;
    world.registry.get(world.cached.local_player)->mount_type = SeatType::None;
    // The press: the M4 is stashed, the point-aim MAG58 is the pending slot,
    // the M4 queues its switch-out, the binoculars request drops.
    view.binoculars_requested = true;
    CHECK(local_player_to_special(world, weapon, inv, view, true) == ToSpecialResult::kHandled);
    CHECK(inv.quick_switch_stash == m4_combo);
    CHECK(inv.pending_combo == target);
    CHECK(weapon.switch_in_flight);
    CHECK(!view.binoculars_requested);
    // A second press while stashed does nothing.
    CHECK(local_player_to_special(world, weapon, inv, view, true) == ToSpecialResult::kHandled);
    CHECK(inv.quick_switch_stash == m4_combo && inv.pending_combo == target);
    // The release waits out a draw (and a reload).
    weapon.slot.current = weapon_action::kSwitchTo;
    CHECK(local_player_to_special(world, weapon, inv, view, false) == ToSpecialResult::kDeferred);
    weapon.slot.current = weapon_action::kReload;
    CHECK(local_player_to_special(world, weapon, inv, view, false) == ToSpecialResult::kDeferred);
    CHECK(inv.quick_switch_stash == m4_combo);
    // The release in a gunner seat keeps the stash.
    weapon.slot.current = weapon_action::kIdle;
    world.registry.get(world.cached.local_player)->mounted = true;
    world.registry.get(world.cached.local_player)->mount_type = SeatType::Gunner;
    CHECK(local_player_to_special(world, weapon, inv, view, false) == ToSpecialResult::kHandled);
    CHECK(inv.quick_switch_stash == m4_combo);
    world.registry.get(world.cached.local_player)->mounted = false;
    world.registry.get(world.cached.local_player)->mount_type = SeatType::None;
    // The release: the M4 is mounted again and the stash clears.
    inv.equipped_combo = target;
    weapon.switch_in_flight = false;
    view.binoculars_requested = true;
    CHECK(local_player_to_special(world, weapon, inv, view, false) == ToSpecialResult::kHandled);
    CHECK(inv.quick_switch_stash == -1);
    CHECK(inv.pending_combo == m4_combo);
    CHECK(!view.binoculars_requested);
    // A release with no stash does nothing.
    inv.pending_combo = -1;
    CHECK(local_player_to_special(world, weapon, inv, view, false) == ToSpecialResult::kHandled);
    CHECK(inv.pending_combo == -1);
    // The equipped slot already the target: the press does nothing.
    CHECK(local_player_to_special(world, weapon, inv, view, true) == ToSpecialResult::kHandled);
    CHECK(inv.quick_switch_stash == -1);
}

// The LocalPlayer's dispatch queue: a deferred dispatch stays queued for the
// next frame's flush, then runs with the keys' state at that time.
// [orig: Input_QueueDeferredEvent @0x4993E0; Input_FlushDeferredEvents @0x497AC0]
void test_to_special_queue_defers() {
    Fixture f;
    const int point_aim = add_entry(f.t, "WPN_MAG58_POINTAIM", 4, 1, 100, 400, "AMMO_762", 1, 1);
    f.t.entries[static_cast<size_t>(point_aim)].flags = weapon_flag::kQuickSwitch;
    World world;
    world.tables.weapons = f.t;
    world.registry.configure_pool(0, 1);
    Entity seed;
    seed.alive = true;
    seed.health = 100;
    world.cached.local_player = world.registry.spawn(0, seed);
    LocalPlayer lp(world);
    lp.inventory.reset(f.t);
    weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO", "WPN_MAG58_POINTAIM"}, lp.inventory,
                                       8, false);
    lp.inventory_valid = true;
    lp.inventory.equipped_combo = 3 * 65;
    WeaponInstallData data;
    data.name = "WPN_M4AUTO";
    data.clipsize = 30;
    local_weapon_install(world, lp.weapon, data, false, false, &lp.inventory, lp.view);
    lp.weapon.slot.current = weapon_action::kReload;
    lp.queue_to_special();
    lp.dispatch_to_special(true);
    CHECK(lp.to_special_queued == 1 && lp.inventory.quick_switch_stash == -1);
    lp.weapon.slot.current = weapon_action::kIdle;
    lp.dispatch_to_special(true);
    CHECK(lp.to_special_queued == 0 && lp.inventory.quick_switch_stash == 3 * 65);
    for (int i = 0; i < 40; ++i) lp.queue_to_special();
    CHECK(lp.to_special_queued == 32);
}

void test_cycle() {
    Fixture f;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(
            f.t, {"WPN_KNIFE", "WPN_PISTOL", "WPN_M4AUTO", "WPN_AK47AUTO"}, inv, 8, false);
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
    weapon_inventory_load_from_display(f.t, {"WPN_PISTOL"}, lone, 8, false);
    weapon_inventory_seed_pools(f.t, lone, 8);
    weapon_inventory_recalc_clips(f.t, lone);
    lone.equipped_combo = 130;
    CHECK(weapon_cycle_slot(f.t, lone, +1, gates).kind == WeaponSwitchOutcome::kNone);
}

void test_defaults() {
    // The static single-player page blob the double-NUL copy takes whole
    // [orig: 0x833BF8 @ 0x5246be].
    auto def = weapon_kit_default(opennova::gameprofile::GAME_JO);
    CHECK(def.size() == 8 && def[0].name == "WPN_M4AUTO" && def[1].name == "WPN_M4" &&
          def[2].name == "WPN_colt45" && def[7].name == "WPN_KNIFE");
    // dfx2.exe's page names WPN_MK23 where JO's names WPN_colt45
    // [orig: the page at 0x832368 @0x52188f (dfx2.exe)].
    auto dfx2 = weapon_kit_default(opennova::gameprofile::GAME_DFX2);
    CHECK(dfx2.size() == 8 && dfx2[0].name == "WPN_M4AUTO" && dfx2[1].name == "WPN_M4" &&
          dfx2[2].name == "WPN_MK23" && dfx2[3].name == "WPN_AT4" && dfx2[7].name == "WPN_KNIFE");
    CHECK(def[3].name == "WPN_AT4" && def[3].ammo_primary == -1 && def[3].flags == -1);
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
// `weapon_class == 1 || weapon_class == 2 || Score_CalculateKillScore(slot, entity, 0, 0)`
// [orig: Player_SwitchToWeaponByHandle @0x4e0294..0x4e02c3], so the knife rides
// entirely on that score term.
static int test_knife_is_selectable() {
	Fixture f;
	WeaponInventory inv;
	inv.reset(f.t);
	weapon_inventory_load_from_display(f.t, {"WPN_KNIFE", "WPN_M4AUTO"}, inv, 8, false);
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

// The local held-weapon draw gate [orig: Entity_CanFireWeapon @ 0x4dcb10, the
// local branch @ 0x4dcbcf..0x4dcc5d]: drawn iff the soldier may FIRE it.
static int test_local_held_weapon_visible() {
	Fixture f;
	World world;
	world.tables.weapons = f.t;
	// The m4 row draws in first person.
	world.tables.weapons.entries[static_cast<size_t>(f.m4)]
			.has_first_person_model_reference = true;

	LocalPlayerWeapon weapon;
	weapon.active = true;
	WeaponInventory inv;
	inv.reset(world.tables.weapons);
	weapon_inventory_load_from_display(world.tables.weapons, {"WPN_M4AUTO"}, inv, 8, false);
	weapon_inventory_seed_pools(world.tables.weapons, inv, 8);
	weapon_inventory_recalc_clips(world.tables.weapons, inv);
	inv.equipped_combo = 3 * 65;

	Entity player;
	player.alive = true;

	CHECK(local_held_weapon_visible(world, player, weapon, inv, false));
	// Dead hides [orig: @0x4dcb22].
	player.flags |= 2u;
	CHECK(!local_held_weapon_visible(world, player, weapon, inv, false));
	player.flags &= ~2u;
	// No EquippedSlot hides [orig: @0x4dcbcf].
	weapon.active = false;
	CHECK(!local_held_weapon_visible(world, player, weapon, inv, false));
	weapon.active = true;
	// No first-person model hides on your OWN body [orig: @0x4dcc32].
	world.tables.weapons.entries[static_cast<size_t>(f.m4)]
			.has_first_person_model_reference = false;
	CHECK(!local_held_weapon_visible(world, player, weapon, inv, false));
	world.tables.weapons.entries[static_cast<size_t>(f.m4)]
			.has_first_person_model_reference = true;
	// Seat rule, LOCAL flavour [orig: @0x4dcc44..0x4dcc5d]: driver hides,
	// gunner hides only in third person, passenger keeps its weapon.
	player.mounted = true;
	player.mount_type = SeatType::Driver;
	CHECK(!local_held_weapon_visible(world, player, weapon, inv, false));
	player.mount_type = SeatType::Gunner;
	CHECK(local_held_weapon_visible(world, player, weapon, inv, false));
	CHECK(!local_held_weapon_visible(world, player, weapon, inv, true));
	player.mount_type = SeatType::Passenger;
	CHECK(local_held_weapon_visible(world, player, weapon, inv, true));
	// NoClipsNoDraw adds the loaded rounds only for a one-round clip: an empty pool
	// hides a rifle with rounds in its clip, while a one-round weapon's loaded round
	// keeps it drawn [orig: Entity_CanFireWeapon @0x4dcbea..0x4dcc2d].
	player.mounted = false;
	player.mount_type = SeatType::None;
	auto &m4 = world.tables.weapons.entries[static_cast<size_t>(f.m4)];
	m4.flags |= weapon_flag::kNoClipsNoDraw;
	inv.pools[static_cast<size_t>(m4.ammo_class_id)] = 0;
	inv.slot(3 * 65)->clip = 5;
	CHECK(!local_held_weapon_visible(world, player, weapon, inv, false));
	m4.clipsize = 1;
	inv.slot(3 * 65)->clip = 1;
	CHECK(local_held_weapon_visible(world, player, weapon, inv, false));
	std::printf("PASS local held-weapon draw gate\n");
	return 0;
}

// The second weapon reads/refunds the SAME loaded bucket, not another clip.
// [orig: WeaponSlots_RecalculateAmmoFromCapacity @ 0x542280]
void test_two_weapons_share_loaded_ammo() {
    Fixture f;
    auto &a = f.t.entries[f.m4];
    auto &b = f.t.entries[f.ak];
    a.ammo_bucket = b.ammo_bucket = 1;
    b.ammo_class_id = a.ammo_class_id;
    b.ammo_class = a.ammo_class;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO", "WPN_AK47AUTO"}, inv, 8, false);
    weapon_pool_set(f.t, inv, a.ammo_class_id, 100);
    weapon_inventory_recalc_clips(f.t, inv);
    CHECK(weapon_pool_get(inv, a.ammo_class_id) == 70);
    CHECK(inv.slot(195)->clip == 0 && inv.slot(196)->clip == 0);
    CHECK(weapon_inventory_total_clips(f.t, inv, 195) == 3);
    CHECK(weapon_inventory_total_clips(f.t, inv, 196) == 3);
    weapon_inventory_set_loaded_rounds(f.t, inv, 195, 23);
    CHECK(weapon_inventory_loaded_rounds(f.t, inv, 196) == 23);
    CHECK(weapon_inventory_reload_slot(f.t, inv, 196, WeaponAmmoSeat{}) == 30);
    CHECK(weapon_pool_get(inv, a.ammo_class_id) == 63);
}

// The fill seeds each slot's scope zoom (MountSlot+0xC) with the local player
// as its owner: the def's second scope_max_mag value clamped into
// [floor, scope_max_mag], the floor being scope_min_mag or, for a class-6 owner
// on a category-3 def without the sniper permission, scope_max_mag. A floor
// above the max still wins (the `jl` arm stores it unchecked).
// [orig: WeaponSlot_InitFromDef @ 0x53EEF1..0x53EF44, entityPtr from
//  WeaponSlotTable_LoadAllFromDefs @ 0x5414FC]
void test_fill_seeds_the_slot_zoom() {
    Fixture f;
    WeaponTableEntry &m4 = f.t.entries[static_cast<size_t>(f.m4)];
    m4.scope_max_mag = 10;     // 'scope_max_mag 10 2' (the JOX M1/T80 turret shape)
    m4.scope_initial_mag = 2;
    WeaponTableEntry &ak = f.t.entries[static_cast<size_t>(f.ak)];
    ak.scope_max_mag = 12;     // one value: the seed starts at the floor
    ak.scope_min_mag = 4;
    WeaponTableEntry &pistol = f.t.entries[static_cast<size_t>(f.pistol)];
    pistol.scope_max_mag = 2;  // an over-max second value is capped
    pistol.scope_initial_mag = 9;
    pistol.scope_min_mag = 1;
    // The knife authors nothing: the record-default floor 2 wins over max 0.
    const std::vector<std::string> kit = {"WPN_KNIFE", "WPN_PISTOL", "WPN_M4AUTO",
                                          "WPN_AK47AUTO"};
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(f.t, kit, inv, 8, false);
    CHECK(inv.slot(3 * 65 + 0)->scope_zoom == 2);
    CHECK(inv.slot(3 * 65 + 1)->scope_zoom == 4);
    CHECK(inv.slot(2 * 65 + 0)->scope_zoom == 2);
    CHECK(inv.slot(1 * 65 + 0)->scope_zoom == 2);
    // A class-6 owner on the Primary (category 3) defs floors them at the max;
    // the permission lifts it, and the lock never touches another category.
    inv.reset(f.t);
    weapon_inventory_load_from_display(f.t, kit, inv, 6, false);
    CHECK(inv.slot(3 * 65 + 0)->scope_zoom == 10);
    CHECK(inv.slot(3 * 65 + 1)->scope_zoom == 12);
    CHECK(inv.slot(2 * 65 + 0)->scope_zoom == 2);
    inv.reset(f.t);
    weapon_inventory_load_from_display(f.t, kit, inv, 6, true);
    CHECK(inv.slot(3 * 65 + 0)->scope_zoom == 2);
    CHECK(inv.slot(3 * 65 + 1)->scope_zoom == 4);
    CHECK(weapon_slot_zoom_sniper_lock(6, 3, false));
    CHECK(!weapon_slot_zoom_sniper_lock(8, 3, false));
    CHECK(!weapon_slot_zoom_sniper_lock(6, 11, false));
    CHECK(weapon_slot_initial_zoom(7, 0, 2, false) == 2);  // revx02 cannon 'SCOPE_MAX_MAG 7'
    CHECK(weapon_slot_initial_zoom(12, 0, 1, false) == 1); // revx02 RCWS 'MAX 12 / MIN 1'
    CHECK(weapon_slot_initial_zoom(8, 8, 2, false) == 8);  // WPN_EMP50BD '8 8'
}

// The HUDLS slot bar's category scan [orig: HUD_DrawWeaponSlotBar
// @0x599D0A..0x599D69]: categories 1..5 never scanned; the first def-bearing
// slot recorded and its loadout_subclasses variants jumped; every other
// def-bearing slot counted, a later weapon's own variants included.
void test_slot_bar_scan() {
    Fixture f;
    const int first = add_entry(f.t, "WPN_C6A", 6, 0, 1, 1, "AMMO_C6", 1, 0, 0, /*subclasses=*/1);
    const int first_sub = add_entry(f.t, "WPN_C6A_V", 6, 1, 1, 1, "AMMO_C6", 1, 0);
    const int second = add_entry(f.t, "WPN_C6B", 6, 5, 1, 1, "AMMO_C6", 1, 0, 0, /*subclasses=*/1);
    const int second_sub = add_entry(f.t, "WPN_C6B_V", 6, 6, 1, 1, "AMMO_C6", 1, 0);
    const int c9 = add_entry(f.t, "WPN_C9", 9, 3, 1, 1, "AMMO_C9", 1, 0);
    const int c0 = add_entry(f.t, "WPN_C0", 0, 2, 1, 1, "AMMO_C0", 1, 0);
    WeaponInventory inv;
    inv.reset(f.t);
    const auto place = [&](int adm, int cat, int rank) {
        inv.slots[static_cast<size_t>(cat * 65 + rank)].adm_index = static_cast<int16_t>(adm);
    };
    place(first, 6, 0);
    place(first_sub, 6, 1);
    place(second, 6, 5);
    place(second_sub, 6, 6);
    place(c9, 9, 3);
    place(c0, 0, 2);
    place(f.m4, 3, 0); // category 3: never scanned
    const auto scan = weapon_inventory_slot_bar_scan(f.t, inv);
    CHECK(scan[6].adm_index == first);
    // The first weapon's variant is jumped; the second AND its variant count.
    CHECK(scan[6].count == 3);
    CHECK(scan[9].adm_index == c9 && scan[9].count == 1);
    CHECK(scan[0].adm_index == c0 && scan[0].count == 1);
    CHECK(scan[3].adm_index == -1 && scan[3].count == 0);
    CHECK(scan[7].adm_index == -1 && scan[7].count == 0);
    // The recorded def's category dword, the key label's record index
    // (200 + it) [orig: @0x599e8c..0x599e99].
    CHECK(scan[6].def_category == 6 && scan[9].def_category == 9 && scan[0].def_category == 0);
    CHECK(scan[7].def_category == -1);
}

// The redraw's seat legs: a UseGun seat redraws nothing; a controller seat spends
// nothing, reads ammo class 1 as 0 and every other class as the EWeap carrier's
// reserve. [orig: WeaponSlots_RecalculateAmmoFromCapacity @0x54228D..0x542294;
//  Entity_GetScoreValueBySlotType @0x54073F..0x540769; WeaponSlot_DecrementAmmo
//  @0x540920]
void test_recalc_seat_legs() {
    WeaponTable t;
    t.ammo_class_names = {"", "CLASS_MANA", "CLS_B"};
    t.ammo_class_caps = {1000, 1000, 1000};
    t.entries.resize(3);
    for (int adm : {1, 2}) {
        WeaponTableEntry &e = t.entries[static_cast<size_t>(adm)];
        e.name = adm == 1 ? "WPN_CLASSONE" : "WPN_CLASSTWO";
        e.valid = true;
        e.category = 3;
        e.rank = static_cast<uint8_t>(adm);
        e.clipsize = 10;
        e.ammo_class_id = adm;
        e.ammo_class_count = 1;
    }
    WeaponInventory inv;
    inv.reset(t);
    inv.slots[3 * 65 + 1].adm_index = 1;
    inv.slots[3 * 65 + 2].adm_index = 2;
    inv.pools[1] = 50;
    inv.pools[2] = 50;
    WeaponAmmoSeat seat;
    seat.parent_slot = 3;
    weapon_inventory_recalc_clips(t, inv, seat);
    CHECK(inv.slot(3 * 65 + 1)->clip == 0 && inv.slot(3 * 65 + 2)->clip == 0 &&
          inv.pools[1] == 50 && inv.pools[2] == 50);
    seat.parent_slot = 2;
    seat.carrier = true;
    seat.carrier_eweap = true;
    seat.carrier_reserve = 4;
    weapon_inventory_recalc_clips(t, inv, seat);
    CHECK(inv.slot(3 * 65 + 1)->clip == 0);
    CHECK(inv.slot(3 * 65 + 2)->clip == 4);
    CHECK(inv.pools[1] == 50 && inv.pools[2] == 50);
}

// The clip counts and the loadout weight read through the seat: a UseGun seat counts
// the carrier's MountSlot reserve against the slot's own loaded rounds, a controller
// seat under a carrier that is no EWeap item counts no pool.
// [orig: WeaponSlot_GetTotalClips @0x5425F0 - the pool @0x542638, the slot word
//  @0x54265B; Entity_GetScoreValueBySlotType @0x540707..0x540799;
//  Terrain_AccumulateSectorScores @0x425220]
void test_total_clips_seat_legs() {
    WeaponTable t;
    t.ammo_class_names = {"", "CLASS_MANA", "CLS_B"};
    t.ammo_class_caps = {1000, 1000, 1000};
    t.entries.resize(3);
    WeaponTableEntry &e = t.entries[2];
    e.name = "WPN_SEATED";
    e.valid = true;
    e.category = 3;
    e.rank = 2;
    e.clipsize = 10;
    e.ammo_class_id = 2;
    e.ammo_class_count = 1;
    e.weaponweight_fp16 = 0x10000;
    e.clipweight_fp16 = 0x10000;
    WeaponInventory inv;
    inv.reset(t);
    inv.slots[3 * 65 + 2].adm_index = 2;
    inv.slots[3 * 65 + 2].clip = 5;
    inv.pools[2] = 25;
    CHECK(weapon_inventory_total_clips(t, inv, 3 * 65 + 2) == 3);
    CHECK(weapon_inventory_loadout_weight_fp16(t, inv) == 0x40000);
    WeaponAmmoSeat seat;
    seat.parent_slot = 3;
    seat.carrier = true;
    seat.carrier_reserve = 40;
    seat.carrier_clip = 7;
    CHECK(weapon_inventory_total_clips(t, inv, 3 * 65 + 2, seat) == 4);
    CHECK(weapon_inventory_loadout_weight_fp16(t, inv, seat) == 0x50000);
    seat.parent_slot = 2;
    CHECK(weapon_inventory_total_clips(t, inv, 3 * 65 + 2, seat) == 0);
    CHECK(weapon_inventory_loadout_weight_fp16(t, inv, seat) == 0x10000);
}

// The local pump reads a shared bucket and its pool through the player's seat: on a
// controller seat under a carrier with no EWeap bit both read 0 and the bucket keeps
// its rounds; off the seat they read the player's own.
// [orig: WeaponSlot_GetSharedLoadedRounds @0x540604..0x540627 from WeaponAction_Idle @0x542990;
//  Entity_GetScoreValueBySlotType @0x54078F..0x540799; WeaponSlot_SetSharedLoadedRounds @0x540684..0x540692]
void test_pump_reads_the_bucket_through_the_seat() {
    Fixture f;
    auto &m4 = f.t.entries[f.m4];
    m4.ammo_bucket = 1;
    World world;
    world.tables.weapons = f.t;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO"}, inv, 8, false);
    inv.equipped_combo = 3 * 65;
    inv.shared_clips[1] = 12;
    inv.pools[static_cast<size_t>(m4.ammo_class_id)] = 90;
    world.registry.configure_pool(0, 1);
    world.registry.configure_pool(1, 1);
    Entity seed;
    seed.alive = true;
    seed.health = 100;
    world.cached.local_player = world.registry.spawn(0, seed);
    Entity hull;
    hull.has_item_def = true;
    const EntityHandle carrier = world.registry.spawn(1, hull);

    WeaponInstallData data;
    data.name = "WPN_M4AUTO";
    data.clipsize = 30;
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    local_weapon_install(world, weapon, data, false, false, &inv, view);
    LocalWeaponPumpIO io;
    io.view = &view;
    io.inventory = &inv;
    ++world.logic_tick;
    local_weapon_pump_tick(world, weapon, io);
    CHECK(weapon.slot.clip == 12 && weapon.slot.reserve == 90);

    Entity *player = world.registry.get(world.cached.local_player);
    player->mounted = true;
    player->mount_type = SeatType::Controller;
    player->mount_target = carrier;
    ++world.logic_tick;
    local_weapon_pump_tick(world, weapon, io);
    CHECK(weapon.slot.clip == 0 && weapon.slot.reserve == 0);
    CHECK(inv.shared_clips[1] == 12);
    std::printf("PASS the pump reads the bucket through the seat\n");
}

// The reload refill takes the owner's seat: on a controller seat under a carrier with
// no EWeap bit a bucket rifle refunds and draws nothing, and a clip weapon empties its
// clip into the pool and draws none back; off the seat the bucket rifle reloads.
// [orig: WeaponSlot_ReloadAmmo @0x5417C5, @0x5417E7, @0x541805, @0x541821, @0x541842;
//  WeaponSlot_DecrementAmmo @0x540994]
void test_reload_takes_the_seat() {
    Fixture f;
    auto &m4 = f.t.entries[f.m4];
    m4.ammo_bucket = 1;
    WeaponInventory inv;
    inv.reset(f.t);
    weapon_inventory_load_from_display(f.t, {"WPN_M4AUTO", "WPN_KNIFE", "WPN_AK47AUTO"}, inv, 8,
            false);
    const int32_t m4_combo = 3 * 65;
    const size_t m4_pool = static_cast<size_t>(m4.ammo_class_id);
    inv.shared_clips[1] = 5;
    inv.pools[m4_pool] = 90;
    WeaponAmmoSeat seat;
    seat.parent_slot = 2;
    seat.carrier = true;
    CHECK(weapon_inventory_reload_slot(f.t, inv, m4_combo, seat) == 0);
    CHECK(inv.shared_clips[1] == 5 && inv.pools[m4_pool] == 90);
    const auto &ak = f.t.entries[static_cast<size_t>(f.ak)];
    const int32_t ak_combo = ak.rank + 65 * ak.category;
    const size_t ak_pool = static_cast<size_t>(ak.ammo_class_id);
    inv.slot(ak_combo)->clip = 4;
    const int32_t ak_before = inv.pools[ak_pool];
    CHECK(weapon_inventory_reload_slot(f.t, inv, ak_combo, seat) == 0);
    CHECK(inv.slot(ak_combo)->clip == 0 &&
            inv.pools[ak_pool] == ak_before + 4 * ak.ammo_class_count);
    const WeaponAmmoSeat unseated;
    CHECK(weapon_inventory_reload_slot(f.t, inv, m4_combo, unseated) == m4.clipsize);
    CHECK(inv.pools[m4_pool] == 90 + 5 * m4.ammo_class_count - m4.clipsize * m4.ammo_class_count);
    std::printf("PASS the reload takes the seat\n");
}

// An EWeap mount's reload: the re-split, the draw from the reserve, the clip the draw;
// an empty reserve leaves the clip empty. [orig: WeaponSlot_ReloadAmmo @0x541720;
//  the re-split @0x5462A0; Entity_SpendMountSlotReserve]
void test_mount_slot_reload() {
    WeaponTableEntry gun;
    gun.clipsize = 10;
    gun.startrounds = 100;
    gun.ammo_class_count = 1;
    WeaponSlotState slot;
    slot.clip = 4;
    slot.reserve = 50;
    weapon_mount_slot_reload(gun, slot, gun, slot);
    CHECK(slot.clip == 10 && slot.reserve == 38); // 4 + 50 + 4 = 58 -> 10 / 48 -> 10 / 38
    slot.clip = 7;
    slot.reserve = 0;
    weapon_mount_slot_reload(gun, slot, gun, slot);
    CHECK(slot.clip == 4 && slot.reserve == 0);   // 14 -> 10 / 4 -> draw 4 -> 4 / 0
    gun.startrounds = 9;
    slot.clip = 7;
    slot.reserve = 0;
    weapon_mount_slot_reload(gun, slot, gun, slot);
    CHECK(slot.clip == 0 && slot.reserve == 0);   // 14 capped at 9 -> 9 / 0 -> draw 0
    // A def with no `ammoclass` line holds one unit a round: a 1-round tank gun
    // with 40 start rounds reloads from its reserve [orig: AdmDef_InitEntryDefaults
    // +0xE0 = 1 @0x53FF5B; WPN_M1TURRET].
    opennova::def::DefWeaponDef row;
    opennova::def::def_init_weapon(row);
    CHECK(row.ammo_class_count == 1);
    WeaponTableEntry turret;
    turret.clipsize = 1;
    turret.startrounds = 40;
    turret.ammo_class_count = static_cast<int16_t>(row.ammo_class_count);
    slot.clip = 0;
    slot.reserve = 39;
    weapon_mount_slot_reload(turret, slot, turret, slot);
    CHECK(slot.clip == 1 && slot.reserve == 38);
    std::printf("PASS the mount slot reload\n");
}

// A kit name "None" resolves to the catalog's entry 0 and survives the filter, so the
// knife fallback does not run [orig: Mission_LoadBMSFile @0x40f80e..0x40f830; the
//  fallback @0x40f899].
void test_kit_filter_keeps_none() {
    Fixture f;
    WeaponAvailability avail;
    auto kept = weapon_kit_filter_by_availability({{"None", -1, -1, -1}}, f.t, avail);
    CHECK(kept.size() == 1);
    CHECK(kept.size() == 1 && kept[0].name == "None");
}

int main() {
    test_kit_filter_keeps_none();
    test_mount_slot_reload();
    test_recalc_seat_legs();
    test_reload_takes_the_seat();
    test_pump_reads_the_bucket_through_the_seat();
    test_total_clips_seat_legs();
    test_slot_bar_scan();
    test_fill_seeds_the_slot_zoom();
    test_two_weapons_share_loaded_ammo();
    test_knife_is_selectable();
    test_local_held_weapon_visible();
    test_availability_pairs();
    test_kit_filter();
    test_display_expand_and_fill();
    test_pools_and_recalc();
    test_select();
    test_switch_walks();
    test_committed_switch_only_draws_after_holstering();
    test_defaults();
    test_kit_damage_classes();
    test_cycle();
    test_to_special_hold_swap();
    test_to_special_queue_defers();
    if (failures == 0) std::printf("weapon_inventory_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
