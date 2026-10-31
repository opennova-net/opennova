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
    // The spawn buffer (SpawnLoadoutBuffer, weapon_inventory.h): the process's, which the role
    // binds (inmatch::ClientProcessState::spawn_loadout_buffer), else this loadout's own (a
    // harness's). ONE buffer serves the local slot pool, the respawn refill, the own slot's
    // seed and the 0x2F serialization alike; it is empty until a writer fills it.
    // [orig: g_SpawnLoadoutBuffer @0x24D4E00]
    SpawnLoadoutBuffer *process_buffer = nullptr;
    SpawnLoadoutBuffer own_buffer;
    std::vector<WeaponKitEntry> &spawn_kit() {
        return (process_buffer != nullptr ? *process_buffer : own_buffer).entries;
    }
    const std::vector<WeaponKitEntry> &spawn_kit() const {
        return (process_buffer != nullptr ? *process_buffer : own_buffer).entries;
    }
    // This load's mission kit chunk wrote the buffer. Retail's single player fills the buffer
    // ahead of the mission load, whose chunk then overwrites it; ours' kernel promotes the
    // chunk first, so the profile-page fill that follows stands back for it.
    // [orig: Game_ApplySessionSettingsToGlobals @0x5519A2..0x5519E4 from SinglePlayer_StartMission
    //  @0x561C28, ahead of Mission_LoadBMSFile @0x40F4E0 (@0x40F961)]
    bool mission_kit_promoted = false;
    // A joiner's shell applies the profile class before L has spawned; the
    // latch survives until the spawn block stamps the entity. 8 (rifleman) is
    // retail's out-of-range clamp default [orig: Server_PlayerAdd class clamp
    // @ 0x51d102].
    int32_t pending_player_class = -1;
    // The kit's weight (16.16 u) as the S2C 0x5A apply sums it into the local
    // entity's +0x37C — the run-promotion band input. Recomputed by every
    // accept/rebuild here (the same formula over the resulting inventory) and
    // mirrored into InfantryState::loadout_weight_fp16 each tick.
    // [orig: Terrain_AccumulateSectorScores @0x425220 (misnamed)]
    int32_t weight_fp16 = 0;
};

// The mission kit chunk's write into the spawn buffer (mission_kit_promoted marks it).
// filter_by_availability runs the SP .bms promote leg — availability-filter with the
// {WPN_KNIFE,-1,-1,-1} fallback [orig: Mission_LoadBMSFile @ 0x40f7ae..0x40f95c, the write
// @0x40F961].
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

// The spawn buffer as it stands: an empty buffer reads empty (no fallback: the static page
// is one writer's arm, the single player's fill).
std::vector<WeaponKitEntry> local_loadout_kit(const World &world,
                                              const LocalPlayerLoadout &loadout);

// The local player's class from the shell's class pick (5..9, the profile's
// clamp [orig: @0x5516d0..0x5516ec]): stamped on L, or latched in
// pending_player_class for a joiner whose L has not spawned (the joiner spawn
// block stamps it). False, changing nothing, outside 5..9.
bool local_loadout_set_player_class(World &world, LocalPlayerLoadout &loadout,
                                    int player_class);

// The Player_InitPlayer weapon leg [orig: @ 0x4e15f0:
// AvatarDef_BuildDisplayList (restrictionData) -> WeaponSlotPool_ResetAllEntries
// -> WeaponSlotTable_LoadAllFromDefs -> WeaponSlots_SeedAmmoPoolsFromDefs ->
// WeaponSlots_RecalculateAmmoFromCapacity -> Player_SelectWeaponSlot(195) ->
// Player_SwitchToWeaponByHandle(195)]. Entity-optional: a joiner rebuilds its
// inventory before L spawns; entity stamps re-run at the joiner spawn block.
// Fill warnings ride the engine log sink [orig: ErrorLog_WriteTimestamped].
// `select_spawn_default`: the whole of Player_InitPlayer's leg, its clears of the view and
// weapon words, the select of 195 and the walk's camera reset; false, the fill alone (a
// loadout apply's rebuild).
void local_loadout_rebuild(World &world, LocalPlayerLoadout &loadout,
                           LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                           bool &inventory_valid, bool select_spawn_default);

// Player_InitPlayer's leg (local_loadout_rebuild with the spawn select) re-run on a body that
// can be in play: a remote joiner's self S2C 0x50 runs Player_InitPlayer(1), and its walk mounts
// past the camera reset. A body with an AI slot (+0x68) whose hidden and dead bits are clear
// rank-cycles the handle's category from the EquippedSlot's rank (from rank 0 when the
// EquippedSlot holds another category: the exact slot first) and mounts the first eligible slot,
// storing the current slot word, as a weapon key's walk does; with none eligible it plays the
// deny. A hidden or dead body, or one without an AI slot, mounts nothing. (The mission start's
// re-runs take local_loadout_rebuild: their body is born hidden at the walk.)
// [orig: NapiNPClientMsg_TeamAssign @0x431910 - Player_InitPlayer(1) @0x431B14;
//  Player_SwitchToWeaponByHandle @0x4E0170 - the +0x68 test @0x4E023A, the Flags test
//  @0x4E0265 / @0x4E0269, the category compare @0x4E026F, the exact slot @0x4E0278..0x4E02C3,
//  the rank cycle @0x4E02C9..0x4E0342, the deny's tail jump @0x4E0354, the word @0x4E035C, the
//  mount @0x4E0362]
void local_loadout_rebuild_on_team_assign(World &world, LocalPlayerLoadout &loadout,
                                          LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                                          bool &inventory_valid);

// Player_InitPlayer's leg (local_loadout_rebuild with the spawn select) over a spawn kit
// that reached the sim after the mission start's environment snap and script init (the
// profile page the shell hands the sim past its load; retail copies it ahead of
// Player_InitPlayer). Both run after Player_InitPlayer in retail, so the fov target they
// left stands over the walk's camera reset.
// [orig: Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC ahead of
//  Environment_SnapStateToTargets @0x525CAE and WacScript_InitAndLoad @0x525CB3;
//  Environment_SnapStateToTargets @0x57D1E0 - g_CameraFovTargetQ16 @0x57D2BB]
void local_loadout_rebuild_after_start(World &world, LocalPlayerLoadout &loadout,
                                       LocalPlayerWeapon &weapon, WeaponInventory &inventory,
                                       bool &inventory_valid);

// The weapon whose combo a loadout apply writes into the current slot word ahead of its
// select. A protocol's S2C 0x5A apply picks one (inmatch::grant_apply_select); the armory
// ACCEPT's is always the held weapon.
enum class LoadoutApplySelect : uint8_t {
    // The body's held weapon (its AdmDef index); a body holding none, or no body, leaves the
    // word. [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - AdmDef_GetEntryByIndex
    //  (+0x2B0) @0x4296B4..0x4296BC; WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566250..0x566276)]
    HeldWeapon,
    // The kit's last row whose def is a primary (weapon_class 1), else AdmDef 0, the "null"
    // def; a def the lookup misses leaves the word.
    // [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x42AF10 (dfx2.exe) - the index's 0
    //  @0x42AF40, `cmp dword ptr [edi+3A4h], 1` @0x42B0AB and the row's index stored @0x42B0BD
    //  in the name-list walk, AdmDef_GetEntryByIndex of it @0x42B4F3, the store @0x42B50B]
    LastPrimaryRow,
};

// The armory ACCEPT apply [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0
// offline leg: parse the tuples, expand sub-weapons, reset + refill the slot
// table, apply the REQUESTED ammo over the default seed, re-select the
// equipped slot]. validate_banned runs the per-entry availability validation
// [orig: the server 0x2F gate @ 0x515a3f — 0 drops the entry]. `select` names the
// weapon the closing select's slot word comes from. The embedder routes its wire
// submissions after a true return.
bool local_loadout_apply_accept(World &world, LocalPlayerLoadout &loadout,
                                LocalPlayerWeapon &weapon,
                                WeaponInventory &inventory,
                                bool &inventory_valid,
                                const std::vector<WeaponKitEntry> &kit,
                                int32_t player_class, bool validate_banned,
                                LoadoutApplySelect select);

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
// restriction until an S2C 0x76 narrows it. [orig: g_HostClassAllowMask
// default 0x3FF]
inline constexpr uint32_t kClassAllowMaskAll = 0x3FF;
// The five-class FILTER mask union (classes 5..9 mapped to bits 1<<0..1<<4) —
// "show every class's weapons" for the charfilter tests.
inline constexpr int32_t kClassFilterMaskAll = 0x1F;

// The class the armory screen opens on: the player's current class when the
// host's S2C 0x76 allow mask permits it, else the next allowed class scanning
// up through 9, else gunner (7). The resolver indexes the class-id bits (5..9)
// of the complete wire u16 directly. [orig: Armory_ResolveSelectedClass
// @0x5642f0 against g_HostClassAllowMask]
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
// order [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430]: a row is included only
// when an `end` closed it (DefWeaponDef::unclosed is 0) AND loadout_selectable
// != 0 AND (charfilter & class_mask) AND (teamfilter & team_mask). The caller
// prepends its own "NONE" row.
void weapon_slot_indices(const opennova::def::DefWeaponDef *rows, size_t count, int slot,
                         int32_t class_mask, int32_t team_mask,
                         std::vector<int32_t> &out);

// The ammo combo's default-select row [orig: the
// `saved == i || (saved == -1 && i == maxclips)` select in
// PlayerInfo_PopulateAmmoComboBoxes @0x55def0 and the armory fill @0x565cd0]: a saved
// count picks its own row (clamped into 1..maxclips), the -1/absent sentinel
// picks the full maxclips row. Returns the selected CLIP COUNT (1..maxclips;
// 0 on a degenerate maxclips).
int player_info_default_clip_row(int saved, int maxclips);

// The GRENADE_AMMO* combos' default-select row. Their rows run 0..maxclips
// INCLUDING the zero row, so the row IS the clip count: the -1/absent
// sentinel picks the full maxclips row, a saved count its own row (a saved 0
// stays 0) [orig: `saved == i || (saved == -1 && i == maxclips)` — the >= 3
// leg of PlayerInfo_PopulateAmmoComboBoxes @0x55def0 (@0x55e412) and the WEAPON
// screen's category-3 fill @0x5647a4..0x5648a6 (@0x564880)]. The clamp of an
// out-of-domain saved count into 0..maxclips is ours: retail selects no row
// for it.
int player_info_default_grenade_row(int saved, int maxclips);

} // namespace opennova::world
