// The TABLE row records' CTableWnd operations (menu_table_row.h).

#include <runtime/menu/menu_table_row.h>

#include <algorithm>

namespace opennova::menu {

namespace {

const std::string kEmpty;

bool row_in_range(const std::vector<MenuTableRow> &rows, int row) {
	return row >= 0 && row < static_cast<int>(rows.size());
}

} // namespace

const std::string &MenuTableRow::cell(int column) const {
	const std::string *text = cell_text(column);
	return text != nullptr ? *text : kEmpty;
}

const std::string *MenuTableRow::cell_text(int column) const {
	if (column < 0 || column >= static_cast<int>(cells.size())) return nullptr;
	const std::optional<std::string> &text = cells[static_cast<size_t>(column)];
	return text.has_value() ? &*text : nullptr;
}

int32_t MenuTableRow::value(int column) const {
	return column >= 0 && column < static_cast<int>(values.size())
			? values[static_cast<size_t>(column)]
			: 0;
}

int table_insert_row(std::vector<MenuTableRow> &rows, const std::string &text0, int32_t value0,
		uint32_t flags, int insert_index) {
	// -1 appends; an index past the count clamps to it [orig: @0x641c3e,
	// @0x641cc4]. The row is zeroed, column 0's text copied in (every other
	// cell missing, D-MNU-43), the column-0 cell record set from the caller's
	// pair ({0, value}), the caller's word the flags, the state 0 [orig:
	// @0x641d38..0x641e0f, the first cell @0x641ee2, @0x642002..0x642082].
	// A column-0 SUBST row whose value matches stands its own text in for
	// the inserted one in the game (@0x641d3e); that swap is unported
	// (D-MNU-45).
	const int count = static_cast<int>(rows.size());
	int at = insert_index == -1 ? count : insert_index;
	if (at > count) at = count;
	if (at < 0) at = 0;
	MenuTableRow row;
	row.cells.push_back(text0);
	row.values.push_back(value0);
	row.flags = flags;
	rows.insert(rows.begin() + at, std::move(row));
	return at;
}

bool table_set_cell_text(std::vector<MenuTableRow> &rows, int row, int column, int column_count,
		const std::string &text) {
	if (!row_in_range(rows, row) || column < 0 || column >= column_count) return false;
	// The one cell it names; the cells between the row's last one and it stay
	// missing [orig: @0x63efb2] (D-MNU-43).
	MenuTableRow &r = rows[static_cast<size_t>(row)];
	if (static_cast<int>(r.cells.size()) <= column) r.cells.resize(static_cast<size_t>(column) + 1);
	r.cells[static_cast<size_t>(column)] = text;
	return true;
}

bool table_set_cell_value(std::vector<MenuTableRow> &rows, int row, int column, int column_count,
		int32_t value) {
	if (!row_in_range(rows, row) || column < 0 || column >= column_count) return false;
	MenuTableRow &r = rows[static_cast<size_t>(row)];
	if (static_cast<int>(r.values.size()) <= column) r.values.resize(static_cast<size_t>(column) + 1);
	r.values[static_cast<size_t>(column)] = value;
	return true;
}

int32_t table_cell_value(const std::vector<MenuTableRow> &rows, int row, int column) {
	if (!row_in_range(rows, row)) return 0;
	return rows[static_cast<size_t>(row)].value(column);
}

void table_remove_row(std::vector<MenuTableRow> &rows, int row) {
	if (row == -1) {
		rows.clear();
		return;
	}
	if (!row_in_range(rows, row)) return;
	rows.erase(rows.begin() + row);
}

bool table_set_row_selected(std::vector<MenuTableRow> &rows, int row, bool selected,
		bool multiselect) {
	const int32_t state = selected ? kTableRowSelected : kTableRowDefault;
	if (row < 0) {
		for (MenuTableRow &r : rows) r.state = state;
		return true;
	}
	if (!row_in_range(rows, row)) return false;
	if (!multiselect)
		for (MenuTableRow &r : rows)
			if (r.state != kTableRowLocked) r.state = kTableRowDefault;
	MenuTableRow &target = rows[static_cast<size_t>(row)];
	if (target.state != kTableRowLocked) target.state = state;
	return true;
}

bool table_click_select(std::vector<MenuTableRow> &rows, int row, bool multiselect) {
	if (!row_in_range(rows, row)) return false;
	MenuTableRow &target = rows[static_cast<size_t>(row)];
	if (target.state == kTableRowLocked) return false;
	if (!multiselect) {
		for (MenuTableRow &r : rows)
			if (r.state != kTableRowLocked) r.state = kTableRowDefault;
		target.state = kTableRowSelected;
		return true;
	}
	target.state = target.state == kTableRowSelected ? kTableRowDefault : kTableRowSelected;
	return true;
}

void table_set_row_color(std::vector<MenuTableRow> &rows, int row, bool enable, uint32_t color) {
	auto apply = [&](MenuTableRow &r) {
		if (enable) {
			r.color = color;
			r.flags |= kTableRowFlagColor;
		} else {
			r.flags &= ~kTableRowFlagColor;
		}
	};
	if (row < 0) {
		for (MenuTableRow &r : rows) apply(r);
		return;
	}
	if (row_in_range(rows, row)) apply(rows[static_cast<size_t>(row)]);
}

} // namespace opennova::menu
