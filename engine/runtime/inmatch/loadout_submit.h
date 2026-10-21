// The joiner loadout-submission seam (ADR 0031 PR E, re-opening S7b's "the
// joiner 0x2F pushes" leg): the Game_StartMission profile-page copy into the
// resident kit buffer, the side-change re-copy rule, and the C2S 0x2F
// submission content builder. The embedding shell keeps the role gates
// (listen-host/joiner), the re-entry latch, and the pump wiring
// (set_loadout_kit); everything here reads engine state only — the world's
// weapon catalog, the playersav profile record, and the resident
// LocalPlayerLoadout buffer.
#pragma once

#include <runtime/inmatch/joiner_connection.h>
#include <net/npwire/ingame_decode.h>

#include <formats/mission/bms.h>
#include <formats/playersav/weapon_sav.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <cstdint>

namespace opennova::inmatch {

// The side selector: the host's S2C 0x04 tail byte is retail's g_LocalTeam
// (teams 1/3 read the BLUE block, 2/4 the RED one [orig: @0x525788]). Before
// that latch lands (assigned_team == 0) fall back to the local entity's own
// team when L already exists.
uint8_t loadout_side_team(const world::World &world, uint8_t assigned_team);

// The Game_StartMission copy: in a live session the assigned side's profile
// page becomes the resident kit buffer [orig: @0x525813
// Buffer_CopyUntilDoubleNull(restrictionData, page, 0x800)], which is BOTH
// what the local slot pool is built from [orig: Player_InitPlayer @0x4e15f0
// -> AvatarDef_BuildDisplayList(.., restrictionData)] and what the C2S 0x2F
// serializes [orig: NetPacket_SendLoadoutSubmit @0x42cdc0]. Retail keeps one
// buffer; keeping two is how the local view and the wire drift apart.
// The caller gates on a live session exactly as retail is (`is_in_session`
// covers a LISTEN HOST as well as a joiner), so single player and the editor
// keep the mission's .bms kit. Returns false (and copies nothing) while the
// side latch has not landed — an UNLATCHED team must not commit a page (the
// selector is the S2C 0x04 tail byte [orig: g_LocalTeam @0x425499];
// anything-not-1-or-3 selects the RED block @0x525798, so seeding at team 0
// would commit the wrong side's page and then have to flip it).
bool seed_session_kit_from_profile(world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		world::LocalPlayerLoadout &loadout, int &seeded_side);

// The side-change re-copy rule: whenever the SIDE the selector names stops
// matching the side the resident buffer was copied from, re-copy. Keyed on
// the side rather than the raw team so a 1<->3 (or 2<->4) reassignment
// inside one side does not needlessly rebuild — those read the same block
// @0x525798 — and so an in-session armory ACCEPT, which changes the buffer
// but never the side, is not undone. [orig: the S2C 0x50 leg re-copies the
// new side's page @0x431a9a; the 0x04 case retail never has, because the
// latch precedes Game_StartMission's copy.] Returns true when it re-seeded
// (the caller re-runs its loadout rebuild + push).
bool reseed_session_kit_on_side_change(world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		world::LocalPlayerLoadout &loadout, int &seeded_side);

// The joiner's C2S 0x2F submission content — the wire seam D-NET-168
// tracked. Builds the full LoadoutKit: the ONE class integer that drives
// both the wire class and the page pick (retail reads it straight out of the
// assigned side's profile block [orig: Game_StartMission @0x5257a0], never
// from the sim-side class pick), the resident-buffer rows, and both
// side blocks for the S2C 0x50 resubmission path. `equipped_combo` is the
// live equipped slot for the pair's SECOND submit (-1 = none)
// [orig: Game_StartMission @0x525c2e passes g_CurrentWeaponSlot].
// The S2C 0x5A authoritative grant -> spawn-kit rows: names resolve through
// the weapon table (retail drops failed AdmDef lookups); the wire bytes are
// SIGNED clip counts — 0xFF is the authored/default sentinel, not 255 clips
// [orig: the grant apply @ 0x4295c4..0x429613].
void kit_from_authoritative_grant(const world::WeaponTable &weapons,
		const WeaponLoadout &grant,
		std::vector<world::WeaponKitEntry> &r_kit);

// A client's S2C 0x5A apply rebuilds the process's loadout name list (World::
// loadout_weapon_names) from the list's rows: emptied, then each row whose weapon the table
// has, its name and its three values, with no authority test ahead of it, so a listen host's
// own client and a joiner's rebuild it alike and every body the process spawns after it takes
// the first granted weapon (D-NET-143). Nothing for a world bound to no list.
// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - the memset @0x42920C, the rows
//  @0x429222..0x4293B9 (AdmDef_GetEntryByIndex @0x42923D, its null skip @0x429249, the name
//  @0x4292F0, the three "%d" values @0x42932B..0x4293A8); PlayerClass_InitEntity
//  @0x4B1100..0x4B1116]
void apply_weapon_loadout_name_list(world::World &world, const WeaponLoadout &list);

// NetPacket_SendLoadoutSubmit does not blindly serialize the requested combo.
// Once the local slot table exists it keeps a side-legal slot, otherwise walks
// that combo's 65-rank category for the first populated side-legal slot. A
// missing table, an out-of-range combo, or no legal replacement preserves the
// raw argument [orig: @0x42ce2d..0x42ce8b].
int32_t resolve_loadout_submit_combo(const world::WeaponTable &weapons,
		const world::WeaponInventory *inventory, uint8_t team,
		int32_t requested_combo);

// The armory ACCEPT in a session: WeaponLoadout_ApplyFromBuffer's session leg, the one
// apply a joiner's client and a listen host's own client run alike. The selection becomes
// the process's loadout name list on both legs (World::loadout_weapon_names, whose first name
// every body the process spawns takes; the resident kit, spawn_kit, waits for the host's
// list); off the authority the slot table empties; and the 0x2F
// the apply sends is returned for the caller's queue: the local body's team byte, the
// armory's class (g_ArmorySelectedClass, the class picked on the armory screen), the
// selection through the sender's name gate and g_CurrentWeaponSlot resolved against the
// table as the sender resolves it (after the reset, which leaves retail's slot word where
// ours' clears equipped_combo, so the word is taken first). DFX2's sender fixes the class.
// Nothing else applies locally: the host's 0x5A grant rebuilds the slots, and a listen
// host's own slot accepts the 0x2F into the local table (D-NET-695, D-NET-NEW-L2-17).
// [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 - the class's buffer copied into
//  g_LoadoutWeaponNameList @0x565D27..0x565D4B, `cmp is_in_session` @0x565D3A (its jz
//  @0x565D4D), `cmp is_authority` @0x565D4F (its jnz @0x565D56),
//  WeaponSlotPool_ResetAllEntries @0x565D58..0x565D66, g_ArmorySelectedClass
//  @0x565D6B / @0x565D86, g_CurrentWeaponSlot @0x565D73, the team
//  `movsx ecx, byte [eax+162h]` @0x565D8C, NetPacket_SendLoadoutSubmit @0x565D94, the jmp to
//  the tail @0x565D9C (g_WeaponScreenOpen = 0 @0x56629F, Game_CloseInGameScreens @0x5662A9:
//  the armory screen closes with no select or mount);
//  NetPacket_SendLoadoutSubmit @0x42CDC0 - the slot resolve @0x42CE2D..0x42CE8B, the name
//  gate @0x42CF0B; dfx2.exe NetPacket_SendLoadoutSubmit @0x42EB40 (@0x42EC15..0x42EC3E)]
LoadoutSubmit armory_accept_in_session(world::World &world, world::LocalPlayer &local,
		const std::vector<world::WeaponKitEntry> &kit, uint8_t armory_class, bool authority,
		bms::MissionType mission_type);

void build_joiner_loadout_kit(const world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		const world::LocalPlayerLoadout &loadout, int equipped_combo,
		const world::WeaponInventory *inventory,
		JoinerConnection::LoadoutKit &out);

} // namespace opennova::inmatch
