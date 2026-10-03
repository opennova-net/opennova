// The powerup family: the med and ammo packs a Powerup item picks up on
// contact, their powerup.def rows, the respawn countdown and the mission-start
// bind (docs/world/powerup-re.md).
//
// [orig: PowerUpDef_LoadFromFile @0x443350; the init walk
//  Entity_ProcessAllExplosionPhysics @0x4432A0 (misnamed) -> sub_442D00
//  @0x442D00; the resolver's Powerup branch @0x4B2FB2..0x4B2FE5 ->
//  Entity_InvokeCollisionCallback @0x442350 -> PowerupAction_Pickup @0x4428A0;
//  the countdown Entity_TickFireTimer @0x442850 -> Entity_InvokeFireCallback
//  @0x442810 -> PowerupAction_Respawn @0x442B40]
#pragma once

#include <runtime/world/entity.h>
#include <runtime/world/system.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::def {
struct DefPowerupFile;
struct DefItemsFile;
} // namespace opennova::def

namespace opennova::world {

class World;
class LocalPlayer;
struct WeaponTable;
struct WeaponAvatarGrant;

// One `action` row of a powerup.def entry: the bound handler (weapon_fsh.h's
// registry id: powerup_pickup, powerup_respawn or the placeholder) and the keys
// the two handlers read. A row the file never authored takes the default
// handler with nothing else; an authored row with no FUNCTION, `function null`
// or an unregistered name keeps the placeholder, which runs nothing.
// [orig: PowerUpDef_RegisterNewEntry @0x442C00 -- the absent-row defaults
//  @0x442C2C..0x442C91 (pickup) / @0x442C94..0x442CEC (respawn);
//  ActionDef_ParseScriptLine's FUNCTION bind @0x40296E, the placeholder
//  ActionSlot_ExecuteAction @0x4020A0 for `null` / unknown names @0x4028F7]
struct PowerupAction {
    bool authored = false;
    int8_t handler = 0; // weapon_handler::*
    std::string soundset;
    std::string soundsetend;
    std::string particle;
    std::string texttoken;
};

// One powerup.def row as the runtime reads it (the 576-byte retail row).
struct PowerupDef {
    std::string name;
    int32_t respawn_time = 0; // row+0x28, seconds; x62 ticks at pickup [orig: @0x442AD0..0x442AE1]
    int32_t max_respawns = 0; // row+0x23C; the entity seeds max-1 respawns, 0 = unlimited (-1)
    int32_t hp = 0;           // row+0x2C: -1 raises to max, >0 adds, else nothing
    int32_t mana = 0;         // row+0x30: -1 refills ammo class 1, else adds to it
    int32_t weapon = 0;       // row+0x34: 0 none, -1 `all`, else the weapon table index
    bool allammo = false;     // row+0x38
    std::array<int32_t, 128> ammo{}; // row+0x3C.. per ammo class id: -1 fills, else adds
    PowerupAction pickup;     // row+0x20
    PowerupAction respawn;    // row+0x24
};

// The mission's powerup rows [orig: the 576-byte-stride array dword_A89598,
// count dword_A89590; PowerUpDef_FindByName @0x442660].
struct PowerupTable {
    std::vector<PowerupDef> rows;
    // The file was read this mission (an empty table with `loaded` clear is the
    // retail "Unable to load powerup.def" state: every Powerup row is destroyed
    // at the bind).
    bool loaded = false;
    // Case-insensitive, first row wins; -1 when absent.
    int32_t index_of(const char *name) const;
    const PowerupDef *by_index(int32_t index) const {
        return index >= 0 && static_cast<size_t>(index) < rows.size()
                ? &rows[static_cast<size_t>(index)]
                : nullptr;
    }
};

// The row build: the parsed file plus the weapon table the `weapon` and `ammo`
// names resolve against (retail resolves both at parse time, over the loaded
// weapon.def) [orig: PowerUpDef_ParseProperty -- AvatarDef_FindByName @0x4431B7
//  / AdmDef_GetIndexFromPtr @0x443211 for `weapon`, the class lookup sub_540590
//  @0x443250 for `ammo`, an unknown class logging "ammo class error"].
PowerupTable build_powerup_table(const def::DefPowerupFile &file, const WeaponTable &weapons);

// Mission-start bind of every pool-1/pool-2 row whose ItemDef carries the
// Powerup attrib: the row's def by the item's `powerupdef` name, the respawn
// countdown armed idle and the remaining-respawn count seeded; a row whose
// name the table lacks is destroyed. [orig: Game_StartMission @0x525DE7 ->
//  Entity_ProcessAllExplosionPhysics @0x4432A0 (pools 1 and 2, the ItemDef
//  attrib&2 test @0x4432CC / @0x443318) -> sub_442D00 @0x442D00 (the
//  PowerUpDef_FindByName miss -> Entity_Destroy @0x442E26)]
void powerup_bind_entities(World &world, const def::DefItemsFile &items);

// A grant the host applies to a REMOTE player's connection pools: retail's
// authority arm of WeaponSlot_AddAmmo writes the validated entity's
// per-connection pool table, and the `allammo` refill re-seeds the
// connection's slot tables [orig: WeaponSlot_AddAmmo @0x540A20 (@0x540AC2..
//  0x540AF1); Entity_UpdateWeaponOverlayFrameState @0x4DC340 (@0x4DC348..
//  0x4DC38F)]. The local player's inventory is written in place instead
//  (retail's g_LocalAmmoPools / g_WeaponSlotArrayBase legs).
struct PowerupGrant {
    EntityHandle picker;
    bool allammo = false;
    std::vector<std::pair<int32_t, int32_t>> ammo_adds; // (ammo class id, amount)
};

// The authority's `weapon` grant, recorded for every picker the arm admitted:
// the host lands a REMOTE picker's on its connection's slot table and fans
// S2C 0x35 [u16 picker][u16 powerup] to every in-match slot but its own, where
// the picker's client mounts the weapon (the listen host's own pickup never
// mounts). The local player's grant already ran in place.
// [orig: Server_BroadcastWeaponOverlayUpdate @0x509FC0 -- the authority gate
//  @0x509FC7, WeaponSlot_InitFromAvatarDef on the validated slot's table
//  @0x509FE9, mask 0x90 @0x509FF1, the two handles @0x50A054..0x50A06C,
//  SendFiltered(0x35, reliable) @0x50A07D]
struct PowerupWeaponGrant {
    EntityHandle picker;
    EntityHandle powerup;
    uint8_t weapon = 0; // the row's +0x2B0 byte
};

// The pickup: the powerup's pickup action run for `picker`, the body whose
// movement resolve contacted it. Exposed for the tests; the tick reaches it
// through powerup_process_contacts. [orig: Entity_InvokeCollisionCallback
//  @0x442350 -> the row's +0x2B8 action -> PowerupAction_Pickup @0x4428A0]
void powerup_pickup(World &world, EntityHandle powerup, EntityHandle picker,
                    const TickContext &ctx);

// The same pickup for a picker the caller supplies: a wire-replica body's
// transient copy on a client (no registry entity; its handle left invalid so
// it is never the local player). [orig: PowerupAction_Pickup @0x4428A0, reached
// from a client's resolver for a remote body]
void powerup_pickup_by(World &world, EntityHandle powerup, Entity &picker,
                       const TickContext &ctx);

// Drains the resolver's Powerup contacts in resolver order, on every peer:
// retail runs the callback inline in whichever machine's resolver produced the
// contact (the joiner's own body included; no authority gate precedes the
// pickup) [orig: the branch @0x4B2FB8..0x4B2FE5].
void powerup_process_contacts(World &world, const TickContext &ctx);

// The local player's side of a `weapon` grant that landed in its own table:
// the 80-tick 3P reload window the refill's reload stamps on the picker, and
// the held weapon's FSM magazine when the refilled slot is the equipped one.
// [orig: WeaponSlot_ReloadAmmo @0x54173c / @0x5417A2, reached from
//  WeaponSlot_InitFromAvatarDef @0x542883 / @0x542909]
void powerup_sync_local_weapon_grant(World &world, LocalPlayer &lp, const Entity &picker,
                                     const WeaponAvatarGrant &landed);

// A client's S2C 0x35 for its own player: the weapon its copy of `row` names
// (the row's +0x2B0 byte, written by its own pickup) lands in its table and is
// mounted. Refused for a dead player, a missing row, or a weapon that names no
// slot; returns whether the weapon landed. The caller resolves the handles and
// checks the picker is its own player.
// [orig: NapiNPClientMsg_0x035 @0x4261A0 -> sub_4E03D0 @0x4E03D0 ->
//  WeaponSlot_InitFromAvatarDef @0x4E0401, Player_MountWeaponSlot @0x4E040E]
bool powerup_weapon_grant_received(World &world, LocalPlayer &lp, const Entity *row,
                                   bool local_dead);

// The respawn countdown over every bound row, authority only: a positive
// countdown steps down, zero fires the respawn action, arms -1 and spends one
// remaining respawn. [orig: Entity_TickFireTimer @0x442850 -- the
//  is_authority and +0x2C8 gates @0x442862, the decrement @0x442878, the fire
//  @0x442883..0x442895]
void powerup_tick(World &world, const TickContext &ctx);

} // namespace opennova::world
