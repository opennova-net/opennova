#include "loadout_screen.h"

#include "loadout_labels.h"
#include "menu_runtime.h"
#include "player_info_kit.h"
#include <base/io/strutil.h>
#include <runtime/world/player_loadout.h>

#include <algorithm>
#include <iterator>

namespace opennova::menu {
namespace {

const char *const kParents[] = {"PRIMARY", "SECONDARY", "ACCESSORY"};
const char *const kGrenades[] = {"GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"};

const def::DefWeaponDef *weapon(const def::DefWeaponsFile &file, int index) {
	return index >= 0 && static_cast<size_t>(index) < file.count ? file.entries + index : nullptr;
}

int saved_count(const LoadoutAmmoCounts &counts, int index) {
	const auto found = counts.find(index);
	return found == counts.end() ? -1 : found->second;
}

void fill_clips(MenuRuntime &menu, int id, const def::DefWeaponDef *w,
		int first, int maximum, const hud::GameTextLookup &text) {
	std::vector<std::string> rows;
	for (int clips = first; clips <= maximum; ++clips) rows.push_back(ammo_row_label(w, clips, text));
	menu.set_widget_items(id, rows);
	if (!rows.empty()) menu.select_row(id, 0, false);
}

std::vector<int> grenade_indices(const def::DefWeaponsFile &weapons, int class_mask, int team_mask) {
	std::vector<int32_t> indices;
	world::weapon_slot_indices(weapons.entries, weapons.count, 3, class_mask, team_mask, indices);
	if (indices.size() > std::size(kGrenades)) indices.resize(std::size(kGrenades));
	return {indices.begin(), indices.end()};
}

} // namespace

int armory_selected_clips(const MenuRuntime &menu, const std::string &control) {
	const int combo = menu.widget_id(control + "_AMMO1");
	if (combo < 0 || menu.selected_row(combo) < 0 || menu.item_count(combo) == 0) return -1;
	return menu.selected_row(combo) + 1;
}

LoadoutSlotRows loadout_fill_slot(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int slot, int class_mask, int team_mask,
		const WeaponAvailability &availability, const std::string &none_label,
		const hud::GameTextLookup &gametext) {
	const int combo = menu.widget_id(control);
	if (combo < 0) return {};
	std::vector<int32_t> filtered;
	world::weapon_slot_indices(weapons.entries, weapons.count, slot, class_mask, team_mask, filtered);
	// [orig: the map availability skip @0x566e6b: a banned (0) weapon never lists]
	std::vector<int32_t> listed;
	std::vector<std::string> labels;
	for (const int32_t index : filtered) {
		const auto *w = weapon(weapons, index);
		if (w == nullptr || (availability && availability(w->weapon_name) == 0)) continue;
		listed.push_back(index);
		labels.push_back(weapon_label(*w, gametext));
	}
	LoadoutSlotRows rows{ -1 };
	std::vector<std::string> items{ none_label };
	for (const int i : armory_slot_order(labels)) {
		items.push_back(labels[static_cast<size_t>(i)]);
		rows.push_back(listed[static_cast<size_t>(i)]);
	}
	menu.set_widget_items(combo, items);
	menu.select_row(combo, 0, false);
	return rows;
}

int32_t loadout_selected_weapon(const MenuRuntime &menu, const std::string &control,
		const LoadoutSlotRows &rows) {
	const int combo = menu.widget_id(control);
	if (combo < 0) return -1;
	const int row = menu.selected_row(combo);
	return row >= 0 && static_cast<size_t>(row) < rows.size() ? rows[static_cast<size_t>(row)] : -1;
}

void loadout_select_weapon_row(MenuRuntime &menu, const std::string &control,
		const LoadoutSlotRows &rows, int32_t weapon_row) {
	const int combo = menu.widget_id(control);
	if (combo < 0) return;
	int found = -1;
	for (size_t row = 0; row < rows.size(); ++row) {
		if (rows[row] >= 0 && rows[row] == weapon_row) {
			found = static_cast<int>(row);
			break;
		}
	}
	menu.select_row(combo, found, false);
}


// [orig: PlayerInfo_PopulateAmmoComboBoxes @0x55def0; the ACCESSORY leg is the same
//  logic inlined in PlayerInfo_PopulateWeaponAccessoryAmmoUI @0x55e8b0]
int player_info_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, int saved_primary, int saved_secondary,
		int saved_type, const hud::GameTextLookup &text) {
	const auto *w = weapon(weapons, parent);
	const bool has_ammo = w && w->clipsize > 0;
	// The ammo-type combo and byte are PRIMARY's and SECONDARY's alone [orig: the
	// "PRIMARY_AMMO1_TYPE" / "SECONDARY_AMMO1_TYPE" names @0x55df5c / @0x55df64, the
	// third a null @0x55df6c].
	const bool typed =
			strutil::iequals(control, "PRIMARY") || strutil::iequals(control, "SECONDARY");
	const int ammo1 = menu.widget_id(control + "_AMMO1");
	const int type = typed ? menu.widget_id(control + "_AMMO1_TYPE") : -1;
	const int ammo2 = menu.widget_id(control + "_AMMO2");
	// The TYPE combo and its byte ride AMMO1: a screen without it touches neither
	// [orig: the AMMO1 test @0x55e040].
	if (ammo1 >= 0) {
		menu.set_widget_shown(ammo1, has_ammo);
		if (type >= 0) menu.set_widget_shown(type, has_ammo);
		if (has_ammo) {
			// The byte's row, a page's -1 filler read as 0 (the byte keeps -1), selected
			// ahead of the NOAMMOTYPES reset over the authored FMJ/AP/SP rows; a row the
			// list lacks keeps its selection [orig: the sign-extended read @0x55e01c /
			// @0x55dfe3, -1 -> 0 @0x55e08a..0x55e091, CListWnd_SetRowVisibility
			// @0x55e0af, which refuses a row outside the list @0x6451a5].
			const int shown_type = saved_type == -1 ? 0 : saved_type;
			if (type >= 0 && shown_type >= 0 && shown_type < menu.item_count(type))
				menu.select_row(type, shown_type, false);
			// NOAMMOTYPES locks the combo and resets the byte, with no combo too
			// [orig: @0x55def0 +188 & 0x40 @0x55e0ca -> UIWidget_SetInteractiveRecursive
			//  @0x55e0d4; the resets @0x55e0f0 / @0x55e0fe].
			const bool locked = (w->flags2 & def::DEF_WEAPON_FLAG2_NOAMMOTYPES) != 0;
			if (type >= 0) menu.set_widget_disabled(type, locked);
			if (locked && typed) saved_type = 0;
			fill_clips(menu, ammo1, w, 1, w->maxclips, text);
			// [orig: saved == i || (saved == -1 && i == maxclips), both fills]
			menu.select_row(ammo1, world::player_info_default_clip_row(saved_primary, w->maxclips) - 1, false);
		}
	}
	if (ammo2 >= 0) {
		// [orig: the stricmp walk over entry+192.. in @0x55def0 / @0x55e8b0 / @0x55f1f0]
		const auto *sub = weapon(weapons, def::def_subclass_weapon_index(weapons.entries, weapons.count, parent));
		const bool sub_ok = has_ammo && sub && sub->clipsize > 0;
		menu.set_widget_shown(ammo2, sub_ok);
		if (sub_ok) {
			fill_clips(menu, ammo2, sub, 1, sub->maxclips, text);
			menu.select_row(ammo2, world::player_info_default_clip_row(saved_secondary, sub->maxclips) - 1, false);
		}
	}
	return saved_type;
}

// First three selectable class-3 definitions in table order; hide leftovers.
// Counts include zero [orig: PlayerInfo_PopulateAmmoComboBoxes @0x55def0, the >= 3 leg].
std::vector<int> player_info_fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const LoadoutAmmoCounts &saved, const hud::GameTextLookup &text) {
	const auto indices = grenade_indices(weapons, class_mask, team_mask);
	for (size_t i = 0; i < std::size(kGrenades); ++i) {
		const int combo = menu.widget_id(kGrenades[i]);
		if (combo < 0) continue;
		menu.set_widget_shown(combo, i < indices.size());
		if (i >= indices.size()) continue;
		const auto *w = weapon(weapons, indices[i]);
		fill_clips(menu, combo, w, 0, w->maxclips, text);
		menu.select_row(combo, world::player_info_default_grenade_row(saved_count(saved, indices[i]), w->maxclips), false);
	}
	return indices;
}



// Preserve the companion's grouping: sub-weapon terms first, then ONE parent
// sum, then grenade terms. Parent counts <=0 use maxclips; sub-weapons also
// treat zero as default, but a chosen grenade zero weighs nothing.
// [orig: PlayerInfo_CalculateLoadoutWeight @0x55f1f0 -- weaponweight + clips*clipweight;
//  sub-weapons/grenades contribute the clip term ONLY]
double player_info_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades,
		const LoadoutAmmoCounts &primary, const LoadoutAmmoCounts &secondary) {
	std::vector<def::DefWeaponDef> rows;
	std::vector<int> counts;
	double total = 0.0;
	for (size_t i = 0; i < parents.size(); ++i) {
		const auto *w = weapon(weapons, parents[i]);
		if (!w) continue;
		rows.push_back(*w);
		counts.push_back(saved_count(primary, parents[i]));
		const auto *sub = weapon(weapons, def::def_subclass_weapon_index(weapons.entries, weapons.count, parents[i]));
		// AMMO2 existence gates its term; being hidden does not.
		if (menu.widget_id(std::string(kParents[i]) + "_AMMO2") >= 0 && sub && sub->clipsize > 0) {
			const int saved = saved_count(secondary, parents[i]);
			total += def::def_extra_ammo_weight(sub, saved <= 0 ? -1 : saved);
		}
	}
	total += def::def_loadout_weight(rows.data(), counts.data(), rows.size());
	for (size_t i = 0; i < grenades.size() && i < std::size(kGrenades); ++i) {
		const int combo = menu.widget_id(kGrenades[i]);
		if (combo < 0 || !menu.is_widget_shown(combo)) continue;
		total += def::def_extra_ammo_weight(weapon(weapons, grenades[i]), saved_count(primary, grenades[i]));
	}
	return total;
}

// [orig: UI_CalculateEquippedWeaponsWeight @0x565490 - the weapon term adm[85] / 65536
//  @0x5654e4, AMMO1 @0x565504..0x56554d and AMMO2 @0x56556d..0x5655b6 (the shown gates,
//  (row + 1) * the row value's adm[84] / 65536), the grenade @0x5655d6..0x56561c (row *
//  adm[84] / 65536); a float total]. The AMMO1 / AMMO2 rows carry the category's weapon
// (their fill's value), the grenade rows the position's.
double armory_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades) {
	float total = 0.0f;
	const auto shown_row = [&menu](const std::string &name) {
		const int id = menu.widget_id(name);
		return id >= 0 && menu.is_widget_shown(id) ? menu.selected_row(id) : -1;
	};
	const auto add = [&total](double term) { total = static_cast<float>(term + total); };
	for (size_t i = 0; i < parents.size(); ++i) {
		const auto *w = weapon(weapons, parents[i]);
		if (menu.widget_id(kParents[i]) >= 0) {
			if (w) add(static_cast<double>(w->weaponweight));
			for (const char *ammo : {"_AMMO1", "_AMMO2"}) {
				const int row = shown_row(std::string(kParents[i]) + ammo);
				if (w && row >= 0) add((row + 1) * static_cast<double>(w->clipweight));
			}
		}
		const auto *g = i < grenades.size() ? weapon(weapons, grenades[i]) : nullptr;
		const int row = shown_row(kGrenades[i]);
		if (g && row >= 0) add(row * static_cast<double>(g->clipweight));
	}
	return total;
}

// --- the per-class loadout memory -------------------------------------------------

namespace {

const playersav::KitPage kEmptyPage{};

// The count a fill selects: the row whose count `saved` names, the last (`maximum`) for an
// untouched -1, none for any other [orig: `saved == i || (saved == -1 && i == maxclips)`
// @0x564cda / @0x564e5a / @0x564880].
int armory_count_row(int saved, int first, int last, int maximum) {
	if (saved >= first && saved <= last) return saved - first;
	if (saved == -1 && maximum >= first && maximum <= last) return maximum - first;
	return -1;
}

} // namespace

void ArmoryLoadoutBuffers::stage(const playersav::Record &record, int entity_team,
		const playersav::KitPage &spawn) {
	const playersav::Side &side = entity_team == 1 || entity_team == 3 ? record.blue : record.red;
	for (int c = 5; c < kArmoryClassBuffers; ++c) {
		const size_t at = static_cast<size_t>(c - 5);
		pages_[static_cast<size_t>(c)] =
				at < side.pages.size() ? side.pages[at] : playersav::KitPage{};
	}
	pages_[1] = spawn;
}

bool ArmoryLoadoutBuffers::stage_on_show(const playersav::Record &record, int entity_team,
		const playersav::KitPage &spawn) {
	const int32_t mask = armory_team_mask(entity_team);
	if (mask == staged_team_mask_) return false;
	staged_team_mask_ = mask;
	stage(record, entity_team, spawn);
	return true;
}

const playersav::KitPage &ArmoryLoadoutBuffers::page(int player_class) const {
	return player_class >= 0 && player_class < kArmoryClassBuffers
			? pages_[static_cast<size_t>(player_class)]
			: kEmptyPage;
}

void ArmoryLoadoutBuffers::store(int player_class, playersav::KitPage page) {
	if (player_class >= 0 && player_class < kArmoryClassBuffers)
		pages_[static_cast<size_t>(player_class)] = std::move(page);
}

int32_t ArmoryScreenPairs::clips_of(int32_t catalog_row) const {
	return catalog_row >= 0 && static_cast<size_t>(catalog_row) < clips.size()
			? clips[static_cast<size_t>(catalog_row)]
			: -1;
}

int32_t ArmoryScreenPairs::secondary_of(int32_t catalog_row) const {
	return catalog_row >= 0 && static_cast<size_t>(catalog_row) < secondary.size()
			? secondary[static_cast<size_t>(catalog_row)]
			: -1;
}

void ArmoryScreenPairs::set_clips(int32_t catalog_row, int32_t value) {
	if (catalog_row < 0) return;
	if (static_cast<size_t>(catalog_row) >= clips.size())
		clips.resize(static_cast<size_t>(catalog_row) + 1, -1);
	clips[static_cast<size_t>(catalog_row)] = value;
}

void ArmoryScreenPairs::set_secondary(int32_t catalog_row, int32_t value) {
	if (catalog_row < 0) return;
	if (static_cast<size_t>(catalog_row) >= secondary.size())
		secondary.resize(static_cast<size_t>(catalog_row) + 1, -1);
	secondary[static_cast<size_t>(catalog_row)] = value;
}

void armory_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, ArmoryScreenPairs &pairs,
		const hud::GameTextLookup &text) {
	const int category = control == kParents[0] ? 0 : control == kParents[1] ? 1 : -1;
	const int ammo1 = menu.widget_id(control + "_AMMO1");
	const int type = category >= 0 ? menu.widget_id(control + "_AMMO1_TYPE") : -1;
	const int ammo2 = menu.widget_id(control + "_AMMO2");
	const auto *w = weapon(weapons, parent);
	const int32_t row = catalog_index_of_row(parent);
	if (ammo1 >= 0) {
		if (w == nullptr || w->clipsize <= 0) {
			menu.set_widget_shown(ammo1, false);
			if (type >= 0) menu.set_widget_shown(type, false);
			if (ammo2 >= 0) menu.set_widget_shown(ammo2, false);
		} else {
			menu.set_widget_shown(ammo1, true);
			if (type >= 0) {
				// The byte's row, -1 read as 0, ahead of the NOAMMOTYPES reset; a row the
				// list lacks keeps its selection [orig: @0x564c0e..0x564c2e;
				// CListWnd_SetRowVisibility @0x645190 refuses it @0x6451a5].
				const int32_t shown = pairs.type[category] == -1 ? 0 : pairs.type[category];
				if (shown >= 0 && shown < menu.item_count(type)) menu.select_row(type, shown, false);
				menu.set_widget_shown(type, true);
			}
			fill_clips(menu, ammo1, w, 1, w->maxclips, text);
			const bool locked = (w->flags2 & def::DEF_WEAPON_FLAG2_NOAMMOTYPES) != 0;
			if (type >= 0) menu.set_widget_disabled(type, locked);
			if (locked && category >= 0) pairs.type[category] = 0;
			menu.select_row(ammo1,
					armory_count_row(pairs.clips_of(row), 1, w->maxclips, w->maxclips), false);
		}
	}
	// AMMO2 over a listed weapon: its other-round subclass's rows, else hidden (the NONE
	// row's catalog entry, which carries no subclass, hides it too)
	// [orig: @0x564d46..0x564e7f; the stricmp walk @0x564d6a..0x564d9c].
	if (ammo2 >= 0) {
		const auto *sub = w ? weapon(weapons, def::def_subclass_weapon_index(weapons.entries,
										  weapons.count, parent))
							: nullptr;
		if (sub == nullptr || sub->clipsize <= 0) {
			menu.set_widget_shown(ammo2, false);
		} else {
			menu.set_widget_shown(ammo2, true);
			fill_clips(menu, ammo2, sub, 1, sub->maxclips, text);
			const int pick =
					armory_count_row(pairs.secondary_of(row), 1, sub->maxclips, sub->maxclips);
			menu.select_row(ammo2, pick, false);
		}
	}
}

std::vector<int> armory_populate_page(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutSlotRows (&rows)[3], const playersav::KitPage &page, ArmoryScreenPairs &pairs,
		const hud::GameTextLookup &text) {
	for (const char *list : kParents) {
		const int id = menu.widget_id(list);
		if (id >= 0) menu.select_row(id, 0, false);
	}
	const PlayerInfoKitRestore walk = player_info_restore_kit(page, weapons);
	pairs.clips = walk.ammo_primary;
	pairs.secondary = walk.ammo_secondary;
	for (size_t i = 0; i < std::size(kParents); ++i) {
		if (walk.select[i] >= 0)
			loadout_select_weapon_row(menu, kParents[i], rows[i], walk.select[i] - 1);
		if (i < 2 && walk.has_type[i]) pairs.type[i] = walk.type[i];
	}
	for (size_t i = 0; i < std::size(kParents); ++i) {
		const int parent = loadout_selected_weapon(menu, kParents[i], rows[i]);
		armory_fill_ammo(menu, weapons, kParents[i], parent, pairs, text);
	}
	std::vector<int> positions;
	for (const int32_t slot : walk.grenade_slots)
		positions.push_back(slot - 1);
	return positions;
}

std::vector<int> armory_fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const ArmoryScreenPairs &pairs,
		const WeaponAvailability &availability, const hud::GameTextLookup &text) {
	const auto indices = grenade_indices(weapons, class_mask, team_mask);
	for (size_t i = 0; i < std::size(kGrenades); ++i) {
		const int combo = menu.widget_id(kGrenades[i]);
		if (combo < 0) continue;
		const auto *w = i < indices.size() ? weapon(weapons, indices[i]) : nullptr;
		menu.set_widget_shown(combo, w != nullptr);
		if (w == nullptr) continue;
		const bool allowed = !availability || availability(w->weapon_name) != 0;
		const int last = allowed ? w->maxclips : 0;
		fill_clips(menu, combo, w, 0, last, text);
		const int32_t saved = pairs.clips_of(catalog_index_of_row(indices[i]));
		menu.select_row(combo, armory_count_row(saved, 0, last, w->maxclips), false);
	}
	return indices;
}

playersav::KitPage armory_serialize_page(const MenuRuntime &menu,
		const def::DefWeaponsFile &weapons, const LoadoutSlotRows (&rows)[3],
		const std::vector<int> &grenades, const ArmoryScreenPairs &pairs, int entity_team,
		int selected_class, bool every_position) {
	playersav::KitPage out;
	const auto add = [&out](const std::string &name, int32_t primary, int32_t secondary,
							 int32_t flags) {
		playersav::KitEntry entry;
		entry.name = name;
		entry.ammo_primary = primary;
		entry.ammo_secondary = secondary;
		entry.flags = flags;
		out.entries.push_back(std::move(entry));
	};
	add(entity_team == 2 || entity_team == 4 ? kKitKnifeRed : kKitKnifeBlue, kKitFiller, kKitFiller,
			kKitFiller);
	if (selected_class == kMedicPlayerClass) add(kKitMedpack, kKitFiller, kKitFiller, kKitFiller);
	for (size_t i = 0; i < std::size(kParents); ++i) {
		if (menu.widget_id(kParents[i]) < 0) continue;
		const int parent = loadout_selected_weapon(menu, kParents[i], rows[i]);
		const auto *w = weapon(weapons, parent);
		if (w == nullptr) continue;
		const int32_t row = catalog_index_of_row(parent);
		// The type byte, sign-extended: -1 for ACCESSORY [orig: @0x565b64..0x565b8d].
		const int32_t flags = i < 2 ? static_cast<int8_t>(pairs.type[i]) : -1;
		add(w->weapon_name, pairs.clips_of(row), pairs.secondary_of(row), flags);
	}
	for (size_t i = 0; i < std::size(kGrenades); ++i) {
		const int combo = menu.widget_id(kGrenades[i]);
		if (!every_position && (combo < 0 || !menu.is_widget_shown(combo)))
			continue;
		const int row = i < grenades.size() ? grenades[i] : -1;
		const auto *w = weapon(weapons, row);
		// A zeroed position (the "None" catalog row) is AdmDef entry 0, the "null" entry the
		// table's init names, which resolves: only dfx2.exe's serializer reaches it, Jointops.exe's
		// fill showing only the positions it fills.
		// [orig: AnimDef_InitAll @0x540830 - the first free entry named "null" @0x540885;
		//  AdmDef_GetEntryByIndex @0x53cef0 - only 0xFF and an unnamed entry refused (dfx2.exe)]
		if (w == nullptr && !(every_position && row < 0))
			continue;
		const int32_t count = pairs.clips_of(catalog_index_of_row(row));
		add(w != nullptr ? w->weapon_name : kAdmDefNullName, count, kKitFiller, kKitFiller);
	}
	return out;
}

} // namespace opennova::menu
