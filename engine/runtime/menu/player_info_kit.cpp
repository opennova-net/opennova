#include <runtime/menu/player_info_kit.h>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <array>

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

namespace {

// The catalog row a name names: the first match, case-insensitive, over the
// seeded "None" (row 0) and then weapon.def's rows; -1 for none
// [orig: EntityDef_FindIndexByName @ 0x563020]. An entry no `end` closed is no row.
int32_t catalog_row_by_name(const def::DefWeaponsFile &weapons, const char *name) {
	if (strutil::iequals(kCatalogNoneName, name)) return 0;
	for (size_t i = 0; i < weapons.count; ++i) {
		const def::DefWeaponDef &w = weapons.entries[i];
		if (w.unclosed == 0 && strutil::iequals(w.weapon_name, name))
			return catalog_index_of_row(static_cast<int32_t>(i));
	}
	return -1;
}

} // namespace

PlayerInfoKitRestore player_info_restore_kit(const playersav::KitPage &page,
		const def::DefWeaponsFile &weapons) {
	PlayerInfoKitRestore out;
	// The page as the record holds it: NUL-separated strings closed by an empty one.
	std::array<uint8_t, playersav::kKitPageBytes> bytes{};
	playersav::encode_kit_page(page, bytes.data(), bytes.size());
	const auto at = [&bytes](size_t offset) {
		return offset < bytes.size() ? reinterpret_cast<const char *>(bytes.data() + offset) : "";
	};
	const auto past = [&bytes](size_t offset) {
		while (offset < bytes.size() && bytes[offset] != 0) ++offset;
		return offset + 1;
	};
	// The CRT's atol as the game links it [orig: j__atol @0x76AB1B].
	const auto value = [&at](size_t offset) { return io::retail_atol(at(offset)); };
	// The three grenade slots and the interleaved count pairs are one block: the
	// slots zeroed, every pair -1 [orig: @0x55e8d0..0x55e8da, @0x55e8e0..0x55e8f5].
	const size_t rows = std::max(kPlayerInfoCatalogRows, weapons.count + 1);
	std::vector<int32_t> block(3 + 2 * rows, -1);
	block[0] = block[1] = block[2] = 0;
	size_t slot = 0;
	for (size_t cursor = 0; cursor < bytes.size() && bytes[cursor] != 0;) {
		const int32_t row = catalog_row_by_name(weapons, at(cursor));
		if (row <= 0) {
			cursor = past(cursor); // one string on [orig: @0x55eadb]
			continue;
		}
		const def::DefWeaponDef &w = weapons.entries[row - 1];
		const size_t primary = past(cursor);
		block[3 + 2 * row] = value(primary); // [orig: @0x55e945]
		const size_t secondary = past(primary);
		// Stored when a row within loadout_subclasses carries another round type
		// [orig: @0x55e963..0x55e9bf, the store @0x55e9a4].
		if (def::def_subclass_weapon_index(weapons.entries, weapons.count, row - 1) >= 0)
			block[4 + 2 * row] = value(secondary);
		const size_t flags_at = past(secondary);
		const int8_t flags = static_cast<int8_t>(value(flags_at)); // [orig: @0x55e9db]
		cursor = past(flags_at);
		if (w.loadout_selectable == 0) continue; // [orig: @0x55e9f0]
		switch (w.weapon_class_slot) {
			case 0: out.select[2] = row; break;
			case 1:
				out.select[0] = row;
				out.has_type[0] = true;
				out.type[0] = flags;
				break;
			case 2:
				out.select[1] = row;
				out.has_type[1] = true;
				out.type[1] = flags;
				break;
			case 3:
				if (slot < block.size()) block[slot++] = row; // [orig: @0x55eac1]
				break;
			default: break;
		}
	}
	for (size_t i = 0; i < 3; ++i) out.grenade_slots[i] = block[i];
	out.ammo_primary.resize(rows);
	out.ammo_secondary.resize(rows);
	for (size_t r = 0; r < rows; ++r) {
		out.ammo_primary[r] = block[3 + 2 * r];
		out.ammo_secondary[r] = block[4 + 2 * r];
	}
	return out;
}

} // namespace opennova::menu
