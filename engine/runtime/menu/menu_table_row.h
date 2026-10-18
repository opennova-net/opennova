#pragma once

// The TABLE widget's row records and the row operations every table caller
// goes through: the 40-byte row of CTableWnd (a text per column, the 8-byte
// per-column cell record whose value the companions read back, the row state,
// the row flags and the override colour) and the CTableWnd routines that
// write them. Both the menu runtime's store and the compiled frame hold rows
// in this shape; the image a SUBST FILE row puts beside a matching text is
// resolved at draw time from the column's SUBST list.
// [orig: CTableWnd_InsertRow @0x641c30 (the row layout: +0 texts, +4 images,
//  +8 cell records, +24 state, +28 flags, +32 colour); docs/mnu/menu-re.md
//  "Table render"]

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::menu {

// The row state (+24): 0 default, 1 locked (immune to every selection
// write but the all-rows one), 3 selected. The payload of the table's
// custom-draw and click events carries it.
// [orig: CTableWnd_SetRowSelected @0x63f5f0; CTableWnd_IsRowSelected (== 3)]
inline constexpr int32_t kTableRowDefault = 0;
inline constexpr int32_t kTableRowLocked = 1;
inline constexpr int32_t kTableRowSelected = 3;

// The row flags (+28, CTableWnd_InsertRow's caller word): bit 0 draws the
// row's text with state 1's colour, bits 1|3 skip the row (draw, hit test and
// the visible count), bit 2 swaps the row colour (+32) into the row state's
// colour slot around the row's draw.
// [orig: CUITable_Render @0x64159d (`& 0xA`), @0x6416c9 (`& 4`); the
//  CTableWnd_DrawCell state pick @0x641048 (`& 1`); CTableWnd_SetRowColor @0x640110]
inline constexpr uint32_t kTableRowFlagState1Color = 0x1u;
inline constexpr uint32_t kTableRowFlagColor = 0x4u;
inline constexpr uint32_t kTableRowHiddenMask = 0xAu;

// The texts (+0) are the row's cell pointers: the insert zeroes them and sets
// the first, a set writes the one it names, so a cell no write reached is
// missing (NULL), as is one past the last set cell; the sort answers a
// missing cell, the draw shows nothing for it: no text (`cell` reads it as
// empty) and no image (MenuFrameCompiler::table_cell_image_).
// [orig: CTableWnd_InsertRow @0x641c30 - the zeroing memset @0x641dd5, the
//  first cell @0x641e4e..0x641ee2; CTableWnd_SetCellText @0x63edf0 - the
//  cell's text @0x63efb2, NULL for none @0x63ef81; CUITable_Render
//  @0x6411d0 - a text cell drawn only when set @0x6417c5] (D-MNU-43).
struct MenuTableRow {
	std::vector<std::optional<std::string>> cells;
	std::vector<int32_t> values;
	int32_t state = kTableRowDefault;
	uint32_t flags = 0;
	uint32_t color = 0;

	bool hidden() const { return (flags & kTableRowHiddenMask) != 0; }
	// The column's text, empty when the cell is missing.
	const std::string &cell(int column) const;
	// The column's text, nullptr when the cell is missing.
	const std::string *cell_text(int column) const;
	int32_t value(int column) const;
};

// CTableWnd_AddRow(text, value, flags, insert) -> CTableWnd_InsertRow: a
// zeroed row at `insert_index` (-1 or past the end appends) whose column 0
// text is `text0` and whose column-0 cell record holds `value0`; the index
// the row landed at.
// [orig: CTableWnd_AddRow @0x642750; CTableWnd_InsertRow @0x641c30]
int table_insert_row(std::vector<MenuTableRow> &rows, const std::string &text0, int32_t value0,
		uint32_t flags, int insert_index);

// CTableWnd_SetCellText: false when the row or the column is out of range
// (`column_count` is the table's COLUMN COUNT).
// [orig: CTableWnd_SetCellText @0x63edf0 — the bounds @0x63ee0b..0x63ee1d]
bool table_set_cell_text(std::vector<MenuTableRow> &rows, int row, int column, int column_count,
		const std::string &text);

// The per-column cell record's value (the pair's second dword): the write
// [orig: CTableWnd_SetCellValue @0x6420f0 (ex
// CTableWnd_SetCellValue)], and the column-0 rewrite [orig: sub_63F440 @0x63f440].
bool table_set_cell_value(std::vector<MenuTableRow> &rows, int row, int column, int column_count,
		int32_t value);
// The cell record's value (+4), none when the row or the column is out of range
// (`column_count` is the table's COLUMN COUNT, +796): a record a smaller count
// left past it is no record until the count grows back (D-MNU-55).
// [orig: CTableWnd_GetCellRecord @0x63f400 + 4 - the row bound @0x63f408, the
//  column bound @0x63f418, none @0x63f433; CTableWnd_GetRowValue @0x63f3d0 = column 0]
std::optional<int32_t> table_cell_record(
		const std::vector<MenuTableRow> &rows, int row, int column, int column_count);

// CTableWnd_RemoveRow: -1 clears the table, a row index removes that row.
// [orig: CTableWnd_RemoveRow @0x641a40]
void table_remove_row(std::vector<MenuTableRow> &rows, int row);

// CTableWnd_SetRowSelected: a row index — without MULTISELECT every
// non-locked row first drops to 0 — then that row (unless locked) to 3 or 0;
// -1 sets EVERY row, locked ones included.
// [orig: CTableWnd_SetRowSelected @0x63f5f0]
bool table_set_row_selected(std::vector<MenuTableRow> &rows, int row, bool selected,
		bool multiselect);

// The table's own click on a data row: a locked row takes nothing; without
// MULTISELECT the non-locked rows drop to 0 and the row goes to 3; with it the
// row toggles 3 <-> 0. False when nothing was hit.
// [orig: CTableWnd_HandleNamedEvent @0x642400 — @0x642550..0x64259d]
bool table_click_select(std::vector<MenuTableRow> &rows, int row, bool multiselect);

// The row colour override: enable stores the colour and sets flag bit 2,
// disable clears the bit; -1 applies to every row.
// [orig: CTableWnd_SetRowColor @0x640110]
void table_set_row_color(std::vector<MenuTableRow> &rows, int row, bool enable, uint32_t color);

} // namespace opennova::menu
