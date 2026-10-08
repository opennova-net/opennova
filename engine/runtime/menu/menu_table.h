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
//
// Code sets a column up the way the XML's HEADER does: the column count first, then
// one init per column (MenuRuntime::table_set_column_count / table_init_column, or
// table_set_columns for both). Each column is a record the table holds:
// - `kept`: the record is the one the table had at this index (the authored
//   column, as the frame compiles it), which a count that does not grow the table
//   leaves in place; a count that grows it starts every record over, zeroed (no
//   label, width 0, justification 0, text cells, no SUBST rows, no cell offsets),
//   the old ones included [orig: CTableWnd_ResizeColumnCount @0x63f6c0, reached
//   through the table's vtable +0x6C (0x7e0940): the shrink path @0x63f870 keeps
//   the array and frees the dropped records; the grow path zeroes the new array
//   @0x63f710 and copies the old COUNT in bytes, not its 180-byte records,
//   @0x63f724, then frees the old @0x63f737].
// - `defined`: an init set the record up. It writes the label, the width, the
//   header justification (-1: 1 / 16) and the cell justification after it, the
//   sort compare, the direction (ascending) and zeroes +4..+104; the cell type
//   (+108), the cell offsets (+152 / +156), the SUBST rows (+164) and the bitmap
//   scale (+168 / +172) stay the record's [orig: CTableWnd_InitRow @0x63f9c0 —
//   the writes @0x63fa27..0x63fc10, none to +108 / +152..+172; the BODY sets
//   those @0x643811..0x643870, a SUBST row @0x643a71].
// The frame draws an undefined record as it is (the kept authored column, or a
// zeroed one, width 0); `cell_type` is the record's as far as the runtime knows
// it (a fresh record's 0) and orders its sort.
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
	bool kept = false;
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
