// The joiner loadout-submission seam (ADR 0031 PR E, re-opening S7b's "the
// joiner 0x2F pushes" leg): the Game_StartMission profile-page copy into the
// resident kit buffer, the side-change re-copy rule, and the C2S 0x2F
// submission content builder. The embedding shell keeps the role it reads
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

// The class a protocol that fixes the loadout class (ProtocolInMatch::c2s_2f.loadout_class_fixed)
// gives a mission of `mission_type`: 7 (gunner) in a mission of type 2, else 8 (rifleman); 0 for
// a protocol that keeps the profile's own.
// [orig: Game_StartMission @0x52299e..0x5229af (dfx2.exe): `movzx eax, byte_A6FB3A; sub eax, 2`,
//  then `mov byte [esi], 8` / `mov byte [esi], 7`; NetPacket_SendLoadoutSubmit @0x42ec15..0x42ec26
//  (dfx2.exe), the same byte]
uint8_t loadout_fixed_class(const Protocol *protocol, bms::MissionType mission_type);

// The Game_StartMission copy: in a live session the assigned side's profile
// page becomes the spawn buffer [orig: @0x525813
// Buffer_CopyUntilDoubleNull(restrictionData, page, 0x800)], which is BOTH
// what the local slot pool is built from [orig: Player_InitPlayer @0x4e15f0
// -> AvatarDef_BuildDisplayList(.., restrictionData)] and what the C2S 0x2F
// serializes [orig: NetPacket_SendLoadoutSubmit @0x42cdc0]. Retail keeps one
// buffer; keeping two is how the local view and the wire drift apart. The copy
// runs on a session peer alone (a listen host, a joiner; a dedicated host copies
// nothing), and an empty page copies empty: the copy writes the page's terminator
// [orig: Game_StartMission - is_in_session @0x525767, is_mp_session_peer @0x525773;
//  Buffer_CopyUntilDoubleNull @0x562F30 (the store ahead of the double-NUL test)].
// The caller gates on a live session exactly as retail is (`is_in_session`
// covers a LISTEN HOST as well as a joiner), so single player and the editor
// keep the mission's .bms kit. Returns false (and copies nothing) while the
// side latch has not landed — an UNLATCHED team must not commit a page (the
// selector is the S2C 0x04 tail byte [orig: g_LocalTeam @0x425499];
// anything-not-1-or-3 selects the RED block @0x525798, so seeding at team 0
// would commit the wrong side's page and then have to flip it).
// A protocol that fixes the class first writes it into the side block the team names
// (loadout_fixed_class: dfx2.exe's 8, or 7 in a mission of type 2), then picks that class's page,
// so every later read of the block (a self S2C 0x50's re-copy of the same side, the 0x2F's
// side kits) reads the class the mission start wrote; the other side's block keeps its own.
// [orig: Game_StartMission @0x52299e..0x5229bd (dfx2.exe) - the class store into the side block
//  @0x5229aa / @0x5229af, the page switch over it @0x5229b2..0x5229bd, the copy @0x522a04]
bool seed_session_kit_from_profile(world::World &world, playersav::Record &profile,
		const Protocol *protocol, bms::MissionType mission_type, uint8_t assigned_team,
		world::LocalPlayerLoadout &loadout, int &seeded_side);

// The single player's fill of the spawn buffer (g_SpawnLoadoutBuffer) at its mission start and
// at its restart: out of a session, with no kit the mission's own chunk promoted over it, the
// current profile's single-player page (weapon.sav's record +65548, written by the profile
// screen's save) when there is a profile and its page's first name is not empty, else the
// static page (weapon_kit_default: "WPN_M4AUTO" "-1" "-1" "-1" ..). The fill precedes the
// mission load, whose non-empty kit chunk overwrites it; ours' kernel promotes that chunk first,
// so a promoted kit (mission_kit_promoted) keeps it. Copied raw, unfiltered. Returns true when it
// wrote (the caller rebuilds the slot pool). The host role runs its static arm at every single
// player bring-up (no profile), the shell its profile arm after the table load.
// [orig: Game_ApplySessionSettingsToGlobals @0x551500 (@0x5519A6..0x5519E4: the mission entry's
//  loadout @0x5519A6..0x5519BB, the profile page byte_256113C[slot * 0x1080C] @0x5519BD..0x5519D8,
//  the static page @0x5519DA, Buffer_CopyUntilDoubleNull @0x5519E4; from SinglePlayer_StartMission
//  @0x561AF0 (the call @0x561C28)); Game_StartMission @0x524360 (the restart copy
//  @0x524683..0x5246C8, currentProfileSlot @0x52469D); Mission_LoadBMSFile @0x40F4E0 (the kit
//  chunk @0x40F6FF..0x40F77C)]
bool seed_single_player_kit_from_profile(world::World &world,
		const playersav::Record *profile, world::LocalPlayerLoadout &loadout);

// The side-change re-copy rule, for a side change no self S2C 0x50 carried: the
// S2C 0x04 latch landing after the catalog, a case retail never has, because its
// latch precedes Game_StartMission's copy. Whenever the SIDE the selector names
// stops matching the side the resident buffer was copied from, re-copy, as that
// copy would have read the latch. Keyed on the side rather than the raw team: a
// 1<->3 (or 2<->4) move reads the same block @0x525798, and an in-session armory
// ACCEPT, which changes the buffer but never the side, stands. A self 0x50
// re-copies whatever the side (reseed_session_kit_on_team_assign). Returns true
// when it re-seeded (the caller re-runs its loadout rebuild + push).
// [orig: Game_StartMission @0x524360 - the side pick @0x525798, Buffer_CopyUntilDoubleNull
//  (the call @0x525818)]
bool reseed_session_kit_on_side_change(world::World &world, playersav::Record &profile,
		const Protocol *protocol, bms::MissionType mission_type, uint8_t assigned_team,
		world::LocalPlayerLoadout &loadout, int &seeded_side);

// A self S2C 0x50's local arm: the page of the side `team` names (teams 1 and 3 the blue
// block, every other the red), for that side's class byte, copied over the resident buffer
// whatever it held (a 0x5A grant's kit, an armory ACCEPT's) and whether or not the side
// moved; a class outside 5..9 copies nothing. The Player_InitPlayer(1) the handler runs
// next rebuilds the slots from it. Returns true when it copied.
// [orig: NapiNPClientMsg_TeamAssign @0x431910 - the g_LocalTeam read @0x431A27, the side
//  pick @0x431A2D..0x431A54, the class switch @0x431A59..0x431A66, the copy `rep movsd`
//  @0x431A9A; Player_InitPlayer @0x4E15F0 - AvatarDef_BuildDisplayList(g_SpawnLoadoutBuffer)
//  @0x4E1882]
bool reseed_session_kit_on_team_assign(const playersav::Record &profile, uint8_t team,
		world::LocalPlayerLoadout &loadout, int &seeded_side);

// The player's weapon profile record the mission starts copy their kit from (retail's
// g_CharSelClass slot), with the side the resident buffer was last copied from. It is
// player-scoped: it outlives every mission's kernel and is installed before the first, so
// its embedder holds it. It starts as the shipped defaults' record, never empty with no
// weapon.sav.
struct SessionWeaponProfile {
	playersav::Record record = playersav::make_defaults().slots[0];
	bool loaded = false;  // a profile's record was installed (use)
	int seeded_side = -1; // -1: the buffer not yet copied from a side

	// A profile's current weapon record, with the session start's class clamp
	// (playersav::clamp_classes carries the witness).
	void use(const playersav::Record &current);
	// The mission start's kit from the record: a session peer (a listen host or a
	// joiner) copies its side's page (seed_session_kit_from_profile), single player its
	// page arm over the record once one is installed (seed_single_player_kit_from_profile).
	// Returns true when it wrote (the caller rebuilds the slot pool).
	// `protocol` and `mission_type` are the session's (the class a protocol fixes,
	// seed_session_kit_from_profile); the single-player arm reads neither.
	bool seed_kit(world::World &world, bool session_peer, const Protocol *protocol,
			bms::MissionType mission_type, uint8_t assigned_team,
			world::LocalPlayerLoadout &loadout);
};

// The joiner's C2S 0x2F submission content — the wire seam D-NET-168
// tracked. Builds the full LoadoutKit: the ONE class integer that drives
// both the wire class and the page pick (retail reads it straight out of the
// assigned side's profile block [orig: Game_StartMission @0x5257a0], never
// from the sim-side class pick), the resident-buffer rows, and both
// side blocks for the S2C 0x50 resubmission path. `current_slot` is the
// current slot word (WeaponInventory::current_slot) for the pair's SECOND
// submit, -1 before any fill (the seam then sends 195)
// [orig: Game_StartMission @0x525c2e passes g_CurrentWeaponSlot (@0x525C1A)].
// The S2C 0x5A authoritative grant -> spawn-kit rows: names resolve through
// the weapon table (retail drops failed AdmDef lookups); the wire bytes are
// SIGNED clip counts — 0xFF is the authored/default sentinel, not 255 clips
// [orig: the grant apply @ 0x4295c4..0x429613].
void kit_from_authoritative_grant(const world::WeaponTable &weapons,
		const WeaponLoadout &grant,
		std::vector<world::WeaponKitEntry> &r_kit);

// The weapon a client's S2C 0x5A apply selects (world::local_loadout_apply_accept's
// `select`), as the session's protocol has it (ProtocolInMatch::s2c_5a): JO's the weapon the
// body holds, DFX2's the grant's last primary row.
// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - @0x4296BC..0x4296D4;
//  NapiNPClientMsg_HandleWeaponLoadoutSync @0x42AF10 (dfx2.exe) - @0x42B0AB..0x42B0BD,
//  @0x42B4F3..0x42B50B]
world::LoadoutApplySelect grant_apply_select(const Protocol *protocol);

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
// every body the process spawns takes; the spawn buffer waits for the host's
// list); off the authority the slot table empties; and the 0x2F
// the apply sends is returned for the caller's queue: the local body's team byte, the
// armory's class (g_ArmorySelectedClass, the class picked on the armory screen), the
// selection through the sender's name gate and g_CurrentWeaponSlot (WeaponInventory::
// current_slot, which the pool reset leaves) resolved against the table as the sender
// resolves it: past the joiner's reset, the raw word. DFX2's sender fixes the class.
// Nothing else applies locally: the host's 0x5A grant rebuilds the slots, and a listen
// host's own slot accepts the 0x2F into the local table (D-NET-695, D-NET-843).
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
// `protocol` is the session's (its client's view's: ClientReplicaPipeline::protocol), whose
// c2s_2f.loadout_class_fixed (DFX2-12) the sender's class follows.
LoadoutSubmit armory_accept_in_session(world::World &world, world::LocalPlayer &local,
		const Protocol *protocol,
		const std::vector<world::WeaponKitEntry> &kit, uint8_t armory_class, bool authority,
		bms::MissionType mission_type);

void build_joiner_loadout_kit(const world::World &world,
		const playersav::Record &profile, uint8_t assigned_team,
		const world::LocalPlayerLoadout &loadout, int current_slot,
		const world::WeaponInventory *inventory,
		JoinerConnection::LoadoutKit &out);

} // namespace opennova::inmatch
