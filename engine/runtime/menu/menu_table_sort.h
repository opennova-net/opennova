#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace opennova::menu {

// TABLE ROW SORTING — the multi-column comparator retail's table widget sorts
// its rows with [orig: compare_table_rows @0x63E9C0], paired with the
// dual-array quicksort that keeps a companion array in step
// [orig: quicksort_dual_array @0x651AB0].
//
// This is what makes the post-round STAT screen's columns clickable: the table
// carries an ORDERED LIST of sort columns and walks them until one produces a
// difference, so a tie on kills falls through to the next column rather than
// settling arbitrarily.

// Retail walks at most twenty sort columns [orig: the >= 20 clamp @0x63EA0E].
inline constexpr int kMaxSortColumns = 20;

// A column can compare as TEXT or as a NUMBER [orig: the `[28] == 1` test
// @0x63EA84].
enum class TableSortKind { Text, Numeric };

// One entry in the sort order.
struct TableSortColumn {
	int column = 0;                        // index into the row's cells
	TableSortKind kind = TableSortKind::Text;
	// [orig: the column's [30] flag]. SET means ASCENDING — see below.
	bool ascending = true;
};

// The direction multiplier [orig: 2 * (flag != 0) - 1 @0x63EA36], applied as
// `direction * (a - b)` [orig: the `return outLine * num_diff` @0x63EAF6].
//
// WORK THE SIGNS THROUGH BEFORE RENAMING THIS. A SET flag gives +1, so
// `+1 * (a - b)` is negative when a < b — a sorts first, i.e. ASCENDING. A
// CLEAR flag gives -1 and reverses that. The flag therefore reads as
// "ascending", which is the opposite of what its position in a "sort
// direction" field suggests; getting it backwards silently inverts every
// column on the board.
inline int table_sort_direction(bool ascending) {
	return ascending ? 1 : -1;
}

// NUMERIC PARSING HAS A WITNESSED SENTINEL. A cell that does not begin with a
// digit — or with a minus followed by a digit — does NOT sort as zero: it
// parses as INT_MAX [orig: the 0x7FFFFFFF arms @0x63EAB2 / @0x63EAD8].
//
// That is deliberate and load-bearing: it sinks blank and non-numeric cells to
// the BOTTOM of an ascending numeric sort instead of clustering them at the
// top with the zeroes. Treating them as 0 puts empty rows above real scores.
inline constexpr int32_t kNonNumericSentinel = 0x7FFFFFFF;

inline bool table_cell_looks_numeric(const std::string &cell) {
	if (cell.empty()) return false;
	const char c0 = cell[0];
	if (c0 >= '0' && c0 <= '9') return true;
	// A lone '-' is not numeric; it needs a digit behind it.
	if (c0 == '-' && cell.size() > 1) {
		const char c1 = cell[1];
		return c1 >= '0' && c1 <= '9';
	}
	return false;
}

inline int32_t table_cell_numeric(const std::string &cell) {
	if (!table_cell_looks_numeric(cell)) return kNonNumericSentinel;
	// atol semantics: leading digits, stop at the first non-digit.
	return static_cast<int32_t>(std::strtol(cell.c_str(), nullptr, 10));
}

// Text comparison is CASE-INSENSITIVE [orig: the stricmp arm].
int table_cell_compare_text(const std::string &a, const std::string &b);

// Compare two rows across the sort order. Returns <0, 0 or >0.
//
// The walk STOPS at the first column that separates them, and a column that
// ties falls through to the next — which is why the order is a list rather
// than a single key. An empty order, or one exhausted without a difference,
// returns 0 and leaves the rows in their existing relative order.
int table_compare_rows(const std::vector<std::string> &row_a,
		const std::vector<std::string> &row_b,
		const std::vector<TableSortColumn> &order);

// Sort `rows` and keep `companion` in step, so a caller can carry per-row
// payload (the slot id, the entity handle) alongside the display cells
// [orig: quicksort_dual_array @0x651AB0 — it sorts a primary array and swaps
//  the auxiliary in lockstep].
//
// The companion is resized to match on mismatch rather than being indexed out
// of range.
void table_sort_rows(std::vector<std::vector<std::string>> &rows,
		std::vector<int> &companion,
		const std::vector<TableSortColumn> &order);

} // namespace opennova::menu
