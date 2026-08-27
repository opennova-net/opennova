// The joiner loadout-submission seam (ADR 0031 PR E, re-opening S7b's "the
// joiner 0x2F pushes" leg): the Game_StartMission profile-page copy into the
// resident kit buffer, the side-change re-copy rule, and the C2S 0x2F
// submission content builder. The embedding shell keeps the role gates
// (listen-host/joiner), the re-entry latch, and the pump wiring
// (set_loadout_kit); everything here reads engine state only — the world's
// weapon catalog, the playersav profile record, and the resident
// LocalPlayerLoadout buffer.
#pragma once

#include <net/npruntime/joiner_connection.h>
#include <net/npwire/ingame_decode.h>

#include <formats/playersav/weapon_sav.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <cstdint>

namespace opennova::np {

// The side selector: the host's S2C 0x04 tail byte is retail's byte_A85B48
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
// selector is the S2C 0x04 tail byte [orig: byte_A85B48 @0x425499];
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
// both the wire class and the page pick, the resident-buffer rows, and both
// side blocks for the S2C 0x50 resubmission path. `equipped_combo` is the
// live equipped slot for the pair's SECOND submit (-1 = none)
// [orig: Game_StartMission @0x525c2e passes g_currentWeaponSlot].
// The S2C 0x5A authoritative grant -> spawn-kit rows: names resolve through
// the weapon table (retail drops failed AdmDef lookups); the wire bytes are
// SIGNED clip counts — 0xFF is the authored/default sentinel, not 255 clips
// [orig: the grant apply @ 0x4295c4..0x429613].
void kit_from_authoritative_grant(const world::WeaponTable &weapons,
		const WeaponLoadout &grant,
		std::vector<world::WeaponKitEntry> &r_kit);

// NetPacket_SendLoadoutSubmit does not blindly serialize the requested combo.
// Once the local slot table exists it keeps a side-legal slot, otherwise walks
// that combo's 65-rank category for the first populated side-legal slot. A
// missing table, an out-of-range combo, or no legal replacement preserves the
// raw argument [orig: @0x42ce2d..0x42ce8b].
int32_t resolve_loadout_submit_combo(const world::WeaponTable &weapons,
		const world::WeaponInventory *inventory, uint8_t team,
		int32_t requested_combo);

void build_joiner_loadout_kit(const world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		const world::LocalPlayerLoadout &loadout, int equipped_combo,
		const world::WeaponInventory *inventory,
		JoinerConnection::LoadoutKit &out);

} // namespace opennova::np
