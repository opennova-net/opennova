#pragma once
// CPairList_ShellSortByValue: the one sort every retail pair list runs before it
// is read (the 0x0A entity priority lists, the round-event list, both
// scoreboards, the spawn-marker pick).

#include <cstddef>
#include <utility>
#include <vector>

namespace opennova::world {

// Descending by key over Knuth's gaps (h = 3h + 1 up to the first gap past
// count / 9, then h /= 3 down to 1). The insertion stops at an equal key (the
// signed `cmp; jge`), so the sort is NOT stable: which of two tied rows leads
// depends on the gaps and on the order the rows were added, so a caller adds
// them in retail's order first. `key_of(row)` is the pair's value.
// [orig: CPairList_ShellSortByValue @0x526CF0 - the gap build @0x526D11..0x526D17,
//  the insertion's `jge` @0x526D6B, gap /= 3 @0x526DC2]
template <class Row, class KeyOf>
void shell_sort_by_value(std::vector<Row> &rows, KeyOf key_of) {
	const std::size_t count = rows.size();
	std::size_t gap = 1;
	while (gap <= count / 9)
		gap = 3 * gap + 1;
	for (; gap > 0; gap /= 3) {
		for (std::size_t i = gap; i < count; ++i) {
			Row insert = std::move(rows[i]);
			const auto key = key_of(insert);
			std::size_t j = i;
			while (j >= gap && key_of(rows[j - gap]) < key) {
				rows[j] = std::move(rows[j - gap]);
				j -= gap;
			}
			rows[j] = std::move(insert);
		}
	}
}

} // namespace opennova::world
