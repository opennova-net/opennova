// weapon.def -> world::WeaponTable, plus the witnessed loadout-service rules the armory table
// feeds (D-NET-141): the adm index allocation, the charfilter/teamfilter permission masks, and
// the S2C 0x5A ammo-byte resolution. Every rule is a faithful port of the 2026-07-02 grill
// witnesses in docs/net/novaworld-net-re.md §5.57.
#pragma once

#include <cstdint>

#include <def/def.h>
#include <world/weapon_table.h>

namespace opennova::np {

// charfilter token -> bit: medic=1 sniper=2 gunner=4 rifleman=8 engineer=0x10
// [orig: token table @0x830EB0, OR-ed @0x543F6E]. 0 = unrecognized (the original warns + skips).
uint8_t charfilter_bit(const char *token);

// teamfilter token -> bit: red=1 blue=2 [orig: token table @0x830ED8, OR-ed @0x543FE3].
uint8_t teamfilter_bit(const char *token);

// Whether one armory entry is visible to a player of (team, player_class) — the reply
// builder's slot filter [orig: Server_SendWeaponSlotListToPlayer @0x502716]:
//   team mask: team 1/3 -> blue(2), 2/4 -> red(1), else both(3)       [orig: @0x502666]
//   char mask: class 5..9 -> 1<<(c-5), 1..3 -> all, else none         [orig: @0x502693]
// Both masks must intersect the entry's teamfilter/charfilter.
bool loadout_entry_permitted(const world::WeaponTableEntry &entry, uint8_t team,
                             uint8_t player_class);

// The S2C 0x5A per-slot ammo bytes for one accepted request entry.
//   primary  = the slot's TOTAL AMMO IN CLIPS [orig: WeaponSlot_GetTotalClips @0x5425F0]:
//              clipsize -1 -> 0xFF (@0x542669); pool/clipsize (@0x542673) clamped 127
//              (@0x542678); pool = requested != 0xFF ? min(requested, maxclips)*clipsize
//              : startrounds (the 0x2F accept fill, §5.57 @0x515550).
//   secondary = the same count for the FIRST different-ammoclass sub-variant in
//              parent+1..parent+loadout_subclasses (its own startrounds fill), else 0xFF
//              [orig: @0x5027c8-@0x502847].
struct LoadoutAmmoBytes {
	uint8_t primary = 0xFF;
	uint8_t secondary = 0xFF;
};
LoadoutAmmoBytes resolve_loadout_ammo(const world::WeaponTable &table, uint8_t adm_index,
                                      uint8_t requested);

// Build the armory table from a parsed weapon.def. Index allocation is the witnessed rule:
// entry 0 = the engine "null" [orig: AnimDef_InitAll @0x543615]; each weapon block reuses an
// existing same-name entry (case-insensitive) else takes the lowest free slot [orig:
// WeaponDefs_ParseLineCallback @0x5436e1 -> AdmDef_FindFreeSlot @0x53FC50] — pure file order
// for one parse into a fresh table. Absent clipsize/startrounds keys read the engine defaults
// (1 / -1) [orig: AdmDef_InitEntryDefaults @0x53ff13/@0x53ff19].
world::WeaponTable build_weapon_table(const DefWeaponsFile &weapons);

} // namespace opennova::np
