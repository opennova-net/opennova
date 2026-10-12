// The loadout screens' shared text compositions — see loadout_labels.h for
// the witnesses. [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430;
// UI_UpdateWeaponWeightDisplay @0x565640]

#include <runtime/menu/loadout_labels.h>

#include <base/io/strutil.h>
#include <formats/def/def.h>

#include <algorithm>
#include <cstdio>
#include <numeric>

using namespace opennova::def;

namespace opennova::menu {

std::string weapon_label(const DefWeaponDef &w, const hud::GameTextLookup &gametext) {
	if (w.loadout_menu_textid[0] == '\0') return w.weapon_name;
	return hud::game_text(gametext, hud::kGameTextWepDes, w.loadout_menu_textid, w.weapon_name);
}

std::string ammo_row_label(const DefWeaponDef *w, int clips, const hud::GameTextLookup &gametext) {
	char out[192];
	if (w == nullptr) {
		std::snprintf(out, sizeof(out), "%d - ", clips);
		return out;
	}
	const std::string round_label = w->round_type[0] != '\0'
			? hud::game_text(gametext, hud::kGameTextWepDes, w->round_type, w->round_type)
			: std::string();
	std::snprintf(out, sizeof(out), "%d - %s", clips * w->clipsize, round_label.c_str());
	return out;
}

namespace {

// The list compare in string mode, ascending: the sign of stricmp over the two
// rows' labels. The game's stricmp folds through the ".ACP" locale WinMain sets
// (setlocale having run, it takes _stricmp_l); ours folds ASCII (D-MNU-39).
// [orig: cmp @0x6448a0 -- string mode @0x6448db, the ascending stricmp
// @0x64495e..0x644968 -> _stricmp @0x76fdf6, its locale test @0x76fdfc ->
// _stricmp_l @0x76fd23]
int label_compare(const std::string &a, const std::string &b) {
	for (size_t i = 0;; ++i) {
		const int ca = i < a.size() ? static_cast<unsigned char>(strutil::ascii_tolower(a[i])) : 0;
		const int cb = i < b.size() ? static_cast<unsigned char>(strutil::ascii_tolower(b[i])) : 0;
		if (ca != cb || ca == 0) return ca - cb;
	}
}

// The rows' quicksort over lo..hi, `rows` carrying the label indices: it pivots
// on the low row and swaps from both ends, then recurses on the left part and
// loops on the right, so rows whose labels tie stand where its swaps leave them.
// [orig: Utility_QuickSortDualArray @0x651AB0]
void quick_sort_rows(std::vector<int> &rows, const std::vector<std::string> &labels, int lo,
		int hi) {
	const auto compare = [&rows, &labels](int a, int b) {
		return label_compare(labels[static_cast<size_t>(rows[static_cast<size_t>(a)])],
				labels[static_cast<size_t>(rows[static_cast<size_t>(b)])]);
	};
	const auto swap_rows = [&rows](int a, int b) {
		std::swap(rows[static_cast<size_t>(a)], rows[static_cast<size_t>(b)]);
	};
	for (;;) {
		const int saved_lo = lo; // [orig: @0x651ac5]
		const int saved_hi = hi; // [orig: @0x651ac9]
		int pivot = lo;          // [orig: @0x651acd]
		while (hi > lo) {        // [orig: @0x651ad1, @0x651c98]
			// The pivot at lo: hi walks down while the pivot sorts before it.
			bool ended = false;
			while (compare(lo, hi) < 0) { // [orig: @0x651b00..0x651b09]
				if (--hi <= lo) {         // [orig: @0x651b0f..0x651b16]
					ended = true;
					break;
				}
			}
			if (ended || lo >= hi) break; // [orig: @0x651b1b]
			swap_rows(hi, lo);            // [orig: @0x651b33..0x651b56]
			++lo;
			pivot = hi;                   // [orig: @0x651baf..0x651bb4]
			if (hi <= lo) break;          // [orig: @0x651bb8]
			// The pivot at hi: lo walks up while the pivot sorts after it.
			while (compare(hi, lo) > 0) { // [orig: @0x651bdb..0x651be4]
				if (hi <= ++lo) {         // [orig: @0x651bea..0x651bef]
					ended = true;
					break;
				}
			}
			if (ended || lo >= hi) break; // [orig: @0x651bfb]
			swap_rows(hi, lo);            // [orig: @0x651c03..0x651c39]
			--hi;
			pivot = lo;                   // [orig: @0x651c8f..0x651c94]
		}
		if (saved_lo < pivot - 1) quick_sort_rows(rows, labels, saved_lo, pivot - 1); // [orig: @0x651cce]
		lo = pivot + 1;                   // [orig: @0x651cd6..0x651cde]
		hi = saved_hi;
		if (hi <= lo) return;             // [orig: @0x651ce1..0x651ce3]
	}
}

} // namespace

std::vector<int> armory_slot_order(const std::vector<std::string> &labels) {
	std::vector<int> order(labels.size());
	std::iota(order.begin(), order.end(), 0);
	// The rows in the fill's order, sorted whole before NONE goes in
	// [orig: ListWidget_SortRows @0x644990 -> Utility_SortDualArray @0x651D00 (rows
	// 0..count-1, @0x651d48) -> Utility_QuickSortDualArray @0x651AB0; cmp @0x6448a0].
	quick_sort_rows(order, labels, 0, static_cast<int>(order.size()) - 1);
	return order;
}

std::string loadout_weight_line(double total, const hud::GameTextLookup &menutxt,
		const hud::GameTextLookup &gameui) {
	// The menu-token fold: menutxt's Menu section, then gameui's, else the
	// fallback [orig: TextResource_GetStringWithFallback(resource, "Menu", key)
	// @0x562ee0 against the menu resource].
	const hud::GameTextLookup menu_text =
			[&menutxt, &gameui](const char *section, const char *key, const char *fallback) {
				return hud::game_text(menutxt, section, key,
						hud::game_text(gameui, section, key, fallback).c_str());
			};
	std::string encumbrance;
	switch (def_encumbrance_class(total)) {
		case DEF_ENCUMBRANCE_HEAVY:
			encumbrance = menu_text("Menu", "HEAVY_ENCUMBRANCE", "Heavy");
			break;
		case DEF_ENCUMBRANCE_NORMAL:
			encumbrance = menu_text("Menu", "NORMAL_ENCUMBRANCE", "Normal");
			break;
		default:
			encumbrance = menu_text("Menu", "LIGHT_ENCUMBRANCE", "Light");
			break;
	}
	const std::string total_weight = menu_text("Menu", "TOTAL_WEIGHT", "Total Weight");
	const std::string lbs = menu_text("Menu", "LBS", "lbs");
	char out[256];
	std::snprintf(out, sizeof(out), "%s %.1f %s (%s)", total_weight.c_str(), total, lbs.c_str(),
			encumbrance.c_str());
	return out;
}

} // namespace opennova::menu
