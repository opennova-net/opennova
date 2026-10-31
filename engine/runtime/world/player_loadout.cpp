// The local player's loadout orchestration — witness map in player_loadout.h.
#include <runtime/world/player_loadout.h>

#include <algorithm>

#include <base/io/log.h>
#include <base/io/strutil.h>

#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
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
    loadout.mission_kit_promoted = !kit.empty();
    loadout.spawn_kit() = std::move(kit);
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
    return loadout.mission_kit_promoted;
}

std::vector<WeaponKitEntry> local_loadout_kit(const World &world,
                                              const LocalPlayerLoadout &loadout) {
    (void)world;
    return loadout.spawn_kit();
}

bool local_loadout_set_player_class(World &world, LocalPlayerLoadout &loadout,
                                    int player_class) {
    if (player_class < 5 || player_class > 9) return false;
    Entity *e = world.registry.get(world.cached.local_player);
    if (e == nullptr) {
        loadout.pending_player_class = player_class;
        return true;
    }
    e->player_class = static_cast<uint8_t>(player_class);
    return true;
}

namespace {

// The select of the current slot word and the mount of what it selected, the tail
// Player_InitPlayer and both loadout applies share: Player_SelectWeaponSlot(word), then
// the EquippedSlot's mount [orig: Player_InitPlayer @0x4E15F0 (the select @0x4E1995, its
//  read of the word @0x4E198E); NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (the
//  select @0x4296E3, Player_MountWeaponSlot @0x4296E8..0x4296F4); WeaponLoadout_ApplyFromBuffer
//  @0x565CD0 (the select @0x566285, the mount @0x56628A..0x566297)].
void select_current_slot(World &world, LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                         bool mount) {
    // No walk here: both loadout applies end on the select and the mount alone, so a running
    // PowerThrow windup survives them; the walk that clears it is Player_InitPlayer's
    // (local_loadout_rebuild) [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566285,
    //  @0x566297); NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (@0x4296E3, @0x4296F4)].
    const WeaponTable &table = world.tables.weapons;
    const WeaponSwitchGates gates =
            local_weapon_switch_gates(world, weapon, &inventory);
    // An empty table (an empty buffer, the armory all-NONE kit) selects nothing, and
    // the body keeps its equipped byte: the no-slot arm stores no +0x2B0 [orig:
    // Player_SelectWeaponSlot @0x4DD680 - the global walk's return 0 @0x4DD794..0x4DD79C;
    // Player_SwitchToWeaponByHandle @0x4E0170 stores none].
    if (!weapon_select_slot(table, inventory, inventory.current_slot,
                !gates.equip_blocked))
        return;
    // The select cleared the NVG scope restore latch [orig:
    // Player_SelectWeaponSlot @0x4dd730 / @0x4dd7fe, every successful select;
    // called @0x4e1995 (Player_InitPlayer), @0x566285
    // (WeaponLoadout_ApplyFromBuffer), @0x4296e3 (the 0x5A apply)].
    weapon.nvg_scope_restore = false;
    inventory.pending_combo = inventory.equipped_combo;
    // A select writes none of the mount's view words: the loadout applies' mount request
    // below writes them, Player_InitPlayer's walk its camera reset's (local_loadout_rebuild)
    // [orig: Player_SelectWeaponSlot @0x4DD680].
    commit_pending_weapon_switch(world, weapon, &inventory, PendingCommit::kSelect);
    if (!mount) return;
    // The loadout applies then mount the EquippedSlot the select stored, through the mount's
    // own leg: its def's category is its own, so the queue is SWITCHRANK on that slot (none
    // while the lower / raise interp runs), the six view-bias words clear, and the
    // completion stores the same slot again (commit_pending_weapon_switch): no draw, no
    // raise (D-WPN-21's select plays none). With no local body (a joiner's grant ahead of
    // L) there is no EquippedSlot to mount [orig: Player_MountWeaponSlot @0x4DFA40 - the
    // EquippedSlot test @0x4DFA63..0x4DFA71].
    // [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (the select @0x4296E3, the
    //  mount of g_LocalPlayerEntity->EquippedSlot @0x4296E8..0x4296F4);
    //  WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566285, @0x56628A..0x566297);
    //  Player_MountWeaponSlot @0x4DFA40 (the store @0x4DFB16, the compare @0x4DFB8B,
    //  WeaponSlot_TryQueueSwitchRank @0x4DFB96, the bias clear @0x4DFBCF..0x4DFBE8)]
    LocalPlayer *local = world.local_player_state;
    if (local == nullptr || &local->weapon != &weapon || !weapon.active ||
            weapon.presentation_pending)
        return;
    local_player_mount_weapon_slot(world, weapon, inventory, local->view, inventory.equipped_combo);
}

// The AdmDef index of the weapon a loadout apply selects (LoadoutApplySelect carries the
// witness): the body's held one, or `kit`'s last primary row, from AdmDef 0.
uint8_t loadout_apply_select_adm(const World &world, const std::vector<WeaponKitEntry> &kit,
                                 LoadoutApplySelect select) {
    if (select == LoadoutApplySelect::HeldWeapon) {
        const Entity *holder = world.registry.get(world.cached.local_player);
        return holder != nullptr ? holder->equipped_adm_index : kAdmSlotNone;
    }
    uint8_t select_adm = 0;
    for (const WeaponKitEntry &entry : kit) {
        const int adm = world.tables.weapons.index_of(entry.name.c_str());
        if (adm < 0) continue;
        const WeaponTableEntry *def = world.tables.weapons.by_index(static_cast<uint8_t>(adm));
        if (def != nullptr && def->weapon_class_slot == 1) select_adm = static_cast<uint8_t>(adm);
    }
    return select_adm;
}

// The apply's select target: the selected weapon's combo (its AdmDef's rank + 65 * category)
// stored into the current slot word, no def leaving the word, which the select then reads;
// then the select and the mount.
// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (AdmDef_GetEntryByIndex(+0x2B0)
//  @0x4296B4..0x4296BC, the store @0x4296C8..0x4296D4, the word's read @0x4296DC);
//  WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566250..0x566276, the word's read @0x56627E)]
void store_select_and_mount(World &world, LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                            uint8_t select_adm) {
    if (const WeaponTableEntry *selected = world.tables.weapons.by_index(select_adm))
        inventory.current_slot =
                selected->category * weapon_combo::kRanksPerCategory + selected->rank;
    select_current_slot(world, weapon, inventory, /*mount=*/true);
}

// The rebuild over `kit`: Player_InitPlayer's weapon leg reads the spawn buffer
// (local_loadout_rebuild), a loadout apply's the kit it applies, which the authority's own
// S2C 0x5A apply builds from its list's rows without copying them into the spawn buffer
// (local_loadout_apply_own_grant).
void rebuild_from_kit(World &world, LocalPlayerLoadout &loadout, LocalPlayerWeapon &weapon,
                      WeaponInventory &inventory, bool &inventory_valid,
                      const std::vector<WeaponKitEntry> &kit, bool select_spawn_default) {
    // [orig: Player_InitPlayer @ 0x4e15f0 — the full leg sequence in the
    //  header note.] Entity-optional: a joiner rebuilds before L spawns. It writes
    //  none of the slot's damage-class bytes: their writers are the 0x2F accept
    //  and the out-of-session armory apply alone (D-NET-691) [orig: the four
    //  +0x15E58 sites: Weapon_CalcImpactDamage @0x4ECA24, Server_SendWeaponSlotListToPlayer
    //  @0x502871, the accept @0x515E92, WeaponLoadout_ApplyFromBuffer @0x56618C].
    Entity *e = world.registry.get(world.cached.local_player);
    // The current slot word starts every Player_InitPlayer at 195, whatever the
    // select below finds [orig: Player_InitPlayer @0x4E15F0 (g_CurrentWeaponSlot = 195
    //  @0x4E17FF)].
    inventory.current_slot = weapon_combo::kDefaultSpawnCombo;
    const WeaponTable &table = world.tables.weapons;
    if (table.empty()) return;
    inventory.reset(table);
    const std::vector<std::string> display =
            weapon_kit_expand_display_list(kit, table);
    // The local player owns every slot the fill seeds: its class feeds the
    // zoom seed's sniper lock. The permission is the fill's value: the
    // session byte (g_SessionAllowSniperScopeZoom) on an in-session peer, else the config's own
    // allowSniperScopeZoom (World::rules.sniper_zoom_fill) [orig:
    //  WeaponSlot_InitFromDef @ 0x53EF07..0x53EF25; Config_SetDefaults
    //  @ 0x54D364].
    const uint8_t seed_class = loadout_seed_class(world, loadout);
    const WeaponFillResult fill = weapon_inventory_load_from_display(table, display,
            inventory, seed_class, world.rules.sniper_zoom_fill,
            world.rules.title().scope_zero_seed);
    for (const std::string &w : fill.warnings)
        io::logf(io::LogLevel::kWarn, "%s", w.c_str()); // [orig: ErrorLog_WriteTimestamped]
    weapon_inventory_seed_pools(table, inventory, seed_class);
    // The redraw and the weight read through the player's seat (an unseated
    // spawn reads its own tables) [orig: WeaponSlots_RecalculateAmmoFromCapacity
    //  @0x54228D; WeaponSlot_GetTotalClips @0x542638 / @0x542651].
    const WeaponAmmoSeat seat = e != nullptr ? entity_ammo_seat(world, *e) : WeaponAmmoSeat{};
    weapon_inventory_recalc_clips(table, inventory, seat);
    // The loadout weight the next 0x5A apply would stamp into entity+0x37C
    // [orig: Terrain_AccumulateSectorScores @0x425220 via @0x4296f9].
    loadout.weight_fp16 = weapon_inventory_loadout_weight_fp16(table, inventory, seat);
    if (e != nullptr) e->carry_flags = (e->carry_flags & ~0x18u) | inventory.carry_flags;
    inventory_valid = true;
    weapon.switch_in_flight = false;
    weapon.switch_deferred_action = -1;
    if (!select_spawn_default) return;
    // Player_InitPlayer's own view and weapon words start cleared: its memset of the
    // local-player block and its two interps' clears precede its weapon leg; the loadout
    // applies run no Player_InitPlayer [orig: Player_InitPlayer @0x4E15F0 (the memset
    //  @0x4E1602, the interps @0x4E1609..0x4E1722); local_player_init_player_clears].
    LocalPlayer *local = world.local_player_state;
    PlayerViewState *view =
            local != nullptr && &local->weapon == &weapon ? &local->view : nullptr;
    if (view != nullptr) local_player_init_player_clears(weapon, *view);
    // The select of the word [orig: Player_InitPlayer @0x4E15F0 (the
    //  Player_SelectWeaponSlot(g_CurrentWeaponSlot) read @0x4E198E, the call @0x4E1995)].
    select_current_slot(world, weapon, inventory, /*mount=*/false);
    // Then its walk on the word, whatever the select found: past its seat gate it clears a
    // running PowerThrow windup, and past the EquippedSlot's action gate it resets the camera
    // (the scope interp cleared, hipfire, an optic EquippedSlot bound, the fov target 80, the
    // binocular clears, the NVG latch).
    // [orig: Player_InitPlayer @0x4E15F0 - Player_SwitchToWeaponByHandle @0x4E19A1 (the windup
    //  clear @0x4E019B, the gates @0x4E017D..0x4E01F6, Player_ResetCameraAndMovementState
    //  @0x4E01FC); local_weapon_walk_clears_windup; weapon_switch_walk_resets_view]
    local_weapon_walk_clears_windup(world, weapon);
    if (view == nullptr ||
            !weapon_switch_walk_resets_view(table, inventory, inventory.current_slot,
                    local_weapon_switch_gates(world, weapon, &inventory)))
        return;
    local_player_camera_reset(&world, weapon, *view);
    // Past the reset the walk binds the interp again, as the reset did, under the body's AI
    // slot (+0x68, which every body's spawn allocates), and mounts only a body whose Flags &
    // 3 is clear. Player_InitPlayer clears the body's hidden bit only after its walk, and the
    // mission start's round init spawns every player body hidden (a joiner's body takes its
    // 0x0C record's Flags, the host's hidden body's), so the mission start's walk mounts
    // nothing: the spawn equips the selected slot and plays no switch action. A remote
    // joiner's team assign re-runs Player_InitPlayer on a body in play (D-WPN-21).
    // [orig: Player_SwitchToWeaponByHandle @0x4E0170 - the +0x68 test @0x4E023A, the rebind
    //  @0x4E024D..0x4E025F, the Flags test @0x4E0265 / @0x4E0269, the mount @0x4E0362;
    //  Player_InitPlayer @0x4E15F0 - the clear @0x4E19C2; Game_StartMission @0x524360 -
    //  Server_InitAllPlayerEntitiesForRound @0x525BAF ahead of Player_InitPlayer @0x525BBC;
    //  Server_InitAllPlayerEntitiesForRound @0x516AA0 - Entity_SpawnFromAnimSlotProperty
    //  @0x516B67; Entity_SpawnFromAnimSlotProperty @0x43C390 - Entity_AllocateAISlot @0x43C48C,
    //  Flags |= 0x101 @0x43C508; Entity_AllocateAISlot @0x40D2C0 - the +0x68 store @0x40D2F4;
    //  NapiNPClientMsg_0x00C @0x42E730 - Entity_AllocateAISlot @0x42E81E, Flags @0x42E91A;
    //  NapiNPClientMsg_TeamAssign @0x431910 - Player_InitPlayer(1) @0x431B14]
}

} // namespace

void local_loadout_rebuild(World &world, LocalPlayerLoadout &loadout,
                           LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                           bool &inventory_valid, bool select_spawn_default) {
    // Player_InitPlayer's weapon leg over the spawn buffer [orig: Player_InitPlayer @0x4E15F0 -
    //  AvatarDef_BuildDisplayList(restrictionData) @0x4E1882].
    rebuild_from_kit(world, loadout, weapon, inventory, inventory_valid,
            local_loadout_kit(world, loadout), select_spawn_default);
}

void local_loadout_rebuild_on_team_assign(World &world, LocalPlayerLoadout &loadout,
                                          LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                                          bool &inventory_valid) {
    // [orig: NapiNPClientMsg_TeamAssign @0x431910 - Player_InitPlayer(1) @0x431B14; the
    //  declaration carries the walk]
    local_loadout_rebuild(world, loadout, weapon, inventory, inventory_valid,
            /*select_spawn_default=*/true);
    LocalPlayer *local = world.local_player_state;
    if (local == nullptr || &local->weapon != &weapon) return;
    // The walk past its reset: the body's AI slot, then its hidden and dead bits
    // [orig: Player_SwitchToWeaponByHandle @0x4E0170 - @0x4E023A, @0x4E0265 / @0x4E0269].
    const Entity *body = world.registry.get(world.cached.local_player);
    if (body == nullptr || world.ai.for_handle(body->handle) == nullptr) return;
    if (((body->flags | body->engine_flags) & (kEntityFlagCarried | kEntityFlagDead)) != 0)
        return;
    // Its rank cycle and mount (or deny), the weapon key's walk past the same gates, the
    // camera reset already run [orig: @0x4E026F..0x4E0362].
    WeaponSwitchOutcome out = weapon_switch_to_handle(world.tables.weapons, inventory,
            inventory.current_slot, local_weapon_switch_gates(world, weapon, &inventory));
    out.reset_view = false;
    handle_weapon_switch_outcome(world, weapon, &inventory, out, local->view);
}

void local_loadout_rebuild_after_start(World &world, LocalPlayerLoadout &loadout,
                                       LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                                       bool &inventory_valid) {
    // [orig: Game_StartMission @0x524360; the declaration carries the order]
    int32_t &fov = world.weather.core.scalar_channels.camera_fov_target_fp;
    const int32_t started = fov;
    local_loadout_rebuild(world, loadout, weapon, inventory, inventory_valid,
            /*select_spawn_default=*/true);
    fov = started;
}

namespace {

// The apply over `kit`, local_loadout_apply_accept's and, `own_grant`, the authority's own S2C
// 0x5A apply's: its rows build the table without becoming the spawn buffer.
bool apply_kit(World &world, LocalPlayerLoadout &loadout, LocalPlayerWeapon &weapon,
               WeaponInventory &inventory, bool &inventory_valid,
               const std::vector<WeaponKitEntry> &kit, int32_t player_class,
               bool validate_banned, LoadoutApplySelect select, bool own_grant) {
    // [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline leg.] A joiner
    // applies its kit before L exists; the inventory is sim-side state and the
    // entity stamps (class + equipped adm) defer to the joiner spawn block.
    // The armory ACCEPT (validate_banned; the 0x5A grant's apply passes false and its
    // rebuild is inmatch::apply_weapon_loadout_name_list's) copies the whole selection,
    // unfiltered, into the process's loadout name list ahead of its session test, so an
    // out-of-session ACCEPT writes it too: every body the process spawns later takes its
    // first weapon (D-NET-143) [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 (the class
    //  buffer copy @0x565D27..0x565D4B ahead of `cmp is_in_session` @0x565D3A's jz @0x565D4D)].
    if (validate_banned && world.loadout_weapon_names != nullptr) {
        world.loadout_weapon_names->entries = kit;
        world.loadout_weapon_names->set = true;
    }
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
    // The authority's own 0x5A apply skips the copy and builds from its rows: its spawn
    // buffer is the slot's buffer the list send copied [orig: `cmp is_authority` @0x4293D1,
    //  its jnz @0x4293D8 past the copy @0x4293E4; Server_SendWeaponSlotListToPlayer
    //  @0x502617..0x502627] [orig: @0x42B218 (dfx2.exe)].
    if (!own_grant) loadout.spawn_kit() = std::move(accepted);
    const std::vector<WeaponKitEntry> &applied = own_grant ? accepted : loadout.spawn_kit();
    // The apply runs no Player_InitPlayer: the current slot word keeps its value
    // through the rebuild's stamp, the rebuild selects nothing, and the selected weapon
    // (`select`: the body's held AdmDef index, or the kit's last primary row) decides the
    // word ahead of the apply's own select below.
    const uint8_t select_adm = loadout_apply_select_adm(world, applied, select);
    const int32_t current_slot = inventory.current_slot;
    rebuild_from_kit(world, loadout, weapon, inventory, inventory_valid, applied,
            /*select_spawn_default=*/false);
    inventory.current_slot = current_slot;
    // The rebuild's drawn clips are dropped, not returned: every loaded round of a def that
    // draws pool units is zeroed ahead of the counts, so the last draw takes its clip out of
    // the count's pool (a four-clip row leaves three in reserve and one loaded), as the S2C
    // 0x5A apply and the offline ACCEPT both do.
    // [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - the draw @0x429540,
    //  WeaponSlots_ClearLoadedRounds @0x42955B, the counts @0x4295FC, the draw @0x4296A9;
    //  WeaponLoadout_ApplyFromBuffer @0x565CD0 - WeaponSlots_ClearLoadedRounds @0x5660FD,
    //  the draw @0x56624B]
    // [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x42AF10 - WeaponOverlay_UpdateAllPerFrame @0x42B399;
    //  WeaponLoadout_ApplyFromBuffer @0x564C30 - WeaponOverlay_UpdateAllPerFrame @0x56505D (dfx2.exe)]
    {
        const Entity *cleared = world.registry.get(world.cached.local_player);
        weapon_inventory_clear_loaded_rounds(world.tables.weapons, inventory,
                cleared != nullptr ? entity_ammo_seat(world, *cleared) : WeaponAmmoSeat{});
    }
    // Out of a session the apply stores each entry's fourth value, raw, into the
    // local slot's damage-class bytes, clearing nothing; in a session it sends the
    // 0x2F instead and the accept stores (D-NET-691). [orig: WeaponLoadout_ApplyFromBuffer
    //  @0x565CD0 - is_in_session @0x565D3A (its jz @0x565D4D; the send @0x565D94), the local entity
    //  @0x565DA8, the store @0x566182..0x56618C]
    if (!world.rules.mp_session) {
        if (Entity *local = world.registry.get(world.cached.local_player))
            weapon_kit_store_damage_classes(applied, world.tables.weapons,
                    world.tables.ammo.entries.size(), local->ammo_damage_class);
    }
    // The ACCEPT leg applies the REQUESTED ammo over the default seed: pool =
    // min(req, maxclips) * clipsize when requested, else startrounds RAW on
    // the main leg; every sub-variant of another ammo class takes ammo_secondary
    // with the x clipsize fallback [orig: @ 0x566166 vs @ 0x566209;
    // WeaponSlot_SetAmmoCount @ 0x540b50; the walk over every sub-variant
    // @ 0x56621D..0x566233].
    const WeaponTable &table = world.tables.weapons;
    for (const WeaponKitEntry &entry : applied) {
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
            // The sub-variant's index wraps at 256: AdmDef_GetEntryByIndex reads only
            // its low byte [orig: WeaponLoadout_ApplyFromBuffer @ 0x5661CA (and the
            //  name-list leg @ 0x566008) -> @ 0x53FC80].
            const WeaponTableEntry *sub = table.by_index(static_cast<uint8_t>(adm + k));
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
        }
    }
    // Both read through the player's seat; a joiner applying before L
    // exists reads its own tables [orig: WeaponSlots_RecalculateAmmoFromCapacity
    //  @0x54228D; WeaponSlot_GetTotalClips @0x542638 / @0x542651].
    const Entity *seated = world.registry.get(world.cached.local_player);
    const WeaponAmmoSeat seat =
            seated != nullptr ? entity_ammo_seat(world, *seated) : WeaponAmmoSeat{};
    weapon_inventory_recalc_clips(table, inventory, seat);
    // The S2C 0x5A apply's last leg: the loadout weight into the local
    // entity's +0x37C (the run-promotion band) [orig:
    // NapiNPClientMsg_HandleWeaponLoadoutSync @0x4296f9 -> @0x425220].
    loadout.weight_fp16 = weapon_inventory_loadout_weight_fp16(table, inventory, seat);
    // The apply's select target into the current slot word, then the select and the mount
    // (store_select_and_mount carries the witness).
    store_select_and_mount(world, weapon, inventory, select_adm);
    return true;
}

} // namespace

bool local_loadout_apply_accept(World &world, LocalPlayerLoadout &loadout,
                                LocalPlayerWeapon &weapon,
                                WeaponInventory &inventory,
                                bool &inventory_valid,
                                const std::vector<WeaponKitEntry> &kit,
                                int32_t player_class, bool validate_banned,
                                LoadoutApplySelect select) {
    return apply_kit(world, loadout, weapon, inventory, inventory_valid, kit, player_class,
            validate_banned, select, /*own_grant=*/false);
}

bool local_loadout_apply_own_grant(World &world, LocalPlayerLoadout &loadout,
                                   LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                                   bool &inventory_valid, const std::vector<WeaponKitEntry> &kit,
                                   int32_t player_class, LoadoutApplySelect select) {
    // [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - the rebuild
    //  @0x4294E3..0x4296AE, the select and mount @0x4296B4..0x4296F4, the weight @0x4296FE]
    return apply_kit(world, loadout, weapon, inventory, inventory_valid, kit, player_class,
            /*validate_banned=*/false, select, /*own_grant=*/true);
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
        // The list's producer counts a row only at its `end` [orig:
        // WeaponDef_ParseProperty @0x54D730, the END compare]; an entry no
        // `end` closed is no row (DefWeaponDef::unclosed).
        if (w.unclosed != 0) continue;
        // [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430] gate.
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

// [orig: PlayerInfo_SetTeamAndClassMask @0x55de60 — g_PlayerInfoTeamMask =
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

// [orig: PlayerInfo_PopulateAmmoComboBoxes @0x55def0; armory fill @0x565cd0 — the
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
