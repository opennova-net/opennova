#include "npruntime/weapon_table_build.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace opennova::np {

namespace {

bool ci_equal(const char *a, const char *b) {
	while (*a != '\0' && *b != '\0' &&
	       std::tolower(static_cast<unsigned char>(*a)) ==
	               std::tolower(static_cast<unsigned char>(*b)))
		++a, ++b;
	return *a == '\0' && *b == '\0';
}

// The slot's TOTAL AMMO IN CLIPS [orig: WeaponSlot_GetTotalClips @0x5425F0]. The pool a fresh
// 0x2F accept fills is `requested != 0xFF ? min(requested, maxclips) * clipsize : startrounds`
// (§5.57 fill [orig: PlayerSlot_InitWeaponsFromLoadout @0x515550]); the count divides it back
// by clipsize. A negative result rides the signed byte to the wire (0xFF = the -1 "default /
// no count" sentinel the client answers with its own startrounds fallback).
int32_t clips_of(const world::WeaponTableEntry &e, uint8_t requested) {
	if (e.clipsize < 0) return -1; // no-clip weapon (knife/medpack/emplaced) [orig: @0x542669]
	const int32_t pool = requested != 0xFF
			? std::min<int32_t>(requested, e.maxclips) * e.clipsize
			: e.startrounds;
	int32_t clips = e.clipsize > 0 ? pool / e.clipsize : pool; // 0 divisor skipped [orig: @0x542670]
	if (clips > 127) clips = 127;                              // [orig: @0x542678]
	return clips;
}

} // namespace

uint8_t charfilter_bit(const char *token) {
	// [orig: token table @0x830EB0]
	if (token == nullptr) return 0;
	if (ci_equal(token, "medic")) return 0x01;
	if (ci_equal(token, "sniper")) return 0x02;
	if (ci_equal(token, "gunner")) return 0x04;
	if (ci_equal(token, "rifleman")) return 0x08;
	if (ci_equal(token, "engineer")) return 0x10;
	return 0; // unrecognized -> no bit (the original warns "unrecognized character type")
}

uint8_t teamfilter_bit(const char *token) {
	// [orig: token table @0x830ED8]
	if (token == nullptr) return 0;
	if (ci_equal(token, "red")) return 0x01;
	if (ci_equal(token, "blue")) return 0x02;
	return 0;
}

bool loadout_entry_permitted(const world::WeaponTableEntry &entry, uint8_t player_class,
                             uint8_t soldier_type) {
	// [orig: Server_SendWeaponSlotListToPlayer — team mask @0x502666, char mask @0x502693,
	//  the slot filter @0x502716]
	uint8_t team_mask;
	switch (player_class) {
		case 1:
		case 3:
			team_mask = 0x02; // blue
			break;
		case 2:
		case 4:
			team_mask = 0x01; // red
			break;
		default:
			team_mask = 0x03; // spectators/unknown see both sides
			break;
	}
	uint8_t char_mask = 0;
	if (soldier_type >= 5 && soldier_type <= 9)
		char_mask = static_cast<uint8_t>(1u << (soldier_type - 5));
	else if (soldier_type >= 1 && soldier_type <= 3)
		char_mask = 0xFF; // the pre-MP persona classes pass everything [orig: @0x50269a]
	return (team_mask & entry.teamfilter) != 0 && (char_mask & entry.charfilter) != 0;
}

LoadoutAmmoBytes resolve_loadout_ammo(const world::WeaponTable &table, uint8_t adm_index,
                                      uint8_t requested) {
	LoadoutAmmoBytes out;
	const world::WeaponTableEntry *e = table.by_index(adm_index);
	if (e == nullptr) return out;
	const int32_t primary = clips_of(*e, requested);
	out.primary = primary < 0 ? 0xFF : static_cast<uint8_t>(primary);
	// The alt-ammo byte: the FIRST sub-variant (parent+1..parent+LSC) whose ammo class differs
	// from the parent's — e.g. an M203HE under an M4M203AUTO — counted from its own startrounds
	// fill. [orig: @0x5027c8 walk, class compare @0x5027f8, count @0x502830-@0x502847]
	if (e->loadout_subclasses > 0) {
		for (uint8_t k = 1; k <= e->loadout_subclasses; ++k) {
			const world::WeaponTableEntry *sub =
					table.by_index(static_cast<uint8_t>(adm_index + k));
			if (sub == nullptr) continue;
			if (!ci_equal(sub->ammo_class.c_str(), e->ammo_class.c_str())) {
				const int32_t alt = clips_of(*sub, 0xFF);
				out.secondary = alt < 0 ? 0xFF : static_cast<uint8_t>(alt);
				break;
			}
		}
	}
	return out;
}

world::WeaponTable build_weapon_table(const DefWeaponsFile &weapons) {
	world::WeaponTable table;

	// Entry 0: the engine-created "null" def — AnimDef_InitAll wipes the 255-entry table and
	// names slot 0 right before weapon.def parses [orig: @0x543615; Game_StartMission
	// @0x5254b3/@0x5254bd]. Its fields keep the InitEntryDefaults values.
	world::WeaponTableEntry null_entry;
	null_entry.name = "null";
	null_entry.valid = true;
	table.entries.push_back(std::move(null_entry));

	for (size_t i = 0; i < weapons.count; ++i) {
		const DefWeaponDef &d = weapons.entries[i];
		world::WeaponTableEntry e;
		e.name = d.weapon_name;
		e.valid = true;
		// Parse-time range clamps [orig: category @0x543999, rank @0x5439ed — warn + reset 0].
		e.category = (d.category >= 0 && d.category < 12) ? static_cast<uint8_t>(d.category) : 0;
		e.rank = (d.rank >= 0 && d.rank <= 64) ? static_cast<uint8_t>(d.rank) : 0;
		// The flat parser reads absent keys as 0; the engine entry defaults are clipsize 1 /
		// startrounds -1 [orig: AdmDef_InitEntryDefaults @0x53ff13/@0x53ff19]. No shipped
		// weapon.def uses an explicit 0 for either key, so 0 == absent here.
		e.clipsize = static_cast<int16_t>(d.clipsize == 0 ? 1 : d.clipsize);
		e.startrounds = static_cast<int16_t>(d.startrounds == 0 ? -1 : d.startrounds);
		e.maxclips = static_cast<int16_t>(d.maxclips);
		for (size_t c = 0; c < d.charfilter_count; ++c)
			e.charfilter |= charfilter_bit(d.charfilter[c]);
		for (size_t t = 0; t < d.teamfilter_count; ++t)
			e.teamfilter |= teamfilter_bit(d.teamfilter[t]);
		e.loadout_selectable = static_cast<uint8_t>(d.loadout_selectable != 0);
		e.loadout_subclasses = static_cast<uint8_t>(d.loadout_subclasses);
		e.ammo_class = d.ammo_class;
		e.ammo_class_count = static_cast<int16_t>(d.ammo_class_count);
		e.ammo_bucket = static_cast<int16_t>(d.ammobucket);
		e.round_type = d.round_type; // resolved to an AmmoTable index by
		                             // resolve_weapon_round_types (§5.60)

		// Allocation rule [orig: WeaponDefs_ParseLineCallback @0x5436e1]: reuse an existing
		// same-name entry (re-parse override), else the LOWEST free slot [orig:
		// AdmDef_FindFreeSlot @0x53FC50] — append, since this lifecycle never frees entries.
		const int existing = table.index_of(d.weapon_name);
		if (existing >= 0) {
			table.entries[static_cast<size_t>(existing)] = std::move(e);
		} else if (table.entries.size() < 255) { // table capacity [orig: 255 entries @0x53fc6b]
			table.entries.push_back(std::move(e));
		}
	}
	return table;
}

} // namespace opennova::np
