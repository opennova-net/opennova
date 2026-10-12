#pragma once

// The loadout screens' shared text compositions (ADR 0040 ladder E4b): the
// weapon row label, the ammo-count row label, the armory's row order and the
// weight readout the in-game armory and the PLAYER_INFO loadout panel both
// render. The strings are the game's own (gametext WepDes, the menu tokens);
// these choose the key, the fallback and the format.

#include <runtime/hud/game_text_lookup.h>

#include <string>
#include <vector>

namespace opennova::def {
struct DefWeaponDef;
}

namespace opennova::menu {

// Weapon display name = loadout_menu_textid resolved in gametext's WepDes
// section, else the raw weapon id
// [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430: entry+40 textid else entry+0].
std::string weapon_label(const def::DefWeaponDef &w, const hud::GameTextLookup &gametext);

// "<rounds> - <round label>" for `clips` clips of `w`; a null (unavailable)
// def rows "<clips> - " [orig: sprintf "%d - %s" with i*clipsize + round_type
// in every ammo fill, @0x564c7d..0x564ce4 on the WEAPON screen; the label
// resolves through gametext WepDes].
std::string ammo_row_label(const def::DefWeaponDef *w, int clips,
		const hud::GameTextLookup &gametext);

// The loadout screens' slot-list row order, the WEAPON screen's and the PLAYER
// screen's: the indices of `labels`, given in the fill's table order, as the
// list sort leaves them, case-insensitively ascending by display label with
// tied labels where retail's quicksort puts them; the caller prepends NONE at
// row 0 [orig: ListWidget_SortRows(list, 0, 1) -> cmp @0x6448a0, the quicksort
// Utility_QuickSortDualArray @0x651AB0; the WEAPON screen's
// UI_PopulateThreeCategoryLists @0x566ed5 / @0x566ee0 / @0x566eef, NONE inserted
// at 0 @0x566f15; PlayerInfo_PopulateWeaponSlotLists @0x560430 -- the sorts
// @0x56054c / @0x560557 / @0x560566, the NONE inserts @0x56058f / @0x56059d /
// @0x5605ab]. The case fold is ASCII where the game's is the ".ACP" locale's (D-MNU-39).
std::vector<int> armory_slot_order(const std::vector<std::string> &labels);

// The weight readout "<TOTAL_WEIGHT> <w> <LBS> (<encumbrance>)": the band from
// def_encumbrance_class (<33.3 LIGHT / <66.6 NORMAL / else HEAVY) names the
// LIGHT_/NORMAL_/HEAVY_ENCUMBRANCE menu token; both lookups use the Menu
// section with its fallback [orig: UI_UpdateWeaponWeightDisplay @0x565640 and
// PlayerInfo_UpdateWeightAndWeaponIcons @0x55f480 — sprintf
// "%s %.1f %s (%s)", keys TOTAL_WEIGHT / LBS / *_ENCUMBRANCE].
// Menu tokens prefer menutxt, then gameui, then the built-in fallback.
std::string loadout_weight_line(double total, const hud::GameTextLookup &menutxt,
		const hud::GameTextLookup &gameui);

} // namespace opennova::menu
