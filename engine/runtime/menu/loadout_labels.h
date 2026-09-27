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

// The WEAPON screen's slot-list row order: the indices of `labels` sorted
// case-insensitively ascending by display label; the caller prepends NONE at
// row 0 [orig: ListWidget_SortRows -> cmp @0x6448a0 with (string, asc); NONE
// inserted at 0 @0x566f15].
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
