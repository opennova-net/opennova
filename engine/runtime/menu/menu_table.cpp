// A TABLE's row sort [orig: CTableWnd_SortByColumn @ 0x640900 -> CTableWnd_CompareRows
// @ 0x63e9c0].

#include <runtime/menu/menu_table.h>

#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>
#include <runtime/menu/list_sort.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <numeric>

namespace opennova::menu {

namespace {

// [orig: CTableWnd_CompareRows @ 0x63e9c0 — strchr("0123456789", c) also finds the
// terminator, so an empty cell reads atol(""), 0]
int32_t numeric_cell(const std::string *cell) {
	const std::string &s = cell != nullptr ? *cell : std::string();
	const auto digit = [](char c) { return c == '\0' || std::isdigit(static_cast<unsigned char>(c)) != 0; };
	const char first = s.empty() ? '\0' : s[0];
	const char second = s.size() > 1 ? s[1] : '\0';
	if (digit(first) || (first == '-' && digit(second))) {
		return io::retail_atol(s.c_str()); // the CRT atol @0x63eb6c / @0x63ebd9 (D-NET-384)
	}
	return 0x7FFFFFFF;
}

// The text arm: stricmp over the two cells, a missing one read as
// g_EmptyStr, under the ".ACP" locale's fold, over each cell's cp1252 bytes
// (utf8_to_retail_text); the answer is stricmp's own, the bytes' distance
// [orig: CTableWnd_CompareRows @ 0x63e9c0 - the g_EmptyStr reads @0x63ead8 /
// @0x63eaeb, the stricmp @0x63eaf2 -> _stricmp @0x76fdf6 -> _stricmp_l
// @0x76fd23] (D-MNU-39).
int text_compare(const std::string *a, const std::string *b) {
	const std::string empty;
	return cp1252_stricmp(utf8_to_retail_text(a != nullptr ? *a : empty),
			utf8_to_retail_text(b != nullptr ? *b : empty));
}

} // namespace

void table_push_sort_key(std::vector<int> &keys, int column, int column_count) {
	const int depth = std::min(std::max(column_count, 1), 20);
	keys.resize(static_cast<size_t>(std::max(depth, 1)), -1);
	if (column != -1 && column != keys[0]) {
		for (int i = depth - 1; i > 0; --i) {
			keys[static_cast<size_t>(i)] = keys[static_cast<size_t>(i - 1)];
		}
		keys[0] = column;
	}
	const int end = std::min(column_count, 19);
	if (end >= 0 && end < static_cast<int>(keys.size())) {
		keys[static_cast<size_t>(end)] = -1;
	}
}

std::vector<int> table_sort_order(const std::vector<MenuTableRow> &rows,
		const std::vector<MenuTableColumn> &columns, const std::vector<int> &keys) {
	std::vector<int> order(rows.size());
	std::iota(order.begin(), order.end(), 0);
	const size_t depth = std::min<size_t>(columns.size(), 20);
	// The answer is the game's int: the direction times the difference, both
	// 32-bit [orig: imul @0x63eb02, @0x63ec1c].
	const auto times = [](int32_t direction, int32_t value) {
		return static_cast<int32_t>(
				static_cast<uint32_t>(direction) * static_cast<uint32_t>(value));
	};
	const auto compare = [&](int x, int y) -> int32_t {
		const std::vector<std::string> &a = rows[static_cast<size_t>(x)].cells;
		const std::vector<std::string> &b = rows[static_cast<size_t>(y)].cells;
		for (size_t i = 0; i < depth && i < keys.size(); ++i) {
			const int key = keys[i];
			if (key < 0 || key >= static_cast<int>(columns.size())) {
				return 0;
			}
			const MenuTableColumn &column = columns[static_cast<size_t>(key)];
			const int32_t direction = column.ascending ? 1 : -1;
			const size_t cell = static_cast<size_t>(key);
			const std::string *ca = cell < a.size() ? &a[cell] : nullptr;
			const std::string *cb = cell < b.size() ? &b[cell] : nullptr;
			// A text column (and a custom one whose first row has the cell):
			// two missing cells pass to the next key; one missing cell answers
			// the column's direction, whichever row lacks it, which leaves the
			// pair's order to the quicksort [orig: CTableWnd_CompareRows - the
			// custom gate @0x63ea5d..0x63ea70, both missing @0x63ea91, the
			// answers @0x63eaa1 / @0x63eab1 -> @0x63ec12..0x63ec1b].
			if (column.cell_type == 0 || (column.cell_type == 2 && ca != nullptr)) {
				if (ca == nullptr && cb == nullptr) {
					continue;
				}
				if (ca == nullptr || cb == nullptr) {
					return direction;
				}
			}
			if (column.numeric_sort) {
				const int32_t diff = static_cast<int32_t>(
						static_cast<uint32_t>(numeric_cell(ca)) - static_cast<uint32_t>(numeric_cell(cb)));
				if (diff != 0) {
					return times(direction, diff);
				}
			} else {
				const int c = text_compare(ca, cb);
				if (c != 0) {
					return times(direction, c);
				}
			}
		}
		return 0;
	};
	// The rows sorted in place through the list widgets' quicksort, so rows
	// equal on every key stand where its swaps leave them [orig:
	// CTableWnd_SortByColumn @0x640900 -> Utility_SortDualArray (the call
	// @0x640991) -> Utility_QuickSortDualArray @0x651AB0] (D-MNU-40).
	sort_dual_array(order, compare);
	return order;
}

} // namespace opennova::menu
