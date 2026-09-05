// The LOCAL PLAYER loadout cluster (S7b, ADR 0028): the armory availability
// table, the resident spawn kit (retail's restrictionData), the pre-spawn
// class latch, and the witnessed orchestration over them — the mission-rules
// promotion with its SP-vs-net gate, the armory ACCEPT apply, and the
// Player_InitPlayer spawn rebuild. Moved verbatim from the shell binding
// (net-re §5.57/§5.63); the embedder feeds plain kit tuples and routes the
// joiner wire submissions — the rules never touch the wire.
// [orig: the SP chunk promotion + gate Mission_LoadBMSFile @ 0x40F4E0; the
//  ACCEPT apply WeaponLoadout_ApplyFromBuffer @ 0x565cd0; the spawn leg
//  Player_InitPlayer @ 0x4e15f0]
#pragma once

#include <formats/def/def.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_inventory.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::world {

class World;

// The local player's whole loadout state — retail's restrictionData buffer
// plus the availability table and the pre-spawn class latch, one aggregate
// the embedder holds beside the world (the LocalPlayerWeapon pattern).
struct LocalPlayerLoadout {
    // [orig: the all-1 default @ 0x551c86]
    WeaponAvailability availability{};
    // The resident kit buffer [orig: restrictionData @ 0x24D4E00 — ONE buffer
    // serves the local slot pool, the respawn refill, and the joiner's 0x2F
    // serialization alike].
    std::vector<WeaponKitEntry> spawn_kit;
    bool spawn_kit_set = false;
    // A joiner's shell applies the profile class before L has spawned; the
    // latch survives until the spawn block stamps the entity. 8 (rifleman) is
    // retail's out-of-range clamp default [orig: Server_PlayerAdd class clamp
    // @ 0x51d102].
    int32_t pending_player_class = -1;

    void reset() {
        availability.reset();
        spawn_kit.clear();
        spawn_kit_set = false;
        // The class latch deliberately survives kit resets — the shell
        // re-applies the kit after each load, the class rides the profile.
    }
};

// Install the spawn kit [orig: the S2C 0x5A apply / SP shared restrictionData
// write @ 0x4293e4]. filter_by_availability runs the SP .bms promote leg —
// availability-filter with the {WPN_KNIFE,-1,-1,-1} fallback [orig:
// Mission_LoadBMSFile @ 0x40f7ae..0x40f95c].
void local_loadout_set_spawn_kit(World &world, LocalPlayerLoadout &loadout,
                                 std::vector<WeaponKitEntry> kit,
                                 bool filter_by_availability);

// Reset + apply availability (name, value) pairs onto the armory table
// [orig: the all-1 default @ 0x551c86; the availability filter source].
void local_loadout_apply_availability_pairs(
        World &world, LocalPlayerLoadout &loadout,
        const std::vector<std::pair<std::string, int32_t>> &pairs);

// The mission loadout/availability promotion WITH the witnessed SP-vs-net
// gate: in a live session the original never READS either chunk —
// Mission_LoadBMSFile tests the session flag and fseeks past the loadout
// chunk and then past the availability chunk, so no map kit and no map
// availability table are ever promoted; is_in_session is true for a LISTEN
// HOST as well as for a joiner (our world.rules.mp_session is exactly that pair).
// The MP kit instead comes from the player profile's per-class page (the
// embedder's Game_StartMission copy). Returns true when a kit was promoted —
// the caller re-runs its spawn rebuild + view-effects reset.
// [orig: Mission_LoadBMSFile @ 0x40F4E0 — gate @ 0x40f694/@ 0x40f6a1,
//  loadout-chunk skip @ 0x40f6b2, availability-chunk skip @ 0x40f6e1; the
//  availability filter @ 0x40f834, the knife fallback @ 0x40f899 and the
//  restrictionData write @ 0x40f961 all live on the non-session branch]
bool local_loadout_promote_mission_rules(
        World &world, LocalPlayerLoadout &loadout,
        const std::vector<std::pair<std::string, int32_t>> &availability_rows,
        std::vector<WeaponKitEntry> kit_rows);

// Stamp the local entity's per-ammo damage-class bytes from the resident kit
// (the default kit when none is set) [orig: the flags byte of each kit tuple;
// net-re §5.63].
void local_loadout_sync_damage_classes(World &world,
                                       const LocalPlayerLoadout &loadout);

// The Player_InitPlayer weapon leg [orig: @ 0x4e15f0:
// AvatarDef_BuildDisplayList (restrictionData) -> WeaponSlotPool_ResetAllEntries
// -> WeaponSlotTable_LoadAllFromDefs -> WeaponSlots_SeedAmmoPoolsFromDefs ->
// WeaponSlots_RecalculateAmmoFromCapacity -> Player_SelectWeaponSlot(195) ->
// Player_SwitchToWeaponByHandle(195)]. Entity-optional: a joiner rebuilds its
// inventory before L spawns; entity stamps re-run at the joiner spawn block.
// Fill warnings ride the engine log sink [orig: ErrorLog_WriteTimestamped].
void local_loadout_rebuild(World &world, LocalPlayerLoadout &loadout,
                           LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                           bool &inventory_valid, bool select_spawn_default);

// The armory ACCEPT apply [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0
// offline leg: parse the tuples, expand sub-weapons, reset + refill the slot
// table, apply the REQUESTED ammo over the default seed, re-select the
// equipped slot]. validate_banned runs the per-entry availability validation
// [orig: the server 0x2F gate @ 0x515a3f — 0 drops the entry]. The embedder
// routes its wire submissions after a true return.
bool local_loadout_apply_accept(World &world, LocalPlayerLoadout &loadout,
                                LocalPlayerWeapon &weapon,
                                WeaponInventory &inventory,
                                bool &inventory_valid,
                                const std::vector<WeaponKitEntry> &kit,
                                int32_t player_class, bool validate_banned);

// The armory PLAYER_CLASS spin catalog: the five soldier classes in the
// authored spin order, each pairing its wire class value with the "Menu"
// section GameText label key. [orig: UI_InitWeaponClassSelection @0x567250 —
// fills the spin with CHARCLASS_MEDIC..CHARCLASS_ENGINEER, values 5..9]
struct ArmoryClassCatalogEntry {
    int32_t class_value;
    const char *text_key;
};
inline constexpr int kArmoryClassCount = 5;
inline constexpr ArmoryClassCatalogEntry kArmoryClassCatalog[kArmoryClassCount] = {
    {5, "CHARCLASS_MEDIC"},
    {6, "CHARCLASS_SNIPER"},
    {7, "CHARCLASS_GUNNER"},
    {8, "CHARCLASS_RIFLEMAN"},
    {9, "CHARCLASS_ENGINEER"},
};

// The host class-allow mask default — every wire class bit 0..9 set, i.e. no
// restriction until an S2C 0x76 narrows it. [orig: g_hostClassAllowMask
// default 0x3FF]
inline constexpr uint32_t kClassAllowMaskAll = 0x3FF;
// The five-class FILTER mask union (classes 5..9 mapped to bits 1<<0..1<<4) —
// "show every class's weapons" for the charfilter tests.
inline constexpr int32_t kClassFilterMaskAll = 0x1F;

// The class the armory screen opens on: the player's current class when the
// host's S2C 0x76 allow mask permits it, else the next allowed class scanning
// up through 9, else gunner (7). The resolver indexes the class-id bits (5..9)
// of the complete wire u16 directly. [orig: Armory_ResolveSelectedClass
// @0x5642f0 against g_hostClassAllowMask]
int armory_resolve_selected_class(int player_class, uint32_t class_allow_mask);

// The armory class filter bit: 1 << (class - 5) for the five soldier classes;
// any other class (an unclassed SP spawn) filters NOTHING — the witnessed
// default is ALL weapons. [orig: @0x5642f0 switch default mask = -1]
int32_t armory_class_filter_mask(int selected_class);

// The PLAYER_INFO screen's team/class masks [orig:
// PlayerInfo_SetTeamAndClassMask @0x55de60]. Team: 2 - (team != 0) — team 0
// reads mask 2, every other team mask 1. Class: PLAYERCLASS carries values
// 5..9 (Medic..Engineer) and masks to the matching power-of-two bit; anything
// else masks NOTHING on this screen (unlike the armory's all-weapons default).
int32_t player_info_team_mask(int team);
int32_t player_info_class_mask(int playerclass_value);

// The weapons that belong in `slot` for the class + team masks, in table
// order [orig: populate_weapon_slot_lists @0x560430]: a row is included only
// when loadout_selectable != 0 AND (charfilter & class_mask) AND
// (teamfilter & team_mask). The caller prepends its own "NONE" row.
void weapon_slot_indices(const opennova::def::DefWeaponDef *rows, size_t count, int slot,
                         int32_t class_mask, int32_t team_mask,
                         std::vector<int32_t> &out);

// The ammo combo's default-select row [orig: the
// `saved == i || (saved == -1 && i == maxclips)` select in
// populate_ammo_combo_boxes @0x55def0 and the armory fill @0x565cd0]: a saved
// count picks its own row (clamped into 1..maxclips), the -1/absent sentinel
// picks the full maxclips row. Returns the selected CLIP COUNT (1..maxclips;
// 0 on a degenerate maxclips).
int player_info_default_clip_row(int saved, int maxclips);

} // namespace opennova::world
