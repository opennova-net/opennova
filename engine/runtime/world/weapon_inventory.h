// The per-player weapon slot pool, spawn-kit chain, map availability rules, and the
// manual weapon-switching walks — structural translations of the witnessed originals.
//
// The original keeps ONE local-player instance of all of this in globals (the 780-slot
// 100-B array weaponSlotArrayBase @ 0xB75FD4, the per-ammo-class pools g_LocalAmmoPools
// @ 0xB75FE8 + entity+288 for class 1, the 2048-B spawn-kit tuple buffer 'restrictionData'
// @ 0x24D4E00, the availability table g_ArmoryWeaponAvailability @ 0x24D5600) and one
// per-player copy server-side (slot+464 table / +94408 buffer / +88664 pools). This port
// gathers the local-player instance into value types; the server-side copy stays in
// engine/runtime/inmatch (D-NET-152 shape).
// [witness record: docs/net/novaworld-net-re.md §5.57/§5.58 + the loadout grill 2026-07-18]
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <runtime/world/weapon_table.h>

namespace opennova::world {

class World;
struct TitleRules;
struct Entity;

// Weapon-slot combo = category*65 + rank; 12 categories x 65 ranks = 780 slots
// [orig: slot = table + 100*(rank + 65*category) @ 0x5415D3; scans clamp @ 780].
namespace weapon_combo {
enum : int32_t {
    kRanksPerCategory = 65,
    kCategories = 12,
    kSlotCount = 780,
    // The engine's default spawn selection: category 3 (the Primary key), rank 0
    // [orig: Player_InitPlayer @ 0x4e17ff g_CurrentWeaponSlot = 195].
    kDefaultSpawnCombo = 195,
};
}

// Availability values [orig: g_ArmoryWeaponAvailability semantics — server 0x2F gate
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
// WeaponDef_BuildItemRestrictionTable @ 0x54DDB0: default 1 per entry, matched names take
// their pair value (-1 maps to 3 kMissionAllowed), and a parent's loadout_subclasses
// sub-entries INHERIT the parent's value (the skip_count walk). Pair source: the .mis
// item_availability chunk ({name, status} records). `mission_allowed` is the value -1 maps
// to: Jointops.exe's 3, dfx2.exe's 2 [orig: @0x54DE47..0x54DE49; dfx2.exe @0x54B0C7..0x54B0C9].
void weapon_availability_apply_pairs(
        WeaponAvailability &avail, const WeaponTable &table,
        const std::vector<std::pair<std::string, int32_t>> &pairs,
        int32_t mission_allowed = weapon_availability_value::kMissionAllowed);

// The template mode of the same builder: each entry takes its template word (-1 maps to
// `mission_allowed`), and a parent's loadout_subclasses sub-entries inherit the parent's
// value. [orig: WeaponDef_BuildItemRestrictionTable @0x54DDB0 (the template leg
// @0x54DE7E..0x54DF3D)]
void weapon_availability_apply_template(
        WeaponAvailability &avail, const WeaponTable &table,
        const std::array<int32_t, 255> &words,
        int32_t mission_allowed = weapon_availability_value::kMissionAllowed);

// One spawn-kit / loadout tuple {name, ammoPrimary, ammoSecondary, flags} — the
// 4-string record of the 2048-B loadout buffers ({name\0 ammoPri\0 ammoSec\0 flags\0}*)
// [orig: g_SpawnLoadoutBuffer @ 0x24D4E00 and the per-class buffers @ 0x25DD740 share the
// format; values default -1 = "engine default"].
struct WeaponKitEntry {
    std::string name;
    int32_t ammo_primary = -1;
    int32_t ammo_secondary = -1;
    int32_t flags = -1;
};

// g_LoadoutWeaponNameList, the process's loadout name list, a buffer apart from the spawn
// buffer (SpawnLoadoutBuffer): the image's initializer (the title's default kit,
// weapon_kit_default) until an armory ACCEPT copies the accepted class's buffer over it, on
// every leg in a session or out of one, or a 0x5A apply rebuilds it from the list's rows,
// the listen host's own client's included. Every player body the process spawns takes its
// first name as its equipped weapon (player_spawn_equipped_default). One per process
// (inmatch::ClientProcessState); a world reads it through World::loadout_weapon_names.
// [orig: g_LoadoutWeaponNameList @0x8333F8 (0x800 bytes, "WPN_M4AUTO" "-1" "-1" "-1" ..
//  "WPN_KNIFE"); PlayerClass_InitEntity @0x4B1100..0x4B1116; WeaponLoadout_ApplyFromBuffer
//  @0x565D27..0x565D4B; NapiNPClientMsg_HandleWeaponLoadoutSync @0x4291FC..0x4293BF]
struct LoadoutWeaponNameList {
    std::vector<WeaponKitEntry> entries;
    bool set = false; // false: the image's initializer
};

// g_SpawnLoadoutBuffer, the process's spawn buffer (restrictionData): zero in the image, so a
// fresh process's buffer is empty, and no session, mission start or load clears it; only its
// writers change it: out of a session the single player's fill (the mission's kit, else the
// profile's single-player page, else the static page weapon_kit_default) at the start and at
// the restart, in a session a peer's mission start copying its side's class page (an empty page
// copies empty), the mission load's kit chunk out of a session, the own slot's list send on the
// authority and the 0x5A apply off it. Every reader takes it as it stands: Player_InitPlayer's
// display list and select 195, the mission start's 0x2F pair and the team assign's resubmit, and
// the round init's own-slot seed; a dedicated start fills nothing, so a Serve Only process runs
// on what its earlier missions left. One per process (inmatch::ClientProcessState), which the
// role binds into the local player's loadout (LocalPlayerLoadout::process_buffer).
// [orig: g_SpawnLoadoutBuffer @0x24D4E00 (0x800 bytes); Game_ApplySessionSettingsToGlobals
//  @0x551500 (the dedicated start's jz @0x5517A7, the single player's fill
//  @0x5519A2..0x5519E4); Game_StartMission @0x524360 (the restart's copy @0x524683..0x5246C8, the
//  peer's side page @0x525767..0x525818, the 0x2F sends @0x52582F / @0x525C24);
//  Buffer_CopyUntilDoubleNull @0x562F30; Mission_LoadBMSFile @0x40F4E0 (@0x40F961);
//  Server_SendWeaponSlotListToPlayer @0x502550 (@0x502617..0x502627);
//  NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (@0x4293D1..0x4293E9);
//  NapiNPClientMsg_TeamAssign (@0x431AA3); Server_InitAllPlayerEntitiesForRound @0x516AA0
//  (@0x516BC4..0x516BE3); Player_InitPlayer @0x4E15F0 (@0x4E1872..0x4E1882, @0x4E1995)]
struct SpawnLoadoutBuffer {
    std::vector<WeaponKitEntry> entries;
};

// The engine defaults: the profile-less spawn kit, the static single-player page
// blob the title compiles in (`rules`' spawn_kit; Jointops.exe's is
// playersav::default_single_player_page; dfx2.exe's names WPN_MK23 for
// WPN_colt45), and the everything-filtered fallback [orig: the page at 0x833BF8
// @ 0x5246be/@ 0x5519e4; the {"WPN_KNIFE", "-1","-1","-1"} synthesis @ 0x40f899]
// [orig: the page at 0x832368 @0x52188f/@0x54ec2a (dfx2.exe)].
std::vector<WeaponKitEntry> weapon_kit_default(const TitleRules &rules);
std::vector<WeaponKitEntry> weapon_kit_knife_fallback();

// The SP mission-load filter [orig: Mission_LoadBMSFile @ 0x40f7ae..0x40f95c]: keep
// only entries whose weapon resolves in the table AND has nonzero availability; if
// NOTHING survives, return the knife fallback. (Unresolved names drop silently — the
// original's catalog walk just skips them @ 0x40f830.)
std::vector<WeaponKitEntry> weapon_kit_filter_by_availability(
        const std::vector<WeaponKitEntry> &kit, const WeaponTable &table,
        const WeaponAvailability &avail);

// The out-of-session armory apply's stores into the slot's damage-class bytes, the
// table Weapon_CalcImpactDamage reads: each kit row whose name resolves stores the low
// byte of its fourth value, raw, at its weapon's round ammo index; nothing is cleared
// first (the table grows to `ammo_count` with zeros). A weapon with no round keeps
// retail's -1 index, whose store aliases the bytes before the table: dropped
// (D-NET-692). [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 (the name test
// @0x565E41, the atol @0x565F3D, the store @0x566182..0x56618C)]
void weapon_kit_store_damage_classes(const std::vector<WeaponKitEntry> &kit,
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
    int16_t scope_zero = 0;
    // The zero pitch and yaw (MountSlot+4 / +8) the init seeds (weapon_scope_zero_slot_seed) and
    // the local zero adjust writes; a mount takes them as they stand.
    int32_t zero_pitch = 0;
    int32_t zero_yaw = 0;
    // The slot's scope zoom (MountSlot+0xC). The personal slot IS this entry
    // in retail (EquippedSlot +0x118 points into the slot table), so a mount
    // reads back the zoom the slot was left at.
    int32_t scope_zoom = 0;
};

// The class-6 sniper lock WeaponSlot_InitFromDef applies to an OWNED slot's zoom
// floor: an owner of player class 6 on a category-3 (Primary) def floors the seed
// at scope_max_mag unless sniper scope zoom is allowed. The permission is the
// session's byte in a session and the config's own option offline; the caller
// passes it. A slot initialized without an owner (the emplacement's own slot)
// never locks [orig: WeaponSlot_InitFromDef @ 0x53EEF1..0x53EF27 — entityPtr 0
// skips @ 0x53EEF7, class @ 0x53EEF9, category @ 0x53EF02, g_SessionAllowSniperScopeZoom
// @ 0x53EF17 / allowSniperScopeZoom_5EC @ 0x53EF1F].
bool weapon_slot_zoom_sniper_lock(int32_t owner_class, int32_t def_category,
                                  bool allow_sniper_scope_zoom);

// The slot's initial scope zoom, MountSlot+0xC [orig: WeaponSlot_InitFromDef
// @ 0x53EF2D..0x53EF44]: the def's second scope_max_mag value; below the floor
// (scope_min_mag, or scope_max_mag under the sniper lock) it takes the floor
// unchecked against the max (`jl` @ 0x53EF38), otherwise it is capped at
// scope_max_mag. So an absent second value starts every optic at its floor.
int32_t weapon_slot_initial_zoom(int32_t scope_max_mag, int32_t scope_initial_mag,
                                 int32_t scope_min_mag, bool sniper_lock);

// The slot init [orig: WeaponSlot_InitFromDef @ 0x53EE70]: the 100-byte slot
// zeroed (clip 0) and bound to `adm_index`, its scope zero words seeded under the
// title's `zero_seed` (TitleRules::scope_zero_seed, DFX2-120) and its zoom with
// `owner_class` as the owner (the sniper lock above).
void weapon_slot_init_from_def(const WeaponTableEntry &def, int16_t adm_index,
                               WeaponInventorySlot &slot, int32_t owner_class,
                               bool allow_sniper_scope_zoom, ScopeZeroSeed zero_seed);

struct WeaponInventory {
    std::array<WeaponInventorySlot, weapon_combo::kSlotCount> slots;
    // Per-ammo-class carried pools, keyed by WeaponTableEntry::ammo_class_id. The
    // original splits storage (class 1 = entity+288 u16, others = the pool array);
    // the arithmetic (per-class cap clamp) is identical and the split is not
    // observable, so one array carries all classes here (D-WPN-24).
    // [orig: g_LocalAmmoPools @ 0xB75FE8; entity+288 @ 0x540ba5; caps @ 0x24E7DE0]
    std::vector<int32_t> pools;
    // Shared loaded rounds, distinct from carried reserves [orig: WeaponSlot_GetSharedLoadedRounds
    // @0x5405F0 / WeaponSlot_SetSharedLoadedRounds @0x540670, local table @0xB761E8].
    std::vector<int32_t> shared_clips;
    int32_t equipped_combo = -1; // [orig: EquippedSlot +0x118]
    // g_CurrentWeaponSlot, the local player's current slot word: a word apart from the
    // EquippedSlot, which no select writes. Player_InitPlayer stamps 195 (the spawn's
    // select then reads it); the category walk, the cycle and the quick-switch equip
    // store the slot they mount; the 0x5A apply, the single player's armory apply and
    // the listen host's own list send store the held weapon's combo (its AdmDef's
    // rank + 65 * category) when the body holds one and otherwise read the word for
    // their select. The 0x2F sends carry it (the mission start's second, the in-session
    // ACCEPT's) and a detach without a saved slot of category < 11 mounts it. So a
    // spawn whose select misses slot 195 (no category-3 weapon, or no weapon) keeps 195
    // here while the EquippedSlot names another slot or none. It starts at the 195 the
    // first Player_InitPlayer stamps (every reader runs after one; the image's 0 is never
    // read). The pool resets never write it.
    // [orig: g_CurrentWeaponSlot @0xB76474 - Player_InitPlayer @0x4E15F0 (the store
    //  @0x4E17FF, the select and switch @0x4E198E / @0x4E199A);
    //  Player_SwitchToWeaponByHandle @0x4E0170 (@0x4E035C); Player_CycleWeaponSlot
    //  @0x4DFE70 (@0x4DFF4A); Player_EquipWeaponByEntity @0x4E0370 (@0x4E03A1..0x4E03B2);
    //  NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (@0x4296BC..0x4296E3);
    //  WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566250..0x566285, the 0x2F's read
    //  @0x565D73); Server_SendWeaponSlotListToPlayer @0x502550 (@0x5028D5..0x5028FC);
    //  Game_StartMission @0x524360 (@0x525C1A); Entity_DetachFromVehicle @0x4355F0
    //  (@0x435650)]
    int32_t current_slot = weapon_combo::kDefaultSpawnCombo;
    // The slot a mount stages for the switch-out's completion to commit.
    // [orig: g_PendingWeaponSlot, Player_MountWeaponSlot @0x4DFB16]
    int32_t pending_combo = -1;
    // entity+0x308: the last SELECTED slot (a select's own store, whether or not
    // the seat lets it reach EquippedSlot), or the personal slot a borrowed seat's
    // attach saved; -1 for none or a slot outside the personal table (a borrowed
    // carrier's). A key switch and a cycle mount without selecting, and a detach
    // never clears it. [orig: Player_SelectWeaponSlot @0x4DD6DA / @0x4DD7A8 /
    // @0x4DD81C; Entity_AttachToVehicleSlot @0x494831; Entity_AttachToUseGunSlot
    // @0x546C31 / @0x546C48; read by Entity_DetachFromVehicle @0x43563A]
    int32_t selected_combo = -1;
    // entity+0x308 when it names a borrowed gun's slot instead: the packed handle
    // of the mount whose slot a borrowing attach saved because the EquippedSlot
    // was that gun's (a gun held on foot, or one a seat switch's detach had not
    // yet switched out), 0xFFFF when +0x308 is selected_combo's personal slot or
    // none. Every select's store replaces it. [orig: Entity_AttachToVehicleSlot
    // @0x49481D..0x494831; Entity_AttachToUseGunSlot @0x546BCF..0x546C31;
    // Player_SelectWeaponSlot @0x4DD6DA / @0x4DD7A8 / @0x4DD81C]
    uint16_t selected_mount = 0xFFFF;
    // entity+44 carry-presentation bits gathered by the fill (8 = def.flags&0x1000,
    // 0x10 = def.flags2&2) [orig: WeaponSlotTable_LoadAllFromDefs @ 0x5415aa/0x5415bc].
    uint32_t carry_flags = 0;
    // The ToSpecial hold-swap's two slots (input case 220): the target, the
    // first slot whose def carries QuickSwitch (weapon.def flags 0x08000000;
    // retail JO authors it only on WPN_MAG58_PointAim), and the stash, the slot
    // the press swapped away from until the release swaps back; -1 = none.
    // [orig: 0xB75FE0 / 0xB75FE4 (IDB-misnamed g_BinocularsWeaponSlot /
    //  g_BinocularsStashedSlot), set by the local fill's tail
    //  WeaponSlotTable_LoadAllFromDefs @ 0x541626..0x54166E]
    int32_t quick_switch_combo = -1;
    int32_t quick_switch_stash = -1;

    void reset(const WeaponTable &table) {
        slots.fill(WeaponInventorySlot{});
        pools.assign(table.ammo_class_names.size(), 0);
        shared_clips.assign(table.ammo_class_names.size(), 0);
        equipped_combo = -1;
        pending_combo = -1;
        selected_combo = -1;
        selected_mount = 0xFFFF;
        carry_flags = 0;
        quick_switch_combo = -1;
        quick_switch_stash = -1;
    }
    // The slot pool's reset: every entry zeroed, and nothing else: the current
    // slot word, the pending and selected slots, the ammo pools and the shared
    // clips keep their values (an in-session joiner's armory ACCEPT runs it
    // ahead of its 0x2F; the 0x5A grant's select replaces the current slot).
    // [orig: WeaponSlotPool_ResetAllEntries @0x53F240 (the 780 entries
    //  @0x53F24C..0x53F284; the owner word +0x24 has no field here)]
    void reset_slot_entries() { slots.fill(WeaponInventorySlot{}); }
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

// Loaded-round access selects def+0xDC when nonzero, otherwise the slot word.
int32_t weapon_inventory_loaded_rounds(const WeaponTable &table,
        const WeaponInventory &inv, int32_t combo);
void weapon_inventory_set_loaded_rounds(const WeaponTable &table,
        WeaponInventory &inv, int32_t combo, int32_t rounds);

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
// carry bits, and (the local player's fill: its only caller here) clear the
// ToSpecial stash and take the first QuickSwitch slot as its target. Unresolved names append a warning ("couldn't find wpn %s" shape).
// Each landed slot takes the zoom seed with the local player as its owner
// (owner_class, the sniper permission) [orig: WeaponSlotTable_LoadAllFromDefs
// @ 0x5414E0, entityPtr @ 0x5414FC passed to the WeaponSlot_InitFromDef call
// @ 0x5415E4].
struct WeaponFillResult {
    std::vector<std::string> warnings;
};
WeaponFillResult weapon_inventory_load_from_display(
        const WeaponTable &table, const std::vector<std::string> &display,
        WeaponInventory &inv, int32_t owner_class, bool allow_sniper_scope_zoom,
        ScopeZeroSeed zero_seed);

// The spawn pool seeding [orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x541690, ex the
// 'WeaponOverlay_BuildTypeLookup' misnomer]: every pool zeroed, then for every populated slot,
// pools[def.ammo_class_id] = classrounds[value(player_class)] when the def authors one,
// else startrounds. player_class 5..9 -> classrounds indices 1/2/3/5/6; other classes
// take plain startrounds. Walk order is slot order, later slots overwrite (witnessed).
void weapon_inventory_seed_pools(const WeaponTable &table, WeaponInventory &inv,
                                 int player_class);

// The joiner's S2C 0x0F pool apply [orig: NapiNPClientMsg_0x00F @ 0x42e324..0x42e34a —
// the 128 wire dwords land in g_LocalAmmoPools verbatim (every entry, zeros
// included; indexed by the retail ammo-class id), then
// WeaponSlots_RecalculateAmmoFromCapacity @ 0x42e424 returns each slot's clip and
// re-draws it from the authority's pools]. Entries past the table's class count
// have no home here and are dropped.
void weapon_inventory_apply_authority_pools(const WeaponTable &table, WeaponInventory &inv,
                                            const std::array<int32_t, 128> &pools);

// The slot's TOTAL AMMO IN CLIPS on the LIVE inventory [orig:
// WeaponSlot_GetTotalClips @ 0x5425F0 — (class pool + loaded rounds) / clipsize;
// clipsize -1 returns -1; clamp 127; def+0xDC selects shared loaded rounds].
// Unpopulated slot -> 0.
int32_t weapon_inventory_total_clips(const WeaponTable &table, const WeaponInventory &inv,
                                     int32_t combo);

// The loadout weight the S2C 0x5A apply stores into the local entity's +0x37C
// [orig: the misnamed Terrain_AccumulateSectorScores @ 0x425220, called
//  @0x4296f9 after Player_MountWeaponSlot]: over the 780 slots in combo order,
//  weaponweight + total clips x clipweight for every populated slot, plus — for
//  each of the def's +0x3AC sub-variant entries whose ammo class DIFFERS from
//  the main's — that sub slot's total clips x the MAIN def's clipweight; the walk
//  then skips the sub-variant slots. 16.16 units.
int32_t weapon_inventory_loadout_weight_fp16(const WeaponTable &table,
                                             const WeaponInventory &inv);

// The clip normalization [orig: WeaponSlots_RecalculateAmmoFromCapacity @ 0x542280]:
// for each populated slot with ammo_class_count (pool-units/round) nonzero and
// clipsize != -1: return the current clip to the pool, then draw one full clip
// clamped by what the pool affords; def+0xDC selects the shared loaded-round bucket.
void weapon_inventory_recalc_clips(const WeaponTable &table, WeaponInventory &inv);

// The owner's seat as the slot-table ammo accessors read it: entity+0x168 and the
// seated carrier (entity+0x16C). A UseGun seat (3) reads each pool as the carrier's
// MountSlot reserve (ammo class 1 as its +0x120 word) and each loaded-round bucket as
// its MountSlot clip, writes no bucket, and its redraw returns at once; a controller
// seat (2) reads each bucket as 0 and writes none, spends nothing, and reads each pool
// as the carrier's reserve when the carrier is an EWeap item, else 0 (ammo class 1
// always 0). Every other seat reads the owner's own tables. The carrier's reserve and
// clip are its one MountSlot's here (retail reads its +710 / +708 words, or +1158 /
// +1156 under its def's PlayerControl bit; 0 without an item def).
// [orig: Entity_GetScoreValueBySlotType @0x5406E0 - the carrier @0x540707..0x54071A,
//  its EWeap bit @0x540721..0x540739, the class-1 leg @0x54073F..0x540763, Entity_GetMountSlotReserve
//  @0x540764..0x540774, seat 2 -> 0 @0x54078F..0x540799; WeaponSlot_GetSharedLoadedRounds @0x540604..0x540627
//  (Entity_GetMountSlotClip @0x5461A0); WeaponSlot_SetSharedLoadedRounds @0x540684..0x540692; WeaponSlot_DecrementAmmo
//  @0x540920; WeaponSlots_RecalculateAmmoFromCapacity @0x54228D..0x542294]
struct WeaponAmmoSeat {
    int32_t parent_slot = 0;      // entity+0x168: 2 controller, 3 UseGun
    bool carrier = false;         // a seated carrier exists
    bool carrier_eweap = false;   // its item def's EWeap bit
    int32_t carrier_reserve = 0;  // Entity_GetMountSlotReserve: its MountSlot reserve word
    int32_t carrier_clip = 0;     // Entity_GetMountSlotClip: its MountSlot clip word
    int32_t carrier_class1 = 0;   // its +0x120 word
    // While the infinite-ammo cheat's bit is up (World::rules.ignore_weapon_ammo_cost), the
    // class carry caps every pool read returns ahead of the seat legs, whatever the owner;
    // null while it is down.
    const std::vector<int32_t> *cheat_carry_caps = nullptr;
};
// The entity's seat, read off its mount, with the cheat's caps while its bit is up
// [orig: entity+0x168 / +0x16C].
WeaponAmmoSeat entity_ammo_seat(const World &world, const Entity &entity);
// A pool read through the seat; `own` is the owner's own pool for the class. Under the
// infinite-ammo cheat it is the class's carry cap (0 past the table).
// [orig: Entity_GetScoreValueBySlotType @0x5406E0 - `test g_DebugCheatFlags, 100h` @0x5406E3,
//  g_AmmoClassMaxCarry[class] @0x5406F4]
int32_t weapon_seat_pool(const WeaponAmmoSeat &seat, int class_id, int32_t own);
// A loaded-round bucket read through the seat; `own` is the owner's own bucket.
// [orig: WeaponSlot_GetSharedLoadedRounds @0x5405F0]
int32_t weapon_seat_bucket(const WeaponAmmoSeat &seat, int32_t own);
// Whether the seat writes the owner's buckets [orig: WeaponSlot_SetSharedLoadedRounds @0x540684..0x540692].
bool weapon_seat_writes_buckets(const WeaponAmmoSeat &seat);
void weapon_inventory_recalc_clips(const WeaponTable &table, WeaponInventory &inv,
                                   const WeaponAmmoSeat &seat);
// Every populated slot whose def draws pool units has its loaded rounds zeroed (the slot
// word, or its def's bucket, which a controller seat does not write), unless the owner sits
// a UseGun seat (+0x168 3).
// [orig: WeaponSlots_ClearLoadedRounds @0x5423C0 - the seat test @0x5423C5, the def[56]
//  gate @0x5423E6, WeaponSlot_SetSharedLoadedRounds(bucket, 0) @0x542430 (its seat test
//  @0x540684..0x540692) or slot+16 = 0 @0x54243A]
void weapon_inventory_clear_loaded_rounds(const WeaponTable &table, WeaponInventory &inv,
                                          const WeaponAmmoSeat &seat);
// The total clips and the loadout weight with the owner's seat (the seatless forms are
// the unseated owner) [orig: WeaponSlot_GetTotalClips @0x5425F0 - the pool
//  @0x542638, the bucket @0x542651; Server_RecalculateAllPlayerScores @0x501546 /
//  @0x5015CF; Terrain_AccumulateSectorScores @0x425220].
int32_t weapon_inventory_total_clips(const WeaponTable &table, const WeaponInventory &inv,
                                     int32_t combo, const WeaponAmmoSeat &seat);
int32_t weapon_inventory_loadout_weight_fp16(const WeaponTable &table,
                                             const WeaponInventory &inv,
                                             const WeaponAmmoSeat &seat);

// The eligibility ammo score [orig: Score_CalculateKillScore @ 0x5407E0 in its
// slot-predicate role: pool for the def's ammo class + the slot's loaded rounds].
int32_t weapon_slot_ammo_score(const WeaponTable &table, const WeaponInventory &inv,
                               int32_t combo);

// The weapon a powerup row hands out, landed in the picker's slot table
// [orig: WeaponSlot_InitFromAvatarDef @ 0x542730 (slotTable, picker, row) — the
//  weapon is the row's +0x2B0 byte]. A def with a `sameas` weapon the picker
//  already holds refills that weapon's slot instead. An EMPTY target slot first
//  takes the def's loadout_subclasses sub-variants (the entries that follow it,
//  each slot initialized, its loaded rounds zeroed, its class pool set to its
//  startrounds) and then the def itself; a held one keeps its slot. Either way a
//  def with an ammo class and a clip then has its loaded rounds zeroed, its class
//  pool SET to its startrounds and one reload drawn. `combo` is the landed slot,
//  -1 when the weapon names no slot (retail returns null: the 0xFF byte, an
//  unnamed def, or category 0 rank 0 with no `sameas` slot).
struct WeaponAvatarGrant {
    int32_t combo = -1;
    // WeaponSlot_ReloadAmmo ran at least once: retail's entry stamps the
    // picker's 80-tick 3P reload window, which the caller owns
    // [orig: WeaponSlot_ReloadAmmo @ 0x54173c].
    bool reloaded = false;
};
WeaponAvatarGrant weapon_inventory_init_from_avatar_def(const WeaponTable &table,
        WeaponInventory &inv, int32_t adm_index, int32_t owner_class,
        bool allow_sniper_scope_zoom, const WeaponAmmoSeat &seat, ScopeZeroSeed zero_seed);

// The authority's pickup refusals [orig: WeaponSlot_RecalculateScore @ 0x542450;
// it returns 0 (proceed) on a peer without authority]. The slot leg: the
// weapon's slot (its `sameas` weapon's when that one resolves) is held and its
// ammo score equals the slot def's startrounds — the picker already carries
// that weapon full [@ 0x54249E..0x54253C].
bool weapon_inventory_weapon_full(const WeaponTable &table, const WeaponInventory &inv,
                                  int32_t adm_index);
// The walk leg (slot -1, the `allammo` arm): every held slot's ammo score
// equals its def's startrounds, an empty table included; a picker without an
// item def scores every slot 0 [@ 0x54253D..0x5425A4].
bool weapon_inventory_all_full(const WeaponTable &table, const WeaponInventory &inv,
                               bool picker_has_item_def);

// The reload transfer [orig: WeaponSlot_ReloadAmmo @ 0x541720, net-re §5.58]: refund
// the remaining clip into the pool, then refill to clipsize clamped by the pool.
// Returns the rounds now loaded as the seat reads them (unchanged when the def has
// no clip). The ammo reads, the spend and the bucket write take the owner's seat.
int32_t weapon_inventory_reload_slot(const WeaponTable &table, WeaponInventory &inv,
                                     int32_t combo, const WeaponAmmoSeat &seat);

// The reload transfer on an EWeap entity, as retail's accessors route an EWeap
// owner. The routed slot (the entity's own, or its carrier's when the route bit
// is set) and its def give the loaded rounds, the draw's size and the clip store;
// the entity's own slot and def take the refund, the reserve read and the spend.
// The remaining clip is refunded through the own slot's re-split (clip + reserve +
// the refund, capped at its def's startrounds, re-split into a clipsize clip and
// the rest), then clipsize * units is drawn from the own reserve word (floored at
// 0, -1 kept) and the routed clip becomes what was drawn.
// [orig: WeaponSlot_ReloadAmmo @0x541720 - Entity_GetWeaponSlots @0x541730 (the
//  route @0x54610E..0x546113), the loaded word @0x5417C9, WeaponSlot_AddAmmo
//  @0x5417E7 -> the re-split @0x5462A0..0x5462FF on the entity's own words
//  (@0x540A85..0x540AB0), the reserve read Entity_GetMountSlotReserve @0x546120
//  from @0x541805 (@0x540780..0x54078E), WeaponSlot_DecrementAmmo @0x541821 ->
//  Entity_SpendMountSlotReserve @0x5461D0, the clip store @0x541850]
void weapon_mount_slot_reload(const WeaponTableEntry &routed_def, WeaponSlotState &routed,
                              const WeaponTableEntry &own_def, WeaponSlotState &own);

// Player_SelectWeaponSlot @ 0x4DD680 — the category-level select. Stores the selected
// slot (entity+0x308) and, unless seated (`commit_equip` false: the original applies
// to EquippedSlot only when !parentEntity || parentSlot not in {2,3}), the equipped one.
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
    // The EquippedSlot's def when that slot is a borrowed gun's (a UseGun seat's, or
    // one held on foot after a refused attach, D-NET-420); null names the inventory's
    // equipped slot. Both walks read the EquippedSlot's own def [orig:
    // Player_SwitchToWeaponByHandle @0x4E0170 (the EquippedSlot load @0x4E01A5, the
    // FSM gate @0x4E01CF..0x4E01F6, its def's category compare @0x4E0271); Player_CycleWeaponSlot @0x4DFE70 (the start index
    // from EquippedSlot->Def @0x4DFE97..0x4DFEDB)].
    const WeaponTableEntry *borrowed_def = nullptr;
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
    // Category-handle walks reset before selection; cycles/direct mounts do not.
    // Also true when an admitted walk finds no slot. [orig: @0x4E0223]
    bool reset_view = false;
};

// Player_SwitchToWeaponByHandle @ 0x4E0170 — the category-key walk. handle =
// category*65 + rank (the input cases 200-210 pass (action-200)*65 @ 0x4e1144).
// Same-category presses rank-cycle; eligibility = weapon_class_slot in {1,2} OR
// ammo score nonzero, AND !(flags2 & 1).
WeaponSwitchOutcome weapon_switch_to_handle(const WeaponTable &table,
                                            WeaponInventory &inv, int32_t handle,
                                            const WeaponSwitchGates &gates);

// Whether the walk on `handle` passes its two gates ahead of its camera reset: a body in no
// Controller, Gunner or Driver seat, and an EquippedSlot whose action admits the walk (not
// FIRE; not RELOAD on the handle's own category; not RECOIL on another), or one with no def.
// [orig: Player_SwitchToWeaponByHandle @0x4E0170 - the seat gate @0x4E017D..0x4E0192, the
//  EquippedSlot and its def @0x4E01A5..0x4E01D4, the action gate @0x4E01D6..0x4E01F6, the
//  reset @0x4E01FC]
bool weapon_switch_walk_resets_view(const WeaponTable &table, const WeaponInventory &inv,
                                    int32_t handle, const WeaponSwitchGates &gates);

// Player_CycleWeaponSlot @ 0x4DFE70 — next/prev over ALL 780 combos (direction +1/-1),
// every candidate needs the ammo score (no weapon_class exemption), silent stop on
// wrap (no deny).
WeaponSwitchOutcome weapon_cycle_slot(const WeaponTable &table, WeaponInventory &inv,
                                      int32_t direction,
                                      const WeaponSwitchGates &gates);


// The HUDLS slot bar's per-category scan over the FIRST ten categories'
// 650 slots [orig: HUD_DrawWeaponSlotBar @0x599D0A..0x599D69]: categories
// 1..5 are skipped whole (left empty); in 0 and 6..9 the first def-bearing
// slot is recorded and the walk then jumps its def's loadout_subclasses
// variant slots (`i += def+0x3AC`), while EVERY def-bearing slot the walk
// visits counts — so a later weapon's own variants count too. `adm_index`
// -1 = no weapon in the category. `def_category` is the recorded def's
// category dword (def+0x00), the index the key label's binding record is
// read at (200 + it) [orig: HUD_DrawWeaponSlotBar @0x599e8c..0x599e99].
struct WeaponSlotBarCategory {
    int16_t adm_index = -1;
    int32_t count = 0;
    int32_t def_category = -1;
};
std::array<WeaponSlotBarCategory, 10> weapon_inventory_slot_bar_scan(const WeaponTable &table,
                                                                     const WeaponInventory &inv);

// The local player's inventory snapshot for the shell/tests as one value the
// embedder fills from WeaponInventory + the weapon table (the occupied combo
// slots with their weapon.def names and magazines, the equipped combo and
// name, the carry flags); its Godot record wraps it by value (ADR 0043 d10).
struct LocalInventoryView {
    struct Slot {
        int32_t combo = 0;
        std::string name;
        int32_t clip = 0;
    };
    bool valid = false;
    int32_t equipped_combo = -1;
    std::string equipped_name;
    uint32_t carry_flags = 0;
    std::vector<Slot> slots;
};

} // namespace opennova::world
