#include <runtime/menu/player_info_kit.h>

#include <algorithm>

namespace opennova::menu {

std::vector<int32_t> player_info_voice_values(int32_t sex) {
	// DEFAULT_VOICE leads, then the enabled rows of the head's sex in table
	// order [orig: PlayerInfo_PopulatePlayerVoiceCombo @ 0x55dce0].
	std::vector<int32_t> values;
	values.push_back(kDefaultVoiceValue);
	for (const PlayerVoiceRow &row : kPlayerVoiceTable) {
		if (!row.enabled || row.sex != sex) continue;
		values.push_back(row.id);
	}
	return values;
}

int32_t player_info_voice_selection(int32_t saved, const std::vector<int32_t> &values) {
	// [orig: `if (!found) profile[team + 1532] = 0` @ 0x55de21]
	return std::find(values.begin(), values.end(), saved) != values.end() ? saved
																			: kDefaultVoiceValue;
}

namespace {

// An entry retail writes with the "-1" filler in all three value slots.
playersav::KitEntry filler_entry(const char *name) {
	playersav::KitEntry e;
	e.name = name;
	e.ammo_primary = kKitFiller;
	e.ammo_secondary = kKitFiller;
	e.flags = kKitFiller;
	return e;
}

// One catalog row with its recorded interleaved count pair: row 0 is the
// seeded "None", any other the weapon.def row before it.
playersav::KitEntry slot_entry(const KitSlotPick &pick, const WeaponNameLookup &weapon_name) {
	playersav::KitEntry e;
	if (pick.weapon_index == 0) e.name = kCatalogNoneName;
	else if (weapon_name) e.name = weapon_name(pick.weapon_index - 1);
	e.ammo_primary = pick.ammo_primary;
	e.ammo_secondary = pick.ammo_secondary;
	e.flags = pick.flags;
	return e;
}

} // namespace

std::vector<playersav::KitEntry> player_info_kit_entries(const PlayerInfoKitSelection &sel,
		const WeaponNameLookup &weapon_name) {
	std::vector<playersav::KitEntry> out;
	// The knife leads every page: the BLUE mask (2) -- and the defensive
	// mask-zero leg -- take WPN_KNIFE, the RED mask (1) takes WPN_KNIFE2
	// [orig: @0x55e4dc].
	out.push_back(filler_entry(
			(sel.team_mask & 2) != 0 || sel.team_mask == 0 ? kKitKnifeBlue : kKitKnifeRed));
	if (sel.player_class == kMedicPlayerClass) out.push_back(filler_entry(kKitMedpack)); // [orig: @0x55e624]
	// PRIMARY and SECONDARY carry their team's ammo-type byte as the entry's
	// fourth value; ACCESSORY always writes the filler (the caller's flags).
	out.push_back(slot_entry(sel.primary, weapon_name));
	out.push_back(slot_entry(sel.secondary, weapon_name));
	out.push_back(slot_entry(sel.accessory, weapon_name));
	// The three grenade slots are a FIXED array [orig: @0x55e7e0].
	for (const KitSlotPick &g : sel.grenades) out.push_back(slot_entry(g, weapon_name));
	return out;
}

} // namespace opennova::menu
