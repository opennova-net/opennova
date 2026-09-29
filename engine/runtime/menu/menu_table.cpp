// A TABLE's row sort [orig: CTableWnd_SortByColumn @ 0x640900 -> CTableWnd_CompareRows
// @ 0x63e9c0].

#include <runtime/menu/menu_table.h>

#include <base/io/strutil.h>

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
		return static_cast<int32_t>(std::strtol(s.c_str(), nullptr, 10));
	}
	return 0x7FFFFFFF;
}

int text_compare(const std::string *a, const std::string *b) {
	const std::string empty;
	const std::string la = strutil::to_lower(a != nullptr ? *a : empty);
	const std::string lb = strutil::to_lower(b != nullptr ? *b : empty);
	return la < lb ? -1 : la > lb ? 1 : 0;
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

std::vector<int> table_sort_order(const std::vector<std::vector<std::string>> &rows,
		const std::vector<MenuTableColumn> &columns, const std::vector<int> &keys) {
	std::vector<int> order(rows.size());
	std::iota(order.begin(), order.end(), 0);
	const size_t depth = std::min<size_t>(columns.size(), 20);
	const auto compare = [&](const std::vector<std::string> &a,
								 const std::vector<std::string> &b) -> int64_t {
		for (size_t i = 0; i < depth && i < keys.size(); ++i) {
			const int key = keys[i];
			if (key < 0 || key >= static_cast<int>(columns.size())) {
				return 0;
			}
			const MenuTableColumn &column = columns[static_cast<size_t>(key)];
			const int64_t direction = column.ascending ? 1 : -1;
			const size_t cell = static_cast<size_t>(key);
			const std::string *ca = cell < a.size() ? &a[cell] : nullptr;
			const std::string *cb = cell < b.size() ? &b[cell] : nullptr;
			// A text column (and a custom one whose first row has the cell):
			// two missing cells pass to the next key; one missing cell decides
			// [orig: CTableWnd_CompareRows answers the column's direction both ways
			// round, leaving the order to its quicksort; ordered here as the
			// missing cell first, so the comparison stays consistent].
			if (column.cell_type == 0 || (column.cell_type == 2 && ca != nullptr)) {
				if (ca != nullptr && cb == nullptr) {
					return direction;
				}
				if (ca == nullptr) {
					if (cb == nullptr) {
						continue;
					}
					return -direction;
				}
			}
			if (column.numeric_sort) {
				const int32_t diff = static_cast<int32_t>(
						static_cast<uint32_t>(numeric_cell(ca)) - static_cast<uint32_t>(numeric_cell(cb)));
				if (diff != 0) {
					return direction * diff;
				}
			} else {
				const int c = text_compare(ca, cb);
				if (c != 0) {
					return direction * c;
				}
			}
		}
		return 0;
	};
	std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
		return compare(rows[static_cast<size_t>(x)], rows[static_cast<size_t>(y)]) < 0;
	});
	return order;
}

} // namespace opennova::menu
