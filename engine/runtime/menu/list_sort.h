#pragma once

// The list widgets' row sort: the quicksort the table sort and the list sort
// both run, and the list sort's row compare in string mode. Witness record:
// docs/mnu/menu-re.md (D-MNU-38, D-MNU-39, D-MNU-40).
// [orig: Utility_SortDualArray @0x651D00 -> Utility_QuickSortDualArray
//  @0x651AB0; its callers CTableWnd_SortByColumn @0x640900 (40-byte rows, the
//  call @0x640991, CTableWnd_CompareRows @0x63e9c0) and ListWidget_SortRows
//  @0x644990 (32-byte rows, the call @0x6449c0, cmp @0x6448a0)]

#include <base/io/cp1252.h>

#include <string>
#include <utility>
#include <vector>

namespace opennova::menu {

// The quicksort over items lo..hi with a three-way compare (negative, zero or
// positive, as the game's row compares answer): it pivots on the low item and
// swaps from both ends, then recurses on the left part and loops on the
// right, so items that compare equal stand where its swaps leave them.
// [orig: Utility_QuickSortDualArray @0x651AB0]
template <typename T, typename Compare>
void quick_sort_dual_array(std::vector<T> &items, int lo, int hi, const Compare &compare) {
	const auto at = [&items](int i) -> T & { return items[static_cast<size_t>(i)]; };
	for (;;) {
		const int saved_lo = lo; // [orig: @0x651ac5]
		const int saved_hi = hi; // [orig: @0x651ac9]
		int pivot = lo; // [orig: @0x651acd]
		while (hi > lo) { // [orig: @0x651ad1, @0x651c98]
			// The pivot at lo: hi walks down while the pivot sorts before it.
			bool ended = false;
			while (compare(at(lo), at(hi)) < 0) { // [orig: @0x651b00..0x651b09]
				if (--hi <= lo) { // [orig: @0x651b0f..0x651b16]
					ended = true;
					break;
				}
			}
			if (ended || lo >= hi)
				break; // [orig: @0x651b1b]
			std::swap(at(hi), at(lo)); // [orig: @0x651b33..0x651b56]
			++lo;
			pivot = hi; // [orig: @0x651baf..0x651bb4]
			if (hi <= lo)
				break; // [orig: @0x651bb8]
			// The pivot at hi: lo walks up while the pivot sorts after it.
			while (compare(at(hi), at(lo)) > 0) { // [orig: @0x651bdb..0x651be4]
				if (hi <= ++lo) { // [orig: @0x651bea..0x651bef]
					ended = true;
					break;
				}
			}
			if (ended || lo >= hi)
				break; // [orig: @0x651bfb]
			std::swap(at(hi), at(lo)); // [orig: @0x651c03..0x651c39]
			--hi;
			pivot = lo; // [orig: @0x651c8f..0x651c94]
		}
		if (saved_lo < pivot - 1)
			quick_sort_dual_array(items, saved_lo, pivot - 1, compare); // [orig: @0x651cce]
		lo = pivot + 1; // [orig: @0x651cd6..0x651cde]
		hi = saved_hi;
		if (hi <= lo)
			return; // [orig: @0x651ce1..0x651ce3]
	}
}

// The sort's entry: every item, 0..count-1 [orig: Utility_SortDualArray @0x651D00
// - the call @0x651d48; its 64-byte size gate passes both callers' rows].
template <typename T, typename Compare>
void sort_dual_array(std::vector<T> &items, const Compare &compare) {
	quick_sort_dual_array(items, 0, static_cast<int>(items.size()) - 1, compare);
}

// The list sort's row compare in string mode, ascending: stricmp over the
// two rows' texts. A row with no text sorts last (the null tests
// @0x6448a6..0x6448c8, both null 0 @0x6448b6); our rows always carry one.
// Mode 1 (strtol over both texts, the end pointers read @0x64490d /
// @0x644915) is no caller's here. The game's stricmp folds through the
// ".ACP" locale WinMain sets (setlocale having run, it takes _stricmp_l),
// over the texts' bytes in that page: ours runs it over each text's cp1252
// bytes (utf8_to_retail_text), so "é" and "É" tie and "Œ" (0x8C, lowered
// 0x9C) sorts before "é" (0xE9) (D-MNU-39). [orig: cmp @0x6448a0 -- string
// mode @0x6448db, the ascending stricmp @0x64495e..0x644968 -> _stricmp
// @0x76fdf6, its locale test @0x76fdfc -> _stricmp_l @0x76fd23]
inline int list_text_compare(const std::string &a, const std::string &b) {
	return cp1252_stricmp(utf8_to_retail_text(a), utf8_to_retail_text(b));
}

} // namespace opennova::menu
