// The local player's loadout ORCHESTRATION (world/player_loadout.h, S7b): the
// mission-rules promotion with the witnessed SP-vs-net gate, the ACCEPT apply's
// requested-ammo overlay + banned validation, and the spawn rebuild's class
// latch — over the same small armory the weapon_inventory unit tests use.
// [orig: Mission_LoadBMSFile @ 0x40F4E0; WeaponLoadout_ApplyFromBuffer
//  @ 0x565cd0; Player_InitPlayer @ 0x4e15f0]
#include <cstdio>

#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

int add_entry(WeaponTable &t, const char *name, int cat, int rank, int clipsize,
              int startrounds, const char *ammo_class, int wclass,
              int subclasses = 0) {
    WeaponTableEntry e;
    e.name = name;
    e.valid = true;
    e.category = static_cast<uint8_t>(cat);
    e.rank = static_cast<uint8_t>(rank);
    e.clipsize = static_cast<int16_t>(clipsize);
    e.startrounds = static_cast<int16_t>(startrounds);
    e.maxclips = 10;
    e.ammo_class = ammo_class;
    e.ammo_class_count = 1;
    e.weapon_class_slot = wclass;
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

struct Rig {
    World world;
    LocalPlayerLoadout loadout;
    LocalPlayerWeapon weapon;
    WeaponInventory inventory;
    bool inventory_valid = false;
    int m4 = -1;

    Rig() {
        WeaponTable &t = world.tables.weapons;
        t.ammo_class_names.emplace_back("");
        t.ammo_class_caps.push_back(0);
        WeaponTableEntry null_e;
        null_e.name = "null";
        null_e.valid = true;
        null_e.ammo_class_id = 0;
        t.entries.push_back(std::move(null_e));
        const int knife = add_entry(t, "WPN_KNIFE", 1, 0, -1, -1, "", 0);
        t.entries[static_cast<size_t>(knife)].ammo_class_id = 0;
        m4 = add_entry(t, "WPN_M4AUTO", 3, 0, 30, 210, "AMMO_556", 1);
        add_entry(t, "WPN_AK47AUTO", 3, 1, 30, 210, "AMMO_762", 1);
    }
};

std::vector<WeaponKitEntry> kit_of(const char *name, int32_t pri = -1,
                                   int32_t sec = -1, int32_t flags = -1) {
    return {WeaponKitEntry{name, pri, sec, flags}};
}

// The gate: a live session — listen host or joiner alike — never promotes the
// mission chunks [orig: @ 0x40f694 -> the fseek pair @ 0x40f6b2/@ 0x40f6e1].
void test_promotion_gate_skips_in_session() {
    Rig r;
    r.world.rules.mp_session = true;
    CHECK(!local_loadout_promote_mission_rules(r.world, r.loadout,
            {{"WPN_AK47AUTO", 0}}, kit_of("WPN_M4AUTO", 3)));
    CHECK(!r.loadout.mission_kit_promoted && r.loadout.spawn_kit().empty());
    CHECK(r.loadout.availability.value_for(3) ==
            weapon_availability_value::kAllowed);
}

// Offline: availability applies first, then the kit filters through it with
// the knife fallback [orig: the filter @ 0x40f834, the fallback @ 0x40f899].
void test_promotion_applies_availability_then_filters_kit() {
    Rig r;
    CHECK(local_loadout_promote_mission_rules(r.world, r.loadout,
            {{"WPN_M4AUTO", 0}}, kit_of("WPN_M4AUTO", 3)));
    CHECK(r.loadout.mission_kit_promoted);
    CHECK(r.loadout.spawn_kit().size() == 1);
    if (r.loadout.spawn_kit().size() == 1)
        CHECK(r.loadout.spawn_kit()[0].name == "WPN_KNIFE");
}

// The ACCEPT overlay: requested clips expand to min(req, maxclips) * clipsize;
// the banned validation drops entries only when requested
// [orig: @ 0x566166; the 0x2F gate @ 0x515a3f].
void test_accept_requested_ammo_and_banned_validation() {
    Rig r;
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO", 3), 8,
            /*validate_banned=*/true, LoadoutApplySelect::HeldWeapon));
    CHECK(r.inventory_valid);
    const WeaponTableEntry *def =
            r.world.tables.weapons.by_index(static_cast<uint8_t>(r.m4));
    CHECK(def != nullptr &&
            r.inventory.pools[static_cast<size_t>(def->ammo_class_id)] == 90);

    // Ban the M4 and re-accept with validation: the entry drops.
    local_loadout_apply_availability_pairs(r.world, r.loadout,
            {{"WPN_M4AUTO", 0}});
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO", 3), 8,
            /*validate_banned=*/true, LoadoutApplySelect::HeldWeapon));
    CHECK(r.loadout.spawn_kit().empty());
    // The S2C 0x5A grant apply skips the validation and keeps it.
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO", 3), 8,
            /*validate_banned=*/false, LoadoutApplySelect::HeldWeapon));
    CHECK(r.loadout.spawn_kit().size() == 1);
}

// The pre-spawn class latch seeds the rebuild's pools until L exists, over an empty
// spawn buffer too [orig: the clamp default 8 @ 0x51d102].
void test_class_latch_survives_kit_reset() {
    Rig r;
    r.loadout.pending_player_class = 6;
    CHECK(r.loadout.spawn_kit().empty());
    CHECK(r.loadout.pending_player_class == 6);
    local_loadout_rebuild(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, /*select_spawn_default=*/true);
    CHECK(r.inventory_valid);
}


// The personal slot's scope zoom rides its inventory entry (MountSlot+0xC):
// the spawn rebuild seeds it with the local player as the owner, a mount reads
// the entry back, a zoom step stores into it, and the next mount of the slot
// reads the stepped zoom back instead of the lazy max. A def the equipped entry
// does not back seeds a fresh slot and stores nothing.
// [orig: WeaponSlotTable_LoadAllFromDefs @ 0x5415E4 -> WeaponSlot_InitFromDef
//  @ 0x53EF2D..0x53EF44; Player_AdjustWeaponElevation @ 0x4DBE6C]
void test_rebuild_and_mount_carry_the_slot_zoom() {
    Rig r;
    WeaponTableEntry &m4 = r.world.tables.weapons.entries[static_cast<size_t>(r.m4)];
    m4.scope_max_mag = 10; // one authored value: the seed starts at the floor 2
    r.world.registry.configure_pool(0, 1);
    r.world.cached.local_player = r.world.registry.spawn(0, Entity{});
    Entity *player = r.world.registry.get(r.world.cached.local_player);
    player->player_class = 8;
    r.loadout.spawn_kit() = kit_of("WPN_M4AUTO");
    local_loadout_rebuild(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, /*select_spawn_default=*/true);
    CHECK(r.inventory.equipped_combo == 3 * 65);
    CHECK(r.inventory.slot(3 * 65)->scope_zoom == 2);

    WeaponInstallData data;
    data.name = "WPN_M4AUTO";
    data.scope_max_mag = 10.0f;
    data.hud_category = 3;
    PlayerViewState view;
    local_weapon_install(r.world, r.weapon, data, false, false, &r.inventory, view);
    CHECK(r.weapon.slot.scope_zoom == 2);
    r.weapon.slot.scope_zoom = 6; // a +2, +2 zoom step on the mounted slot
    local_weapon_store_scope_zoom(r.world, r.weapon, r.inventory);
    CHECK(r.inventory.slot(3 * 65)->scope_zoom == 6);
    local_weapon_install(r.world, r.weapon, data, false, false, &r.inventory, view);
    CHECK(r.weapon.slot.scope_zoom == 6);

    // A class-6 owner on the Primary def: the rebuild floors the seed at the max.
    player->player_class = 6;
    local_loadout_rebuild(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, /*select_spawn_default=*/true);
    CHECK(r.inventory.slot(3 * 65)->scope_zoom == 10);

    WeaponInstallData shell;
    shell.name = "WPN_SHELL_ONLY";
    shell.scope_max_mag = 12.0f;
    shell.scope_min_mag = 4;
    shell.hud_category = 2;
    local_weapon_install(r.world, r.weapon, shell, false, false, &r.inventory, view);
    CHECK(r.weapon.slot.scope_zoom == 4);
    r.weapon.slot.scope_zoom = 8;
    local_weapon_store_scope_zoom(r.world, r.weapon, r.inventory);
    CHECK(r.inventory.slot(3 * 65)->scope_zoom == 10);
}

// A mount reads the def's own slot of the slot table, whatever slot is equipped:
// clip = that slot's loaded rounds, reserve = the def's ammo-class pool. A def the
// loadout never landed reads its slot's cleared words, no rounds and the pool's
// count; no inventory at all (a harness, the editor's preview) keeps the def seed.
// [orig: MountSlot+0x10 + Entity_GetScoreValueBySlotType @0x5406e0; the slot
//  table @0x5415D3; WeaponSlot_InitFromDef @0x53EE86]
void test_a_mount_reads_the_defs_own_slot() {
    Rig r;
    r.world.registry.configure_pool(0, 1);
    r.world.cached.local_player = r.world.registry.spawn(0, Entity{});
    const WeaponTable &t = r.world.tables.weapons;
    const int ak = t.index_of("WPN_AK47AUTO");
    CHECK(ak >= 0);
    const int16_t ak_class = t.entries[static_cast<size_t>(ak)].ammo_class_id;
    r.loadout.spawn_kit() = {WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1},
                             WeaponKitEntry{"WPN_AK47AUTO", -1, -1, -1}};
    local_loadout_rebuild(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, /*select_spawn_default=*/true);
    CHECK(r.inventory.equipped_combo == 3 * 65);
    CHECK(r.inventory.slot(3 * 65 + 1) != nullptr && r.inventory.slot(3 * 65 + 1)->adm_index == ak);
    weapon_inventory_set_loaded_rounds(t, r.inventory, 3 * 65 + 1, 7);
    weapon_pool_set(t, r.inventory, ak_class, 25);

    WeaponInstallData data;
    data.name = "WPN_AK47AUTO";
    data.clipsize = 30;
    data.startrounds = 210;
    PlayerViewState view;
    local_weapon_install(r.world, r.weapon, data, false, false, &r.inventory, view);
    CHECK(r.weapon.slot.clip == 7);
    CHECK(r.weapon.slot.reserve == 25);

    // The kit without the AK: its slot is cleared, the pool still the class's.
    r.loadout.spawn_kit() = kit_of("WPN_M4AUTO");
    local_loadout_rebuild(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, /*select_spawn_default=*/true);
    weapon_pool_set(t, r.inventory, ak_class, 12);
    local_weapon_install(r.world, r.weapon, data, false, false, &r.inventory, view);
    CHECK(r.weapon.slot.clip == 0);
    CHECK(r.weapon.slot.reserve == 12);

    // No slot table: the tools' seed.
    local_weapon_install(r.world, r.weapon, data, false, false, nullptr, view);
    CHECK(r.weapon.slot.clip == 30);
    CHECK(r.weapon.slot.reserve == 210);

    // A tool's mount (the shell's by-name install, the debug viewmodel seam) of the def
    // the loadout never landed takes the same seed; a landed def still reads its slot.
    WeaponInstallData tool = data;
    tool.tool_seed = true;
    local_weapon_install(r.world, r.weapon, tool, false, false, &r.inventory, view);
    CHECK(r.weapon.slot.clip == 30);
    CHECK(r.weapon.slot.reserve == 210);
    WeaponInstallData landed_tool;
    landed_tool.name = "WPN_M4AUTO";
    landed_tool.clipsize = 30;
    landed_tool.startrounds = 210;
    landed_tool.tool_seed = true;
    const int16_t m4_class = t.entries[static_cast<size_t>(r.m4)].ammo_class_id;
    weapon_inventory_set_loaded_rounds(t, r.inventory, 3 * 65, 9);
    weapon_pool_set(t, r.inventory, m4_class, 33);
    local_weapon_install(r.world, r.weapon, landed_tool, false, false, &r.inventory, view);
    CHECK(r.weapon.slot.clip == 9);
    CHECK(r.weapon.slot.reserve == 33);
}

void test_armor_carry_bit_follows_accepted_loadout() {
    Rig r;
    r.world.registry.configure_pool(0, 1);
    r.world.cached.local_player = r.world.registry.spawn(0, Entity{});
    Entity *player = r.world.registry.get(r.world.cached.local_player);
    r.world.tables.weapons.entries[r.m4].flags |= weapon_flag::kArmor;
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO"), 8, false, LoadoutApplySelect::HeldWeapon));
    CHECK((player->carry_flags & 8) != 0);
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_KNIFE"), 8, false, LoadoutApplySelect::HeldWeapon));
    CHECK((player->carry_flags & 8) == 0);
}

// A running PowerThrow windup survives both loadout applies, which end on the select and the
// mount with no walk; the spawn rebuild's walk (Player_InitPlayer's) clears it (D-NET-1095,
// V-ROLL-AJ). [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566285, @0x566297);
//  NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (@0x4296E3, @0x4296F4); Player_InitPlayer
//  @0x4E15F0 - Player_SwitchToWeaponByHandle @0x4E19A1 (-> @0x4E019B)]
void test_a_windup_survives_the_applies_and_the_spawn_walk_clears_it() {
    Rig r;
    r.world.registry.configure_pool(0, 1);
    r.world.cached.local_player = r.world.registry.spawn(0, Entity{});
    r.weapon.power_throw_start_tick = 7;
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO"), 8, /*validate_banned=*/true,
            LoadoutApplySelect::HeldWeapon));
    CHECK(r.weapon.power_throw_start_tick == 7); // the armory ACCEPT runs no walk
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO"), 8, /*validate_banned=*/false,
            LoadoutApplySelect::HeldWeapon));
    CHECK(r.weapon.power_throw_start_tick == 7); // nor the S2C 0x5A apply
    local_loadout_rebuild(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, /*select_spawn_default=*/true);
    CHECK(r.weapon.power_throw_start_tick == 0); // Player_InitPlayer's walk clears it
}

// The apply's select (LoadoutApplySelect): the held weapon's combo, or under a protocol whose
// S2C 0x5A apply takes the kit's last primary row (DFX2-35) that row's, whatever the body holds,
// and AdmDef 0's (the "null" def, combo 0) when the kit lists no primary.
// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - @0x4296BC..0x4296D4;
//  NapiNPClientMsg_HandleWeaponLoadoutSync @0x42AF10 (dfx2.exe) - @0x42B0AB..0x42B0BD,
//  @0x42B4F3..0x42B50B]
void test_the_apply_selects_the_held_weapon_or_the_last_primary_row() {
    Rig r;
    const int m24 = add_entry(r.world.tables.weapons, "WPN_M24", 4, 0, 5, 30, "AMMO_762", 1);
    r.world.registry.configure_pool(0, 1);
    r.world.cached.local_player = r.world.registry.spawn(0, Entity{});
    r.world.registry.get(r.world.cached.local_player)->equipped_adm_index =
            static_cast<uint8_t>(m24);
    const std::vector<WeaponKitEntry> kit = {WeaponKitEntry{"WPN_M24", -1, -1, -1},
            WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}, WeaponKitEntry{"WPN_KNIFE", -1, -1, -1}};
    const int32_t m24_combo = 4 * weapon_combo::kRanksPerCategory;
    const int32_t m4_combo = 3 * weapon_combo::kRanksPerCategory;
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit, 8, /*validate_banned=*/false,
            LoadoutApplySelect::HeldWeapon));
    CHECK(r.inventory.current_slot == m24_combo && r.inventory.selected_combo == m24_combo);
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit, 8, /*validate_banned=*/false,
            LoadoutApplySelect::LastPrimaryRow));
    CHECK(r.inventory.current_slot == m4_combo && r.inventory.selected_combo == m4_combo);
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_KNIFE"), 8, /*validate_banned=*/false,
            LoadoutApplySelect::LastPrimaryRow));
    CHECK(r.inventory.current_slot == 0);
}

} // namespace

// The armory open-time class policy [orig: Armory_ResolveSelectedClass
// @0x5642f0]: allowed classes open as-is, disallowed scan up through 9, an
// exhausted scan falls to gunner (7); the filter bit is 1 << (class-5) for
// 5..9 and ALL weapons (-1) otherwise.
static void test_armory_class_policy() {
    CHECK(armory_resolve_selected_class(6, 1u << 6) == 6);
    CHECK(armory_resolve_selected_class(5, (1u << 8)) == 8); // scan up
    CHECK(armory_resolve_selected_class(9, 0u) == 7);        // exhausted -> gunner
    CHECK(armory_resolve_selected_class(8, (1u << 9)) == 9);
    CHECK(armory_class_filter_mask(5) == 1);
    CHECK(armory_class_filter_mask(9) == (1 << 4));
    CHECK(armory_class_filter_mask(0) == -1);  // unclassed SP spawn: no filter
    CHECK(armory_class_filter_mask(10) == -1);
}

// The PLAYER_INFO screen policies [orig: PlayerInfo_SetTeamAndClassMask
// @0x55de60; PlayerInfo_PopulateAmmoComboBoxes @0x55def0]: team 0 masks 2, every
// other team masks 1; classes 5..9 mask their bit and anything else masks
// NOTHING (unlike the armory's all-weapons default); the ammo combo's
// default select takes the saved count (clamped 1..maxclips) or the full
// maxclips row on the -1 sentinel.
static void test_player_info_menu_policy() {
    CHECK(player_info_team_mask(0) == 2);
    CHECK(player_info_team_mask(1) == 1);
    CHECK(player_info_team_mask(7) == 1);
    CHECK(player_info_class_mask(5) == 1);
    CHECK(player_info_class_mask(9) == (1 << 4));
    CHECK(player_info_class_mask(0) == 0);   // this screen filters nothing in
    CHECK(player_info_class_mask(10) == 0);
    CHECK(player_info_default_clip_row(-1, 6) == 6);  // untouched -> full row
    CHECK(player_info_default_clip_row(3, 6) == 3);
    CHECK(player_info_default_clip_row(9, 6) == 6);   // clamps into the table
    CHECK(player_info_default_clip_row(-1, 0) == 0);  // degenerate maxclips
    // The grenade combos carry a zero row: the row is the clip count.
    CHECK(player_info_default_grenade_row(-1, 4) == 4);  // untouched -> full row
    CHECK(player_info_default_grenade_row(0, 4) == 0);   // a saved 0 stays 0
    CHECK(player_info_default_grenade_row(2, 4) == 2);
    CHECK(player_info_default_grenade_row(9, 4) == 4);   // clamps into the table
    CHECK(player_info_default_grenade_row(-1, 0) == 0);  // degenerate maxclips
}

// The PLAYER_INFO weapon list counts a row only at its `end` [orig:
// WeaponDef_ParseProperty @0x54D730, the END compare], so an entry no `end`
// closed is in the weapon table but in no list.
static void test_slot_list_skips_an_unclosed_entry() {
    using opennova::def::DefWeaponDef;
    DefWeaponDef rows[2] = {};
    for (DefWeaponDef &row : rows) {
        row.weapon_class_slot = 1;
        row.loadout_selectable = 1;
        row.charfilter_mask = 1;
        row.teamfilter_mask = 1;
    }
    rows[1].unclosed = 1;
    std::vector<int32_t> indices;
    weapon_slot_indices(rows, 2, 1, 1, 1, indices);
    CHECK(indices.size() == 1 && indices[0] == 0);
}

// A sub-variant index past 255 wraps to the table's start in the accept's ammo leg,
// as the display list's does (D-NET-757), and the walk runs on past the "null" def
// the wrap reaches to every sub-variant of another class: the grenade launcher takes
// the requested 5 rounds. [orig: WeaponLoadout_ApplyFromBuffer @0x5661CA ->
//  AdmDef_GetEntryByIndex @0x53FC80; the walk @0x56621D..0x566233]
void test_accept_sub_variant_index_wraps() {
    Rig r;
    WeaponTable &t = r.world.tables.weapons;
    t.entries.clear();
    t.ammo_class_names.clear();
    t.ammo_class_caps.clear();
    t.ammo_class_names.emplace_back("");
    t.ammo_class_caps.push_back(0);
    WeaponTableEntry null_e;
    null_e.name = "null";
    null_e.valid = true;
    null_e.ammo_class_id = 0;
    t.entries.push_back(std::move(null_e));
    const int sub = add_entry(t, "WPN_EDGE_GL", 3, 40, 1, 3, "AMMO_40MM", 1);
    t.entries.resize(254);
    const int edge = add_entry(t, "WPN_EDGE", 3, 0, 30, 210, "AMMO_556", 1, 3);
    CHECK(edge == 254);
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_EDGE", 3, 5), 8, /*validate_banned=*/false,
            LoadoutApplySelect::HeldWeapon));
    const WeaponTableEntry &gl = t.entries[static_cast<size_t>(sub)];
    CHECK(r.inventory.pools[static_cast<size_t>(gl.ammo_class_id)] == 5);
}

int main() {
    test_accept_sub_variant_index_wraps();
    test_slot_list_skips_an_unclosed_entry();
    test_promotion_gate_skips_in_session();
    test_promotion_applies_availability_then_filters_kit();
    test_accept_requested_ammo_and_banned_validation();
    test_class_latch_survives_kit_reset();
    test_armor_carry_bit_follows_accepted_loadout();
    test_rebuild_and_mount_carry_the_slot_zoom();
    test_a_mount_reads_the_defs_own_slot();
    test_a_windup_survives_the_applies_and_the_spawn_walk_clears_it();
    test_the_apply_selects_the_held_weapon_or_the_last_primary_row();
    test_armory_class_policy();
    test_player_info_menu_policy();
    if (failures == 0) std::printf("player_loadout_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
