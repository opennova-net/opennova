#include "loadout_screen.h"

#include "loadout_labels.h"
#include "menu_runtime.h"
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

int armory_selected_clips(const MenuRuntime &menu, const std::string &control) {
	const int combo = menu.widget_id(control + "_AMMO1");
	if (combo < 0 || menu.selected_row(combo) < 0 || menu.item_count(combo) == 0) return -1;
	return menu.selected_row(combo) + 1;
}

} // namespace

// [orig: PlayerInfo_PopulateAmmoComboBoxes @0x55def0; the ACCESSORY leg is the same
//  logic inlined in PlayerInfo_PopulateWeaponAccessoryAmmoUI @0x55e8b0]
int player_info_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, int saved_primary, int saved_secondary,
		int saved_type, const hud::GameTextLookup &text) {
	const auto *w = weapon(weapons, parent);
	const bool has_ammo = w && w->clipsize > 0;
	const int ammo1 = menu.widget_id(control + "_AMMO1");
	const int type = menu.widget_id(control + "_AMMO1_TYPE");
	const int ammo2 = menu.widget_id(control + "_AMMO2");
	if (ammo1 >= 0) {
		menu.set_widget_shown(ammo1, has_ammo);
		if (has_ammo) {
			fill_clips(menu, ammo1, w, 1, w->maxclips, text);
			// [orig: saved == i || (saved == -1 && i == maxclips), both fills]
			menu.select_row(ammo1, world::player_info_default_clip_row(saved_primary, w->maxclips) - 1, false);
		}
	}
	if (type >= 0) {
		menu.set_widget_shown(type, has_ammo);
		if (has_ammo) {
			// Keep FMJ/AP/SP authored rows; NOAMMOTYPES locks and resets the
			// saved byte [orig: @0x55def0 +188 & 0x40 -> UIWidget_SetInteractiveRecursive].
			const bool locked = (w->flags2 & def::DEF_WEAPON_FLAG2_NOAMMOTYPES) != 0;
			menu.set_widget_disabled(type, locked);
			if (locked) saved_type = 0;
			// Retail selects the ROW INDEX of the saved type
			// (CListWnd_SetRowVisibility @0x55e0af) from the byte read BEFORE the
			// NOAMMOTYPES reset; this selects by authored value after it. The
			// two agree because player.mnu authors FMJ/AP/SP as 0/1/2 in row
			// order, and a locked combo then shows the reset row (D-MNU-9's
			// deferred cascade); a miss retains the selection.
			for (int row = 0; row < menu.item_count(type); ++row) {
				if (menu.item_value(type, row) == std::to_string(saved_type)) {
					menu.select_row(type, row, false);
					break;
				}
			}
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

// Controls are assigned BEFORE checking availability: a banned grenade keeps
// its position with a zero-only row [orig: @0x5647a4..0x5648a6;
//  WeaponDef_RegisterUICallbacks @0x567020 -> WeaponDef_UISlotSelectCallback, args 0/1/2].
std::vector<int> armory_fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const std::vector<playersav::KitEntry> &current,
		const WeaponAvailability &availability, const hud::GameTextLookup &text) {
	const auto indices = grenade_indices(weapons, class_mask, team_mask);
	for (size_t i = 0; i < std::size(kGrenades); ++i) {
		const int combo = menu.widget_id(kGrenades[i]);
		if (combo < 0) continue;
		const auto *w = i < indices.size() ? weapon(weapons, indices[i]) : nullptr;
		const bool allowed = w && (!availability || availability(w->weapon_name) != 0);
		const int maxclips = allowed ? w->maxclips : 0;
		fill_clips(menu, combo, w, 0, maxclips, text);
		if (!w) continue;
		int selected = 0;
		for (const auto &entry : current) {
			if (!strutil::iequals(entry.name, w->weapon_name)) continue;
			selected = world::player_info_default_grenade_row(entry.ammo_primary, maxclips);
			break;
		}
		menu.select_row(combo, selected, false);
	}
	return indices;
}

// The armory's current-parent reselection: counts are one-based; a changed or
// absent current name takes the full default [orig: the fill @0x565cd0].
void armory_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, const std::string &current_name,
		int current_clips, const hud::GameTextLookup &text) {
	const int combo = menu.widget_id(control + "_AMMO1");
	if (combo < 0) return;
	const auto *w = weapon(weapons, parent);
	const int maxclips = w ? w->maxclips : 0;
	fill_clips(menu, combo, w, 1, maxclips, text);
	if (maxclips <= 0) return;
	if (current_name.empty() || !strutil::iequals(w->weapon_name, current_name)) current_clips = -1;
	menu.select_row(combo, world::player_info_default_clip_row(current_clips, maxclips) - 1, false);
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

// The WEAPON screen still defers its ammo-TYPE/sub-weapon cascade. Parent
// weights come first; extra-ammo terms read positive selected rows. An
// absent grenade control contributes zero clips.
// [orig: UI_CalculateEquippedWeaponsWeight @0x565490 -- adm[85]/65536 +
//  (row+1)*ammoDef[84]/65536; PlayerInfo_CalculateLoadoutWeight @0x55f1f0 sibling;
//  the grenade selected_row*adm[84] arm @0x5655c9..0x56561c]
// NOT yet the retail gates (D-MNU-9): retail reads each AMMO1 / TYPE /
// GRENADE arm only while its control IsShown (@0x565504 / @0x56556d /
// @0x5655d6) and has no maxclips default for an unselected AMMO1 (no term at
// all); this side reads hidden controls too and lets armory_selected_clips'
// -1 fall to def_loadout_weight's maxclips term. Vacuous today (the armory
// never hides these controls), pinned as ours in loadout_screen_test.
double armory_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades) {
	std::vector<def::DefWeaponDef> rows;
	std::vector<int> counts;
	for (size_t i = 0; i < parents.size(); ++i) {
		const auto *w = weapon(weapons, parents[i]);
		if (!w) continue;
		rows.push_back(*w);
		counts.push_back(armory_selected_clips(menu, kParents[i]));
	}
	double total = def::def_loadout_weight(rows.data(), counts.data(), rows.size());
	for (size_t i = 0; i < grenades.size() && i < std::size(kGrenades); ++i) {
		const int combo = menu.widget_id(kGrenades[i]);
		const int clips = combo >= 0 ? menu.selected_row(combo) : 0;
		if (clips > 0) total += def::def_extra_ammo_weight(weapon(weapons, grenades[i]), clips);
	}
	return total;
}

} // namespace opennova::menu
