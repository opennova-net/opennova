#pragma once

// A TABLE's columns and its row sort [orig: init_table_row @ 0x63f9c0 (a column's
// setup); CTableWnd_SortByColumn @ 0x640900 -> CTableWnd_CompareRows @ 0x63e9c0 (the
// sort)]. The frame compiler draws the columns; the runtime keeps the rows and
// sorts them, as menu_table_row.h's records (a row's colour override and selection
// travel with it). Witness record: docs/mnu/menu-re.md ("Table render").

#include <runtime/menu/menu_table_row.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

// One table column as init_table_row sets it up [orig: init_table_row @ 0x63f9c0,
// the 180-byte column at table+780]: the header label, the width (+124; 0 is not
// drawn), the header justification (+128: 0 left, 1 centre, 2 right) and vertical
// justification (+132: 0 top, 16 centre, 32 bottom), the cell justification a BODY
// sets (+144 / +148, the header's until a BODY sets them), the cell type (+108: 0
// text, 1 image, 2 custom drawn, 4 image else text), the sort compare (+112:
// numeric when set) and direction (+120: ascending when set). The XML HEADER /
// BODY rows set them up; code installs its own (the stat RESULTLIST [orig:
// StatScreen_PopulateStatResultsList @ 0x562240]).
// A populate may also set the count first and init the columns one by one
// (MenuRuntime::table_set_column_count / table_init_column): an entry no init
// has defined yet keeps the column the resize left there (the authored one, or
// a zeroed new one) [orig: resize_column_count @0x63f6c0 — new records zeroed,
// existing ones copied; CTableWnd_InitRow @0x63f9c0 — -1 justify / vjustify
// take 1 / 16].
struct MenuTableColumn {
	std::string label;
	int width = 0;
	int justify = 1;
	int vjustify = 16;
	int body_justify = 1;
	int body_vjustify = 16;
	int cell_type = 0;
	bool numeric_sort = false;
	bool ascending = true;
	bool defined = true;
};

// The sort-key stack after a sort on `column` [orig: CTableWnd_SortByColumn
// @ 0x640900 — a column other than the first key pushes onto the front (the
// stack is min(columns, 20) deep), then the entry at min(columns, 19) ends it].
void table_push_sort_key(std::vector<int> &keys, int column, int column_count);

// The rows' order after a sort over `keys` [orig: CTableWnd_CompareRows @ 0x63e9c0,
// key by key until one is -1: the column's direction (+1 ascending, -1
// descending); a numeric column compares the atol of a cell that starts with a
// digit (or '-' and a digit) and 0x7FFFFFFF for any other; else the cells compare
// by stricmp; a cell a row does not have sorts as described there]. Returns the
// permutation (new row r is old row order[r]); a stable sort keeps rows that
// compare equal in order (retail's Utility_QuickSortDualArray is not ported).
std::vector<int> table_sort_order(const std::vector<MenuTableRow> &rows,
		const std::vector<MenuTableColumn> &columns, const std::vector<int> &keys);

} // namespace opennova::menu
