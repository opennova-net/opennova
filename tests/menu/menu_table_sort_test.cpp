// Table row sorting: the multi-column walk, the numeric sentinel, the
// direction polarity and the companion array.
// [orig: compare_table_rows @0x63E9C0; quicksort_dual_array @0x651AB0]

#include <menu/menu_table_sort.h>

#include <cstdio>

using namespace opennova::menu;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

using Row = std::vector<std::string>;

// THE DIRECTION POLARITY IS COUNTERINTUITIVE: the SET flag yields +1 and reads
// as ASCENDING, because the result is direction * (a - b). Getting it backwards
// silently inverts every column on the board.
void test_direction_polarity() {
	// A SET flag is ASCENDING (+1), because the result is direction * (a - b):
	// +1 * (a - b) is negative when a < b, so a sorts first.
	CHECK(table_sort_direction(true) == 1, "the set flag is +1 = ascending");
	CHECK(table_sort_direction(false) == -1, "clear is -1 = descending");
}

// A NON-NUMERIC CELL PARSES AS INT_MAX, NOT ZERO. That sinks blank rows to the
// bottom of an ascending numeric sort instead of clustering them with the
// zeroes at the top.
void test_numeric_sentinel() {
	CHECK(table_cell_numeric("42") == 42, "a plain number parses");
	CHECK(table_cell_numeric("-7") == -7, "a negative parses");
	CHECK(table_cell_numeric("0") == 0, "zero is zero, not the sentinel");
	CHECK(table_cell_numeric("") == kNonNumericSentinel, "empty is the sentinel");
	CHECK(table_cell_numeric("abc") == kNonNumericSentinel, "text is the sentinel");
	CHECK(table_cell_numeric("-") == kNonNumericSentinel,
			"a lone minus is NOT numeric");
	CHECK(table_cell_numeric("-x") == kNonNumericSentinel,
			"a minus needs a digit behind it");
	// The distinction that matters: "" must NOT sort as 0.
	CHECK(table_cell_numeric("") != 0,
			"a blank cell must not sort as zero");
	CHECK(table_cell_numeric("") > table_cell_numeric("999999"),
			"and it sinks below every real value");
}

// Text comparison is case-insensitive.
void test_text_compare() {
	CHECK(table_cell_compare_text("abc", "ABC") == 0, "case is ignored");
	CHECK(table_cell_compare_text("abc", "abd") < 0, "orders by content");
	CHECK(table_cell_compare_text("abc", "ab") > 0, "a longer prefix sorts after");
	CHECK(table_cell_compare_text("", "a") < 0, "empty sorts first");
	CHECK(table_cell_compare_text("", "") == 0, "two empties tie");
}

// A TIE FALLS THROUGH TO THE NEXT COLUMN — which is why the order is a LIST
// and not a single key. A single-key sort settles ties arbitrarily.
void test_tie_falls_through() {
	const Row a = {"Ace", "10", "3"};
	const Row b = {"Bee", "10", "5"};
	// Sort by column 1 (equal), then column 2.
	std::vector<TableSortColumn> order = {
		{1, TableSortKind::Numeric, true},
		{2, TableSortKind::Numeric, true},
	};
	CHECK(table_compare_rows(a, b, order) != 0,
			"a tie on the first column is broken by the second");

	// With ONLY the tied column, they compare equal.
	std::vector<TableSortColumn> single = {{1, TableSortKind::Numeric, true}};
	CHECK(table_compare_rows(a, b, single) == 0,
			"and with only that column they tie");

	// An empty order never separates anything.
	CHECK(table_compare_rows(a, b, {}) == 0, "no order, no difference");
}

// A short row does not read out of range — a missing cell is the empty string.
void test_missing_cells() {
	const Row full = {"Ace", "10"};
	const Row short_row = {"Bee"};
	std::vector<TableSortColumn> order = {{1, TableSortKind::Text, true}};
	// Should not crash, and the missing cell behaves as empty.
	const int c = table_compare_rows(full, short_row, order);
	CHECK(c != 0, "a present cell beats a missing one");
	CHECK(table_compare_rows(short_row, short_row, order) == 0,
			"two missing cells tie");
}

// THE COMPANION ARRAY RIDES ALONG, so a caller can carry the slot id next to
// the display cells and still know which row is whose after the sort.
void test_companion_stays_in_step() {
	std::vector<Row> rows = {{"c", "3"}, {"a", "1"}, {"b", "2"}};
	std::vector<int> companion = {30, 10, 20};
	std::vector<TableSortColumn> order = {{0, TableSortKind::Text, true}};
	table_sort_rows(rows, companion, order);

	CHECK(rows.size() == 3 && companion.size() == 3, "sizes preserved");
	CHECK(rows[0][0] == "a" && rows[1][0] == "b" && rows[2][0] == "c",
			"rows sorted ascending by text");
	CHECK(companion[0] == 10 && companion[1] == 20 && companion[2] == 30,
			"and the companion followed its row");

	// A short companion is sized rather than indexed out of range.
	std::vector<Row> rows2 = {{"b"}, {"a"}};
	std::vector<int> tiny;
	table_sort_rows(rows2, tiny, order);
	CHECK(tiny.size() == 2, "a short companion is resized, not overrun");
}

// Sorting is STABLE, so rows tying on every column keep their input order.
void test_stability() {
	std::vector<Row> rows = {{"x", "1"}, {"y", "1"}, {"z", "1"}};
	std::vector<int> companion = {1, 2, 3};
	std::vector<TableSortColumn> order = {{1, TableSortKind::Numeric, true}};
	table_sort_rows(rows, companion, order);
	CHECK(companion[0] == 1 && companion[1] == 2 && companion[2] == 3,
			"rows that tie everywhere keep their original order");
}

// Descending reverses the result.
void test_descending() {
	std::vector<Row> rows = {{"1"}, {"3"}, {"2"}};
	std::vector<int> comp = {1, 3, 2};
	table_sort_rows(rows, comp, {{0, TableSortKind::Numeric, true}});
	CHECK(rows[0][0] == "1" && rows[2][0] == "3", "ascending");

	std::vector<Row> rows2 = {{"1"}, {"3"}, {"2"}};
	std::vector<int> comp2 = {1, 3, 2};
	table_sort_rows(rows2, comp2, {{0, TableSortKind::Numeric, false}});
	CHECK(rows2[0][0] == "3" && rows2[2][0] == "1", "descending reverses it");
}

} // namespace

int main() {
	test_direction_polarity();
	test_numeric_sentinel();
	test_text_compare();
	test_tie_falls_through();
	test_missing_cells();
	test_companion_stays_in_step();
	test_stability();
	test_descending();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_table_sort_test OK\n");
	return 0;
}
