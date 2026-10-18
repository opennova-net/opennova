#include <runtime/inmatch/stat_screen_feed.h>

#include <base/io/cp1252.h>
#include <runtime/hud/end_round_overlay.h> // stat_field_string_index
#include <runtime/menu/menu_runtime.h>

#include <cstdio>
#include <cstdint>
#include <optional>
#include <vector>

namespace opennova::inmatch {

using hud::stat_field_string_index;

std::vector<StatScreenColumn> stat_screen_columns(const EndRoundStats &board,
		bool show_disabled, int table_width) {
	// [orig: StatScreen_PopulateStatResultsList @0x562240 — the column count
	//  @0x562290..0x5622b0, the width split @0x5622c7..0x5622e6, NAME @0x562318,
	//  "Squad" @0x56236b, the field headers @0x562399..0x56249e]
	std::vector<StatScreenColumn> out;
	int count = 2;
	for (const auto &f : board.team_fields)
		if (show_disabled || f.second != 0) ++count;
	const int column_width = count == 1 ? table_width - 150 : (table_width - 150) / (count - 1);
	{
		StatScreenColumn name;
		name.literal = "!Name";
		name.header_fallback = "!Name";
		name.width = 150;
		out.push_back(name);
		StatScreenColumn squad;
		squad.literal = "Squad";
		squad.header_fallback = "Squad";
		squad.width = column_width;
		out.push_back(squad);
	}
	for (size_t i = 0; i < board.team_fields.size(); ++i) {
		const int field_id = board.team_fields[i].first;
		if (!show_disabled && board.team_fields[i].second == 0) continue;
		StatScreenColumn col;
		col.field_id = field_id;
		col.field_index = static_cast<int>(i);
		col.width = column_width;
		const int str_index = stat_field_string_index(field_id);
		char key[64];
		char fallback[64];
		if (str_index != 0) {
			std::snprintf(key, sizeof(key),
					show_disabled ? "STROVER_STATFIELD%02d" : "STROVER_STATFIELDSMALL%02d",
					str_index);
			std::snprintf(fallback, sizeof(fallback), "!%s", key);
			col.header_key = key;
			col.header_fallback = fallback;
		} else {
			// [orig: "Unk entry %d" @0x5623f5]
			std::snprintf(fallback, sizeof(fallback), "Unk entry %d", field_id);
			col.header_key = fallback;
			col.header_fallback = fallback;
		}
		out.push_back(col);
	}
	return out;
}

std::vector<StatScreenRow> stat_screen_rows(const EndRoundStats &board,
		const std::vector<StatScreenPlayer> &players, bool show_disabled) {
	// [orig: @0x5624a3..0x5626fa]
	std::vector<StatScreenRow> out;
	for (const StatScreenPlayer &p : players) {
		if (p.team == 0) continue; // field_lookup_ptr[14] != 0
		const EndRoundPlayerRow *row = nullptr;
		for (const EndRoundPlayerRow &r : board.players)
			if (r.slot == p.slot) { row = &r; break; }
		if (row == nullptr) continue;
		StatScreenRow sr;
		sr.slot = p.slot;
		sr.team = p.team;
		sr.name = p.name;
		sr.squad = p.squad.empty() ? "-" : p.squad;
		// The row colour by the slot's team: 1 blue, 2 red, any other the table's
		// [orig: the team switch @0x56258a..0x562596, push 0FF00AFFFh @0x56259f,
		// push 0FFFF0000h @0x562598, then CTableWnd_SetRowColor @0x5625a9].
		if (p.team == 1) sr.color_argb = 0xFF00AFFFu;
		else if (p.team == 2) sr.color_argb = 0xFFFF0000u;
		for (size_t i = 0; i < board.team_fields.size(); ++i) {
			const int field_id = board.team_fields[i].first;
			if (!show_disabled && board.team_fields[i].second == 0) continue;
			const int value = i < row->per_team.size() ? row->per_team[i] : 0;
			char cell[32];
			switch (field_id) {
				case 5:
					std::snprintf(cell, sizeof(cell), "%2i:%02i", value / 60, value % 60);
					break;
				case 19:
					std::snprintf(cell, sizeof(cell), "%i", value);
					break;
				default:
					if ((field_id >= 1 && field_id <= 4) || (field_id >= 6 && field_id <= 18) ||
							(field_id >= 20 && field_id <= 32)) {
						if (value != -1) std::snprintf(cell, sizeof(cell), "%i", value);
						else std::snprintf(cell, sizeof(cell), "-");
					} else {
						std::snprintf(cell, sizeof(cell), "??");
					}
					break;
			}
			sr.cells.push_back(cell);
		}
		out.push_back(sr);
	}
	return out;
}

bool stat_screen_row_visible(int tab_index, uint8_t team) {
	// [orig: StatScreen_StatFilterTabHandler @0x562140]
	if (tab_index == 1) return team == 2;
	if (tab_index == 2) return team == 1;
	return true;
}

void fill_stat_results(menu::MenuRuntime &menu, int table,
		const std::vector<StatScreenColumn> &columns, const std::vector<StatScreenRow> &rows,
		int local_slot) {
	std::vector<menu::MenuTableColumn> set;
	set.reserve(columns.size());
	for (const StatScreenColumn &column : columns) {
		menu::MenuTableColumn c;
		c.label = utf8_to_retail_text(column.header);
		c.width = column.width;
		c.justify = kStatScreenColumnJustify;
		c.vjustify = kStatScreenColumnVJustify;
		c.body_justify = c.justify;
		c.body_vjustify = c.vjustify;
		// NAME and Squad compare as text; a stat field numerically (CTableWnd_InitRow's
		// sort argument 0 / 1 @ 0x562346 / 0x56237a / 0x56242b).
		c.numeric_sort = column.field_id != 0;
		set.push_back(std::move(c));
	}
	menu.table_clear_rows(table);
	menu.table_set_columns(table, set);
	for (const StatScreenRow &row : rows) {
		// The slot's name and squad tag as the roster's bytes, the row appended
		// with the slot's team as its column-0 value, the flags 0 [orig: the
		// CTableWnd_AddRow call @0x562565, the team pushed @0x562550..0x56255a];
		// the slot id as column 1's value [orig: the value call @0x562576, the
		// byte +0x0C @0x56256a; CTableWnd_SetCellValue @0x6420f0 (ex
		// CTableWnd_SetCellValue)] beside the squad [orig: @0x562585], then the
		// formatted fields [orig: CTableWnd_SetCellText @0x5626b1].
		const int index = menu.table_insert_row(table, row.name, row.team, 0, -1);
		menu.table_set_cell_value(table, index, 1, row.slot);
		menu.table_set_cell_text(table, index, 1, row.squad);
		// A team row takes its colour; any other keeps the table's (the feed's
		// white) [orig: CTableWnd_SetRowColor @0x5625a9].
		if (row.color_argb != 0xFFFFFFFFu)
			menu.table_set_row_color(table, index, true, row.color_argb);
		for (size_t i = 0; i < row.cells.size(); ++i)
			menu.table_set_cell_text(table, index, static_cast<int>(i) + 2, row.cells[i]);
	}
	// The sort: the first stat column, descending [orig: sub_63EC30(table, 2, 0)
	// then CTableWnd_SortByColumn(table, 2) @0x5626e6..0x5626f9].
	menu.table_set_column_ascending(table, kStatScreenSortColumn, kStatScreenSortAscending);
	menu.table_sort_by_column(table, kStatScreenSortColumn);
	// After it, the first row whose column-1 value (the slot) is the local
	// player entity's slot id (+0x154) is selected, through the table's
	// MULTISELECT rule; a row with no record is passed over, and no match
	// selects nothing [orig: the row walk @0x562700..0x562754 over
	// CTableWnd_GetCellRecord(row, 1) @0x562715, the no-record test
	// @0x56271a..0x56271c, the compare @0x56271e..0x56272e,
	// CTableWnd_SetRowSelected @0x562754] (D-MNU-51).
	const int count = menu.table_row_count(table);
	for (int r = 0; r < count; ++r) {
		const std::optional<int32_t> slot = menu.table_cell_record(table, r, 1);
		if (slot.has_value() && *slot == local_slot) {
			menu.table_set_row_selected(table, r, true);
			break;
		}
	}
}

} // namespace opennova::inmatch
