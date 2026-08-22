#include <menu/menu_table_sort.h>

#include <algorithm>
#include <cstdlib>

#include <io/strutil.h>

namespace opennova::menu {

int table_cell_compare_text(const std::string &a, const std::string &b) {
	// Case-insensitive, matching retail's stricmp arm. A missing cell compares
	// as the empty string rather than being skipped [orig: the g_empty_str
	// substitution @0x63EB1E].
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i) {
		const unsigned char ca =
				static_cast<unsigned char>(opennova::strutil::ascii_tolower(a[i]));
		const unsigned char cb =
				static_cast<unsigned char>(opennova::strutil::ascii_tolower(b[i]));
		if (ca != cb) return ca < cb ? -1 : 1;
	}
	if (a.size() == b.size()) return 0;
	return a.size() < b.size() ? -1 : 1;
}

namespace {

const std::string &cell_at(const std::vector<std::string> &row, int index) {
	static const std::string kEmpty;
	if (index < 0 || static_cast<size_t>(index) >= row.size()) return kEmpty;
	return row[static_cast<size_t>(index)];
}

} // namespace

int table_compare_rows(const std::vector<std::string> &row_a,
		const std::vector<std::string> &row_b,
		const std::vector<TableSortColumn> &order) {
	// Retail clamps the walk at twenty columns [orig: @0x63EA0E].
	const size_t limit = order.size() < static_cast<size_t>(kMaxSortColumns)
			? order.size()
			: static_cast<size_t>(kMaxSortColumns);
	for (size_t i = 0; i < limit; ++i) {
		const TableSortColumn &col = order[i];
		const int dir = table_sort_direction(col.ascending);
		const std::string &a = cell_at(row_a, col.column);
		const std::string &b = cell_at(row_b, col.column);

		if (col.kind == TableSortKind::Numeric) {
			// Non-numeric cells parse as INT_MAX, which sinks them rather than
			// clustering them with the zeroes.
			const int32_t na = table_cell_numeric(a);
			const int32_t nb = table_cell_numeric(b);
			// The difference is taken in 64-bit: two sentinels differ by 0, but
			// a sentinel against a negative score overflows a 32-bit subtract.
			const int64_t diff = static_cast<int64_t>(na) - static_cast<int64_t>(nb);
			if (diff != 0) return diff < 0 ? -dir : dir;  // dir * sign(a - b)
		} else {
			const int c = table_cell_compare_text(a, b);
			if (c != 0) return c < 0 ? -dir : dir;
		}
		// A tie falls through to the next column — that is why the order is a
		// list and not a single key.
	}
	return 0;
}

void table_sort_rows(std::vector<std::vector<std::string>> &rows,
		std::vector<int> &companion,
		const std::vector<TableSortColumn> &order) {
	if (rows.size() < 2 || order.empty()) return;
	// The companion rides along; size it rather than indexing out of range.
	companion.resize(rows.size(), 0);

	// Sort an index permutation, then apply it to both arrays — the same
	// lockstep the dual-array quicksort achieves by swapping both.
	std::vector<size_t> idx(rows.size());
	for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
	// STABLE, so rows that tie on every sort column keep their existing order
	// instead of being permuted arbitrarily.
	std::stable_sort(idx.begin(), idx.end(), [&](size_t l, size_t r) {
		return table_compare_rows(rows[l], rows[r], order) < 0;
	});

	std::vector<std::vector<std::string>> sorted_rows;
	std::vector<int> sorted_companion;
	sorted_rows.reserve(rows.size());
	sorted_companion.reserve(companion.size());
	for (size_t i : idx) {
		sorted_rows.push_back(std::move(rows[i]));
		sorted_companion.push_back(companion[i]);
	}
	rows.swap(sorted_rows);
	companion.swap(sorted_companion);
}

} // namespace opennova::menu
