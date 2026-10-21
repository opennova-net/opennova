// The C2S 0x2F loadout grant — the accept half of NapiNPServerMsg_HandlePlayerLoadout:
// the submission envelope, the granted-kit resolve over the host's armory table (the S2C
// 0x5A body, the authority ammo pools, the per-ammo damage classes, the rebuilt host-side
// slot rows), and the current-slot-list re-send body every abort and the deploy release
// answer with. Split out of server_message_dispatch.cpp, whose 0x2F case and
// Server_ReleasePlayerDeployment are the callers; internal to the inmatch group.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <net/npwire/ingame_decode.h> // LoadoutSubmit / WeaponLoadout

#include <runtime/inmatch/napi_np_connection.h>

#include <runtime/world/weapon_inventory.h> // WeaponAmmoSeat

namespace opennova::world {
class World;
class LocalPlayer;
struct Entity;
struct WeaponTable;
}

namespace opennova::inmatch {

// tag=0x5A WEAPON-LOADOUT-SYNC, built from the joiner's own C2S 0x2F loadout submit.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790 — parses the request, validates the
// envelope (loadout_envelope_accepted), clamps the in-range soldier type to [5,9]-else-8
// (@0x515913), stamps entity+660 playerClass (@0x515ab0), loads the entries
// with their loadout_subclasses sub-variants into the player's 780-slot weapon table (the first
// weapon on a combo keeps it), then Server_SendWeaponSlotListToPlayer @0x502550 walks that
// table ascending by weapon-slot combo (category*65 + rank, @0x5026e5..@0x5028a0), filtering
// each slot by the team/char masks (@0x502716) and emitting one 4-byte group:
// [admIdx = AdmDef_FindIndexByName @0x50273b][ammoPrimary = WeaponSlot_GetTotalClips
// @0x502794][ammoSecondary = the same count for the first different-ammoclass sub-variant in
// parent+1..parent+LSC (@0x5027c8), else 0xFF][the slot's player+89688 damage-class byte at
// the weapon's own AdmDef index (@0x50286A..0x50288A)], then skipping the weapon's LSC slots
// (@0x50284E), so a sub-variant outside them is a row of its own (D-NET-509).]
//
// With the armory table fed (world::World::weapons), the reply resolves REAL counts from the
// rebuilt table (weapon_inventory) — the golden ASH_I5A reply {2:255, 3:10, 21:10, 76:1, 77:2,
// 78:3, 83:3} reproduces from the host's own resolved weapon.def (D-NET-141). Table-less hosts
// (unit paths / no resource root) keep the
// prior request-echo: the client clamps echoed bytes on apply [orig: @0x4295d7-0x4295e9], a
// tracked divergence for that configuration only. The accepted fourth byte is stored RAW in the
// slot's player+89688 table at the weapon's round ammo index (write_ammo_damage_classes), and
// every row reads that table at its own AdmDef index (apply_granted_loadout) (D-NET-508).
struct GrantedWeaponLoadout {
	WeaponLoadout reply;
	// The serverPlayer+88664 authority pool image copied by S2C 0x0F. Each
	// accepted request writes its ammo class in wire order, so a later weapon
	// sharing that class wins exactly as retail does.
	std::array<int32_t, 128> ammo_pools{};
    // Loaded-round buckets [orig: player+89176; WeaponSlot_SetSharedLoadedRounds @0x540670].
    std::array<int32_t, 128> shared_clips{};
	// The accept's player+89688 stores, in request order: each accepted entry's
	// weapon round ammo index (adm+4; -1 unresolved) and its raw fourth byte, the
	// last store at an index winning. [orig: NapiNPServerMsg_HandlePlayerLoadout
	//  @0x515E0D..0x515E92]
	std::vector<std::pair<int16_t, uint8_t>> damage_class_writes;
	uint32_t carry_flags = 0;
	// The host-side 780-row weapon-slot table this accept rebuilds, one row per
	// populated combo (sub-variants included) with the clip the rebuild drew — the rows the C2S
	// 0x06 clip check / 0x25 relay refill and the 1 Hz kit-weight recompute
	// read. [orig: playerSlot+464 + 100*combo, slot+16 loaded rounds after
	//  WeaponSlots_RecalculateAmmoFromCapacity @0x515db5..0x515f4d]
	struct Row {
		uint16_t combo = 0;
		uint8_t adm_index = 0;
		int16_t clip = 0;
	};
	std::vector<Row> rows;
	// The entries the accept kept, in request order (the masks and the availability
	// passed): the slot's loadout buffer, which every deploy rebuilds the kit from.
	// [orig: NapiNPServerMsg_HandlePlayerLoadout - the kept entry @0x515A5A..0x515A7E,
	//  the buffer @0x515CBD..0x515D83]
	std::vector<LoadoutSubmitEntry> kept_entries;
	// The rebuilt table itself, its pools and buckets and the fill's carry bits: the
	// slot's own table, which for the authority's local player is the local one
	// (own_slot_local_player). On the heap: the 780 rows outweigh a test main's stack.
	std::shared_ptr<const world::WeaponInventory> table;
};

// The local player whose weapon table is this slot's own, or null: on the authority
// Player_InitPlayer points the local weapon table (g_WeaponSlotArrayBase) at the local
// player's slot table (Entity_GetWeaponSlotsPtr, slot+0x1D0), so every slot-table
// write for the slot holding the local player (the 0x2F accept's grant, the deploy's
// and the round init's rebuilds) writes the local table (world::LocalPlayer::inventory,
// its pools and buckets too, which the authority reads through the slot:
// Entity_GetScoreValueBySlotType's validated slot @0x5407A4..0x5407B3) (D-NET-695,
// D-NET-479). Null for every other slot, whose rows are its entity's
// (world::Entity::weapon_slots) and whose pools are NapiNPConnection::reply's.
// [orig: Player_InitPlayer @0x4E15F0 - the authority test @0x4E18A8, the call
//  @0x4E18B7, the store @0x4E18C1]
world::LocalPlayer *own_slot_local_player(const NapiNPConnection &conn, world::World &world);

// The accept's availability gate over the host's weapon table (g_ArmoryWeaponAvailability,
// 255 dwords: the session's first mission builds it from the cfg words and the mission's
// availability, apply_first_mission_settings, and the admin's WEAPON SET writes it): an entry at 0
// is refused, one at the title's armory-only value (2 in Jointops.exe, 3 in dfx2.exe)
// only while the requester's entity carries the armory-zone bit (Flags 0x400000).
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515A3F..0x515A58; dfx2.exe @0x5129d0
//  (@0x512c8a)]
struct LoadoutAvailability {
	// The restricted (index, value) entries (NapiNPServerCtx::weapon_restrictions); an
	// index absent here reads 1.
	const std::vector<std::pair<uint8_t, uint8_t>> *restrictions = nullptr;
	int32_t armory_only = 2;
	bool requester_in_armory_zone = false;
};


// The 0x2F submission envelope, checked BEFORE anything is applied (team 1..4, above 4 only
// in a team-less game type; a NONZERO class 5..9). See the definition for the witness.
bool loadout_envelope_accepted(const LoadoutSubmit &req, uint32_t game_type);

// Resolve the accepted submission into the granted kit: the S2C 0x5A reply, the authority
// ammo pool image, the per-ammo damage classes, the carry flags and the rebuilt slot rows.
// The rebuild starts from the slot's pools and buckets and reads its revive latch, team
// and the entity's seat (world::entity_ammo_seat); the fills' sniper lock reads the
// fill's zoom value (World::rules.sniper_zoom_fill). A null/empty `table` is the
// resource-less request-echo fallback. `local_table` is the slot's table when it is the
// local one (own_slot_local_player), whose pools and buckets the rebuild starts from.
GrantedWeaponLoadout grant_weapon_loadout(const LoadoutSubmit &req,
										  uint16_t class_allow_mask,
										  const world::WeaponTable *table,
										  const NapiNPConnection &conn,
										  const world::WeaponAmmoSeat &seat,
										  bool allow_sniper_scope_zoom,
										  const LoadoutAvailability *availability = nullptr,
										  const world::WeaponInventory *local_table = nullptr);


// The accept's damage-class stores onto the slot's table (NapiNPConnection::
// ammo_damage_classes), the accept's alone: the deploy's rebuild writes none.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515E8B..0x515E92; the table's one other
//  writer is the out-of-session armory apply, WeaponLoadout_ApplyFromBuffer @0x565CD0
//  (the store @0x56618C)]
void write_ammo_damage_classes(NapiNPConnection &conn, const GrantedWeaponLoadout &grant);

// The rebuilt kit onto the slot: the retained 0x5A body the re-sends answer with (each
// row's fourth byte the slot's damage-class byte at the row's AdmDef index), the
// authority ammo pools and loaded-round buckets, and (with a weapon table) the
// player entity's own slot rows (world::Entity::weapon_slots) — every granted combo
// with its drawn clip replaces the previous rows. A table-less host keeps its rows.
// For the slot holding the local player (`local` non-null) the rebuilt table, its
// pools, buckets and the fill's carry bits are the local table's instead
// (own_slot_local_player); the selection words (EquippedSlot, the pending and the
// selected slot) are the entity's and stay.
// [orig: WeaponSlotPool_ResetAllEntries + WeaponSlotTable_LoadAllFromDefs +
//  WeaponSlots_RecalculateAmmoFromCapacity, NapiNPServerMsg_HandlePlayerLoadout
//  @0x515DB5..0x515F4D and PlayerSlot_InitWeaponsFromLoadout @0x515571..0x51577C]
void apply_granted_loadout(NapiNPConnection &conn, world::Entity &player,
                           const GrantedWeaponLoadout &grant, bool rebuild_rows,
                           world::LocalPlayer *local = nullptr);

// The deploy's kit rebuild: the slot's weapon table, ammo pools and clips rebuilt
// from its loadout buffer (the last accepted 0x2F's entries and counts under the
// stamped class), so a redeployed player starts every life with the kit it was
// granted, full again; the entity's carry bits are the fill's. Nothing else changes:
// no class stamp, no damage-class table, no cooldown. A slot that never submitted
// rebuilds an empty table; a host with no weapon table keeps its state. (D-NET-378)
// [orig: PlayerSlot_InitWeaponsFromLoadout @0x515550, called from
//  Server_DeployPlayer @0x5178E1 on a deploy that is not a revive]
void PlayerSlot_InitWeaponsFromLoadout(NapiNPConnection &conn, world::World &world,
                                       world::Entity &player);

// The round init's rebuild of a kept slot's table from its loadout buffer, the
// buffer the map change keeps: the kit and its sub-variants, the pools seeded from
// the defs under the slot's class and one clip drawn (the counts wait for the deploy).
// A host with no weapon table keeps its state.
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0, @0x516BF3..0x516C20]
void PlayerSlot_RebuildKitForRound(NapiNPConnection &conn, world::World &world,
                                   world::Entity &player);

// The slot's class (+0x15EDC, NapiNPConnection::slot_class), which every weapon-slot
// list send stores as the slot's PlayerType and writes as the list's header byte.
// [orig: Server_SendWeaponSlotListToPlayer @0x502550 - the PlayerType from +0x15EDC
//  @0x5025A8..0x5025DB, the header byte @0x5025E0..0x5025E6]
uint8_t current_player_class(const NapiNPConnection &conn);

// Every weapon-slot list send's own legs: the slot's PlayerType set to its class; for
// the authority's own slot (its local byte +5: the listen host's own connection) its
// loadout buffer copied into the spawn buffer (g_SpawnLoadoutBuffer: the local player's
// LocalPlayerLoadout::spawn_kit) and its class stored into the profile's class for the
// slot team's side (g_CharClassTeam1 for team 1 or 3, else g_CharClassTeam2: the own
// connection's char_vars.char_class[0 / 1], which Player_InitPlayer re-stamps the own
// body's class from at the next mission start); and, after the rows, for the slot whose
// entity is the local player, the local select of the slot the body's equipped weapon
// names (g_CurrentWeaponSlot = its category * 65 + rank, the current one when the index
// names no weapon) and the mount of the EquippedSlot.
// [orig: Server_SendWeaponSlotListToPlayer @0x502550 - @0x5025A8..0x5025DB, the local
//  test `cmp byte [ebp+5], 0` @0x5025EC, the buffer copy @0x502617..0x502627, the class
//  stores @0x502629..0x50264B; the local entity test @0x5028B8..0x5028CB, the slot
//  @0x5028CD..0x5028F5, Player_SelectWeaponSlot @0x5028FC, Player_MountWeaponSlot
//  @0x50290E]
void weapon_slot_list_own_legs(NapiNPConnection &conn, world::World *world);

// The round init's seed of the authority's own slot (+5): the spawn buffer
// (g_SpawnLoadoutBuffer, LocalPlayerLoadout's resident kit) copied into the slot's
// loadout buffer ahead of the slot table's rebuild. Nothing without a local player.
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 - `cmp byte [ebp+5], 0`
//  @0x516BC4 (or the bot byte +0x178E3 @0x516BCA, never set in JO), the copy
//  @0x516BD3..0x516BE3]
void PlayerSlot_SeedOwnLoadoutBuffer(NapiNPConnection &conn, world::World &world);

// The tag=0x5A body for every re-send that grants NOTHING new: the retained granted body
// when one exists, else the empty-table shape headed by the player's live class.
std::vector<uint8_t> build_current_loadout_reply(const std::vector<uint8_t> &retained,
                                                 uint8_t player_class);

// Server_SendWeaponSlotListToPlayer for a re-send that grants nothing new: the list's own
// legs (weapon_slot_list_own_legs), then the slot class's current-list body
// (build_current_loadout_reply over the retained body).
std::vector<uint8_t> current_weapon_slot_list(NapiNPConnection &conn, world::World *world);

} // namespace opennova::inmatch
