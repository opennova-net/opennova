// The local player's loadout orchestration — witness map in player_loadout.h.
#include <runtime/world/player_loadout.h>

#include <algorithm>

#include <base/io/log.h>
#include <base/io/strutil.h>

#include <runtime/world/entity.h>
#include <runtime/world/world.h>

using namespace opennova::def;

namespace opennova::world {

namespace {

// The seed class for pool seeding: the live entity's class, else the shell's
// latched pre-spawn request, else retail's out-of-range clamp default 8
// [orig: Server_PlayerAdd class clamp @ 0x51d102].
uint8_t loadout_seed_class(const World &world, const LocalPlayerLoadout &loadout) {
    const Entity *e = world.registry.get(world.cached.local_player);
    if (e != nullptr) return e->player_class;
    return static_cast<uint8_t>(
            loadout.pending_player_class >= 5 && loadout.pending_player_class <= 9
                    ? loadout.pending_player_class
                    : 8);
}

// Stamp the class onto the live entity, or latch it for the joiner spawn
// block when L does not exist yet.
void loadout_stamp_class(World &world, LocalPlayerLoadout &loadout,
                         int32_t player_class) {
    if (player_class < 5 || player_class > 9) return;
    Entity *e = world.registry.get(world.cached.local_player);
    if (e != nullptr)
        e->player_class = static_cast<uint8_t>(player_class);
    else
        loadout.pending_player_class = player_class;
}

} // namespace

void local_loadout_set_spawn_kit(World &world, LocalPlayerLoadout &loadout,
                                 std::vector<WeaponKitEntry> kit,
                                 bool filter_by_availability) {
    if (filter_by_availability && !kit.empty()) {
        // The SP .bms promote leg: availability-filter with the knife fallback
        // [orig: Mission_LoadBMSFile @ 0x40f7ae..0x40f95c].
        kit = weapon_kit_filter_by_availability(kit, world.tables.weapons,
                                                loadout.availability);
    }
    loadout.spawn_kit_set = !kit.empty();
    loadout.spawn_kit = std::move(kit);
}

void local_loadout_apply_availability_pairs(
        World &world, LocalPlayerLoadout &loadout,
        const std::vector<std::pair<std::string, int32_t>> &pairs) {
    loadout.availability.reset(); // [orig: the all-1 default @ 0x551c86]
    if (pairs.empty()) return;
    weapon_availability_apply_pairs(loadout.availability, world.tables.weapons, pairs);
}

bool local_loadout_promote_mission_rules(
        World &world, LocalPlayerLoadout &loadout,
        const std::vector<std::pair<std::string, int32_t>> &availability_rows,
        std::vector<WeaponKitEntry> kit_rows) {
    // THE GATE [orig: @ 0x40f694 `cmp is_in_session, 0` -> the fseek pair
    // @ 0x40f6b2 / @ 0x40f6e1]: a live session — listen host or joiner alike —
    // never promotes either chunk; the MP kit comes from the profile page.
    if (world.rules.mp_session) return false;
    if (!availability_rows.empty())
        local_loadout_apply_availability_pairs(world, loadout,
                availability_rows);
    if (kit_rows.empty()) return false;
    local_loadout_set_spawn_kit(world, loadout, std::move(kit_rows),
            /*filter_by_availability=*/true);
    return loadout.spawn_kit_set;
}

void local_loadout_sync_damage_classes(World &world,
                                       const LocalPlayerLoadout &loadout) {
    Entity *e = world.registry.get(world.cached.local_player);
    if (e == nullptr) return;
    e->ammo_damage_class.assign(world.tables.ammo.entries.size(), 0);
    const std::vector<WeaponKitEntry> kit =
            loadout.spawn_kit_set ? loadout.spawn_kit : weapon_kit_default();
    weapon_kit_build_damage_classes(kit, world.tables.weapons,
            world.tables.ammo.entries.size(), e->ammo_damage_class);
}

void local_loadout_rebuild(World &world, LocalPlayerLoadout &loadout,
                           LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                           bool &inventory_valid, bool select_spawn_default) {
    // [orig: Player_InitPlayer @ 0x4e15f0 — the full leg sequence in the
    //  header note.] Entity-optional: a joiner rebuilds before L spawns.
    Entity *e = world.registry.get(world.cached.local_player);
    const WeaponTable &table = world.tables.weapons;
    local_loadout_sync_damage_classes(world, loadout);
    if (table.empty()) return;
    const std::vector<WeaponKitEntry> kit =
            loadout.spawn_kit_set ? loadout.spawn_kit : weapon_kit_default();
    inventory.reset(table);
    const std::vector<std::string> display =
            weapon_kit_expand_display_list(kit, table);
    // The local player owns every slot the fill seeds: its class feeds the
    // zoom seed's sniper lock. The permission is World::rules'
    // session byte (byte_A821F0); offline retail reads the config's own
    // g_mp_allowsniperscopezoom instead, zero by default like the rule
    // [orig: Config_SetDefaults @ 0x54D364; apply_session_settings_to_globals
    //  @ 0x552284].
    const uint8_t seed_class = loadout_seed_class(world, loadout);
    const WeaponFillResult fill = weapon_inventory_load_from_display(table, display,
            inventory, seed_class, world.rules.allow_sniper_scope_zoom);
    for (const std::string &w : fill.warnings)
        io::logf(io::LogLevel::kWarn, "%s", w.c_str()); // [orig: ErrorLog_WriteTimestamped]
    weapon_inventory_seed_pools(table, inventory, seed_class);
    weapon_inventory_recalc_clips(table, inventory);
    // The loadout weight the next 0x5A apply would stamp into entity+0x37C
    // [orig: Terrain_AccumulateSectorScores @0x425220 via @0x4296f9].
    loadout.weight_fp16 = weapon_inventory_loadout_weight_fp16(table, inventory);
    if (e != nullptr) e->carry_flags = (e->carry_flags & ~0x18u) | inventory.carry_flags;
    inventory_valid = true;
    weapon.switch_in_flight = false;
    weapon.switch_deferred_action = -1;
    if (!select_spawn_default) return;
    const WeaponSwitchGates gates =
            local_weapon_switch_gates(world, weapon, &inventory);
    if (!weapon_select_slot(table, inventory, weapon_combo::kDefaultSpawnCombo,
                !gates.equip_blocked)) {
        // An empty table (the armory all-NONE kit) equips nothing.
        if (e != nullptr) e->equipped_adm_index = 0xFF;
        return;
    }
    // Player_InitPlayer follows the select with SwitchToWeaponByHandle(195)
    // [orig: @ 0x4e1995/@ 0x4e19a1], but the switch's mount walk rides the
    // entity's AI-slot binding gate [orig: entity+0x68 test @ 0x4e023a; writer
    // Entity_AllocateAISlot @ 0x40d2f4] — modeled here as not-yet-bound during
    // the spawn rebuild (the motor/presentation bind after load), so the spawn
    // equips exactly the selected slot and plays no switch actions. The gate's
    // init-time value is an open question (D-WPN-21, docs/divergence-ledger.md).
    inventory.pending_combo = inventory.equipped_combo;
    commit_pending_weapon_switch(world, weapon, &inventory);
    weapon.start_in_switchto = false;
}

bool local_loadout_apply_accept(World &world, LocalPlayerLoadout &loadout,
                                LocalPlayerWeapon &weapon,
                                WeaponInventory &inventory,
                                bool &inventory_valid,
                                const std::vector<WeaponKitEntry> &kit,
                                int32_t player_class, bool validate_banned) {
    // [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline leg.] A joiner
    // applies its kit before L exists; the inventory is sim-side state and the
    // entity stamps (class + equipped adm) defer to the joiner spawn block.
    loadout_stamp_class(world, loadout, player_class);
    std::vector<WeaponKitEntry> accepted;
    for (const WeaponKitEntry &entry : kit) {
        if (entry.name.empty()) continue;
        const int idx = world.tables.weapons.index_of(entry.name.c_str());
        if (idx < 0) continue;
        // The per-entry availability validation [orig: the server 0x2F gate
        // @ 0x515a3f — 0 drops the entry; 2 requires the armory zone, which
        // the ACCEPT flow is already gated on host-side].
        if (validate_banned &&
                loadout.availability.value_for(idx) ==
                        weapon_availability_value::kBanned)
            continue;
        accepted.push_back(entry);
    }
    // The accepted loadout becomes the respawn kit [orig: the S2C 0x5A apply
    // writes restrictionData @ 0x4293e4; SP shares the buffer]. An explicit
    // empty kit stays empty (the armory all-NONE accept leaves the table bare).
    loadout.spawn_kit_set = true;
    loadout.spawn_kit = std::move(accepted);
    local_loadout_rebuild(world, loadout, weapon, inventory, inventory_valid,
            /*select_spawn_default=*/true);
    // The ACCEPT leg applies the REQUESTED ammo over the default seed: pool =
    // min(req, maxclips) * clipsize when requested, else startrounds RAW on
    // the main leg; the first different-class sub-variant takes ammo_secondary
    // with the x clipsize fallback [orig: @ 0x566166 vs @ 0x566209;
    // WeaponSlot_SetAmmoCount @ 0x540b50].
    const WeaponTable &table = world.tables.weapons;
    for (const WeaponKitEntry &entry : loadout.spawn_kit) {
        const int adm = table.index_of(entry.name.c_str());
        if (adm < 0) continue;
        const WeaponTableEntry *def = table.by_index(static_cast<uint8_t>(adm));
        if (def == nullptr) continue;
        if (def->clipsize != -1) {
            const int32_t total = entry.ammo_primary >= 0
                    ? std::min<int32_t>(entry.ammo_primary, def->maxclips) *
                            def->clipsize
                    : def->startrounds;
            weapon_pool_set(table, inventory, def->ammo_class_id, total);
        }
        for (int k = 1; k <= def->loadout_subclasses; ++k) {
            const WeaponTableEntry *sub = (adm + k < 256)
                    ? table.by_index(static_cast<uint8_t>(adm + k))
                    : nullptr;
            if (sub == nullptr) continue;
            if (strutil::iequals(sub->ammo_class, def->ammo_class)) continue;
            int32_t total = entry.ammo_secondary >= 0
                    ? std::min<int32_t>(entry.ammo_secondary, sub->maxclips)
                    : static_cast<int32_t>(sub->startrounds);
            // A nonnegative sub-weapon count is expressed in clips and expands
            // to rounds; a negative fallback is the no-clip sentinel and stays
            // raw. This is what keeps the implicit satchel detonator
            // switch-eligible. [orig: WeaponLoadout_ApplyFromBuffer
            // @ 0x5661E8..0x566215]
            if (total >= 0) total *= sub->clipsize;
            weapon_pool_set(table, inventory, sub->ammo_class_id, total);
            break; // the FIRST different-class sub-variant [orig: @ 0x5027c8 shape]
        }
    }
    weapon_inventory_recalc_clips(table, inventory);
    // The S2C 0x5A apply's last leg: the loadout weight into the local
    // entity's +0x37C (the run-promotion band) [orig:
    // NapiNPClientMsg_HandleWeaponLoadoutSync @0x4296f9 -> @0x425220].
    loadout.weight_fp16 = weapon_inventory_loadout_weight_fp16(table, inventory);
    return true;
}

// [orig: Armory_ResolveSelectedClass @0x5642f0] The scan-up + gunner fallback
// against the host allow mask; a class with its bit set opens as-is.
void weapon_slot_indices(const DefWeaponDef *rows, size_t count, int slot,
                         int32_t class_mask, int32_t team_mask,
                         std::vector<int32_t> &out) {
    out.clear();
    for (size_t i = 0; i < count; ++i) {
        const DefWeaponDef &w = rows[i];
        if (w.weapon_class_slot != slot) continue;
        // [orig: populate_weapon_slot_lists @0x560430] gate.
        if (w.loadout_selectable == 0) continue;
        if ((w.charfilter_mask & class_mask) == 0) continue;
        if ((w.teamfilter_mask & team_mask) == 0) continue;
        out.push_back(static_cast<int32_t>(i));
    }
}

int armory_resolve_selected_class(int player_class, uint32_t class_allow_mask) {
    // The &31 mirrors x86 shl's hardware count masking for an out-of-range
    // class byte (and keeps the C++ shift defined).
    int c = player_class;
    if (((1u << (c & 31)) & class_allow_mask) == 0u) {
        ++c;
        while (c <= 9 && ((1u << (c & 31)) & class_allow_mask) == 0u) ++c;
        if (c > 9) c = 7; // gunner
    }
    return c;
}

// [orig: Armory_ResolveSelectedClass @0x5642f0 switch] Classes 5..9 filter by
// their bit; the default arm is the all-ones mask (no filtering).
int32_t armory_class_filter_mask(int selected_class) {
    if (selected_class < 5 || selected_class > 9) return -1;
    return 1 << (selected_class - 5);
}

// [orig: PlayerInfo_SetTeamAndClassMask @0x55de60 — g_playerInfoTeamMask =
// 2 - (team != 0)]
int32_t player_info_team_mask(int team) {
    return team == 0 ? 2 : 1;
}

// [orig: @0x55de60 — 5->1, 6->2, 7->4, 8->8, 9->16; out of range masks
// nothing on this screen]
int32_t player_info_class_mask(int playerclass_value) {
    if (playerclass_value < 5 || playerclass_value > 9) return 0;
    return 1 << (playerclass_value - 5);
}

// [orig: populate_ammo_combo_boxes @0x55def0; armory fill @0x565cd0 — the
// `saved == i || (saved == -1 && i == maxclips)` row select]
int player_info_default_clip_row(int saved, int maxclips) {
    if (maxclips <= 0) return 0;
    if (saved > 0) return saved < maxclips ? saved : maxclips;
    return maxclips;
}

int player_info_default_grenade_row(int saved, int maxclips) {
    if (maxclips <= 0) return 0;
    if (saved < 0) return maxclips;
    return saved < maxclips ? saved : maxclips;
}

} // namespace opennova::world
