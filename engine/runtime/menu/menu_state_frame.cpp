#include <runtime/menu/menu_state_frame.h>

#include <algorithm>
#include <tuple>

namespace opennova::menu {

MenuWidgetState &frame_widget(MenuFrameState &state, int index) {
	for (MenuWidgetState &row : state.widgets)
		if (row.index == index) return row;
	MenuWidgetState row;
	row.index = index;
	state.widgets.push_back(row);
	return state.widgets.back();
}

const MenuWidgetState *find_frame_widget(const MenuFrameState &state, int index) {
	for (const MenuWidgetState &row : state.widgets)
		if (row.index == index) return &row;
	return nullptr;
}

void frame_set_shown(MenuFrameState &state, int index, bool shown) {
	MenuWidgetState &row = frame_widget(state, index);
	row.hide = !shown;
	row.show = shown;
}

void frame_set_disabled(MenuFrameState &state, int index, bool disabled) {
	MenuWidgetState &row = frame_widget(state, index);
	row.has_disabled = true;
	row.disabled = disabled;
}

void frame_set_checked(MenuFrameState &state, int index, bool checked) {
	MenuWidgetState &row = frame_widget(state, index);
	row.has_checked = true;
	row.checked = checked;
}

void frame_set_focused(MenuFrameState &state, int index, bool focused) {
	frame_widget(state, index).focused = focused;
}

void frame_set_caret(MenuFrameState &state, int index, int caret) {
	frame_widget(state, index).caret = caret;
}

void frame_set_text(MenuFrameState &state, int index, const std::string &text) {
	MenuWidgetState &row = frame_widget(state, index);
	row.has_text = true;
	row.text = text;
}

void frame_set_rect(MenuFrameState &state, int index, int left, int top, int right, int bottom) {
	MenuWidgetState &row = frame_widget(state, index);
	row.has_rect = true;
	row.rect.left = left;
	row.rect.top = top;
	row.rect.right = right;
	row.rect.bottom = bottom;
}

bool frame_set_hover_item(MenuFrameState &state, int index, int row_index) {
	MenuWidgetState &row = frame_widget(state, index);
	if (row.hover_item == row_index) return false;
	row.hover_item = row_index;
	return true;
}

void frame_set_selection(MenuFrameState &state, int index, int selected, int hover, int scroll_row) {
	MenuWidgetState &row = frame_widget(state, index);
	row.selected_item = selected;
	row.hover_item = hover;
	row.scroll_row = scroll_row;
}

// [orig: CScrollWnd_SetRangeAndClamp @ 0x64d490 — the value clamped into the range; CScrollWnd_SetPageSize
//  @ 0x64ce10]
void frame_set_scroll_range(MenuFrameState &state, int index, int minimum, int maximum, int page, int value) {
	MenuWidgetState &row = frame_widget(state, index);
	if (minimum > maximum) {
		minimum = 0;
		maximum = 0;
	}
	row.has_scroll_range = true;
	row.scroll_min = minimum;
	row.scroll_max = maximum;
	row.scroll_page = page;
	row.scroll_value = std::clamp(value, minimum, maximum);
}

void frame_set_popup_open(MenuFrameState &state, int index, bool open) {
	frame_widget(state, index).popup_open = open;
}

void frame_set_items(MenuFrameState &state, int index, const std::vector<std::string> &items) {
	MenuWidgetState &row = frame_widget(state, index);
	row.has_items = true;
	row.items = items;
}

void frame_set_selected_set(MenuFrameState &state, int index, const std::vector<int> &rows) {
	MenuWidgetState &row = frame_widget(state, index);
	row.selected_items.assign(rows.begin(), rows.end());
}

void frame_set_table_rows(MenuFrameState &state, int index, const std::vector<MenuTableRow> &rows) {
	frame_widget(state, index).table_rows = rows;
}

void frame_set_clip_rect(MenuFrameState &state, int index, bool enabled, int left, int top, int right,
		int bottom) {
	MenuWidgetState &row = frame_widget(state, index);
	row.has_clip = enabled;
	row.clip = { left, top, right, bottom };
}

void frame_set_table_columns(MenuFrameState &state, int index, bool installed,
		const std::vector<MenuTableColumn> &columns, int sort_column) {
	MenuWidgetState &row = frame_widget(state, index);
	row.has_table_columns = installed;
	row.table_columns = columns;
	row.table_sort_column = sort_column;
}

std::string frame_widget_text(const MenuFrameCompiler &compiler, const MenuFrameState &state, int index) {
	const MenuWidgetState *row = find_frame_widget(state, index);
	return row && row->has_text ? row->text : compiler.widget_authored_text(index);
}

int frame_widget_caret(const MenuFrameState &state, int index) {
	const MenuWidgetState *row = find_frame_widget(state, index);
	return row ? row->caret : -1;
}

namespace {

// The field an edit's input works on: its text as the frame holds it, its caret (at the end when
// none is held), within the text.
EditField edit_field(const MenuFrameCompiler &compiler, const MenuWidgetState &row, int index) {
	EditField field;
	field.text = row.has_text ? row.text : compiler.widget_authored_text(index);
	field.caret = row.caret >= 0 ? row.caret : static_cast<int>(field.text.size());
	field.caret = std::min(field.caret, static_cast<int>(field.text.size()));
	return field;
}

void keep_field(MenuWidgetState &row, const EditField &field) {
	row.has_text = true;
	row.text = field.text;
	row.caret = field.caret;
}

} // namespace

// [orig: CEditWnd_HandleInputEvent @ 0x661510 — the printable gate before CEditWnd_InsertChar @ 0x661ee0; the
//  special keys CEditWnd_HandleKeyEvent @ 0x6623a0]
bool frame_edit_char(const MenuFrameCompiler &compiler, MenuFrameState &state, int index, int unicode) {
	// The router's printable filter and the ops both live in menu_edit.h (their witnesses there).
	if (!edit_char_insertable(unicode)) return false;
	EditLimits limits;
	if (!compiler.widget_edit_limits(index, &limits)) return false;
	MenuWidgetState &row = frame_widget(state, index);
	EditField field = edit_field(compiler, row, index);
	const bool changed = edit_insert_char(field, limits, static_cast<char>(unicode));
	keep_field(row, field);
	return changed;
}

EditKeyResult frame_edit_key(const MenuFrameCompiler &compiler, MenuFrameState &state, int index, int key,
		bool shift) {
	MenuWidgetState &row = frame_widget(state, index);
	EditField field = edit_field(compiler, row, index);
	const EditKeyResult result = edit_apply_key(field, key, 1, shift);
	keep_field(row, field);
	return result;
}

// --- MenuStateFrame ------------------------------------------------------------------------------

void MenuStateFrame::clear() {
	state_ = MenuFrameState();
	click_ = MenuClickLatch();
	configured_ = false;
	++serial_;
}

void MenuStateFrame::configure_screen(const std::string &screen) {
	// A configure starts the frame over, as the device's does (MenuFrame::configure_screen).
	state_ = MenuFrameState();
	click_ = MenuClickLatch();
	configured_ = configure_ && configure_(screen, compiler_);
	++configures_;
	++serial_;
}

void MenuStateFrame::set_widget_shown_override(int index, bool shown) {
	frame_set_shown(state_, index, shown);
	++serial_;
}
void MenuStateFrame::set_widget_disabled(int index, bool disabled) {
	frame_set_disabled(state_, index, disabled);
	++serial_;
}
void MenuStateFrame::set_widget_checked(int index, bool checked) {
	frame_set_checked(state_, index, checked);
	++serial_;
}
void MenuStateFrame::set_widget_text(int index, const std::string &text) {
	frame_set_text(state_, index, text);
	++serial_;
}
void MenuStateFrame::set_widget_items(int index, const std::vector<std::string> &items) {
	frame_set_items(state_, index, items);
	++serial_;
}
void MenuStateFrame::set_widget_selection(int index, int selected, int hover, int scroll_row) {
	frame_set_selection(state_, index, selected, hover, scroll_row);
	++serial_;
}
void MenuStateFrame::set_widget_scroll_range(int index, int minimum, int maximum, int page, int value) {
	frame_set_scroll_range(state_, index, minimum, maximum, page, value);
	++serial_;
}
void MenuStateFrame::set_widget_selected_set(int index, const std::vector<int> &rows) {
	frame_set_selected_set(state_, index, rows);
	++serial_;
}
void MenuStateFrame::set_widget_table_rows(int index, const std::vector<MenuTableRow> &rows) {
	frame_set_table_rows(state_, index, rows);
	++serial_;
}
void MenuStateFrame::set_widget_table_columns(int index, bool installed,
		const std::vector<MenuTableColumn> &columns, int sort_column) {
	frame_set_table_columns(state_, index, installed, columns, sort_column);
	++serial_;
}
void MenuStateFrame::set_widget_clip_rect(int index, bool enabled, int left, int top, int right, int bottom) {
	frame_set_clip_rect(state_, index, enabled, left, top, right, bottom);
	++serial_;
}
void MenuStateFrame::set_widget_hover_item(int index, int row) {
	if (frame_set_hover_item(state_, index, row)) ++serial_;
}
void MenuStateFrame::set_widget_popup_open(int index, bool open) {
	frame_set_popup_open(state_, index, open);
	++serial_;
}
void MenuStateFrame::set_widget_focused(int index, bool focused) {
	frame_set_focused(state_, index, focused);
	++serial_;
}
void MenuStateFrame::set_widget_rect(int index, int left, int top, int right, int bottom) {
	frame_set_rect(state_, index, left, top, right, bottom);
	++serial_;
}
void MenuStateFrame::set_widget_caret(int index, int caret) {
	frame_set_caret(state_, index, caret);
	++serial_;
}

std::string MenuStateFrame::get_widget_text(int index) const {
	return frame_widget_text(compiler_, state_, index);
}

int MenuStateFrame::item_count(int index) const {
	return configured_ ? compiler_.item_count(index, state_) : 0;
}

std::string MenuStateFrame::item_display_text(int index, int row) const {
	return configured_ ? compiler_.item_display_text(index, state_, row) : std::string();
}

bool MenuStateFrame::is_widget_disabled(int index) const {
	return configured_ && compiler_.widget_disabled(index, state_);
}

MenuRectF MenuStateFrame::widget_rect(int index) const {
	MenuRectF out;
	mnu::RectEdges rect{};
	if (!configured_ || !compiler_.widget_rect(index, state_, &rect)) return out;
	out.x = float(rect.left);
	out.y = float(rect.top);
	out.w = float(rect.right - rect.left);
	out.h = float(rect.bottom - rect.top);
	return out;
}

namespace {

// What the pump holds of a frame state that a sample can change: each window's hover, press and
// spin arrow, and the cursor's claim.
using PumpHeld = std::vector<std::tuple<int32_t, bool, bool, int32_t>>;
PumpHeld pump_held(const MenuFrameState &state) {
	PumpHeld held;
	for (const MenuWidgetState &row : state.widgets)
		if (row.hovered || row.pressed || row.spin_part)
			held.emplace_back(row.index, row.hovered, row.pressed, row.spin_part);
	held.emplace_back(-2, false, false, state.cursor_claim);
	held.emplace_back(-3, false, false, state.cursor_spin_part);
	return held;
}

} // namespace

int MenuStateFrame::process_mouse(float x, float y, bool button_down, bool &scroll_owned) {
	scroll_owned = false;
	if (!configured_) return -1;
	const PumpHeld before = pump_held(state_);
	const MenuFrameCompiler::MouseClaim claim = compiler_.pump_mouse(state_, x, y, button_down, 1.0f, 1.0f);
	state_.cursor_x = x;
	state_.cursor_y = y;
	if (pump_held(state_) != before) ++serial_;
	// The click: the release over the widget the press claimed (a press a scrollbar part took never
	// arms one), as the game's frame takes it (MenuFrame::process_mouse) [orig: CWnd_ProcessMouseEvent
	// @ 0x647a00 — the click event 0x3000001 after the child pump].
	const int clicked = click_.sample(claim.hovered, button_down, claim.scroll_index < 0);
	scroll_owned = claim.scroll_index >= 0;
	if (clicked >= 0 && clicked_) clicked_(clicked);
	if (claim.scroll_value_changed) {
		++serial_;
		if (scrolled_) scrolled_(claim.scroll_index, claim.scroll_value);
	}
	return claim.hovered;
}

bool MenuStateFrame::process_popup_mouse(int index, float x, float y, bool button_down) {
	if (!configured_) return false;
	const MenuFrameCompiler::MouseClaim claim =
			compiler_.pump_popup_mouse(state_, index, x, y, button_down, 1.0f, 1.0f);
	state_.cursor_x = x;
	state_.cursor_y = y;
	if (claim.scroll_value_changed) {
		++serial_;
		if (scrolled_) scrolled_(claim.scroll_index, claim.scroll_value);
	}
	if (claim.scroll_index >= 0) ++serial_;
	return claim.scroll_index >= 0;
}

bool MenuStateFrame::process_mouse_wheel(float x, float y, int steps) {
	if (!configured_) return false;
	MenuFrameCompiler::MouseClaim claim;
	if (!compiler_.pump_mouse_wheel(state_, x, y, steps, 1.0f, 1.0f, &claim)) return false;
	if (claim.scroll_value_changed) {
		++serial_;
		if (scrolled_) scrolled_(claim.scroll_index, claim.scroll_value);
	}
	return true;
}

void MenuStateFrame::set_cursor_state(bool visible, float x, float y) {
	state_.cursor_visible = visible;
	state_.cursor_x = x;
	state_.cursor_y = y;
}

int MenuStateFrame::combo_popup_row_at(int index, float x, float y) const {
	return configured_ ? compiler_.combo_popup_row_at(index, state_, x, y, 1.0f, 1.0f) : -1;
}

bool MenuStateFrame::combo_popup_contains(int index, float x, float y) const {
	return configured_ && compiler_.combo_popup_contains(index, state_, x, y, 1.0f, 1.0f);
}

int MenuStateFrame::list_row_at(int index, float x, float y) const {
	return configured_ ? compiler_.list_row_at(index, state_, x, y, 1.0f, 1.0f) : -1;
}

int MenuStateFrame::spin_arrow_at(int index, float x, float y) const {
	return configured_ ? compiler_.spin_arrow_at(index, state_, x, y, 1.0f, 1.0f) : 0;
}

bool MenuStateFrame::table_hit(int index, float x, float y, int *row, int *column) const {
	*row = -1;
	*column = -1;
	return configured_ && compiler_.table_hit(index, state_, x, y, 1.0f, 1.0f, row, column);
}

std::string MenuStateFrame::widget_mnemonic(int index) const {
	return configured_ ? compiler_.widget_mnemonic(index) : std::string();
}

void MenuStateFrame::set_open_popup(int index) {
	if (state_.popup_root == index) return;
	state_.popup_root = index;
	++serial_;
}

bool MenuStateFrame::edit_char(int index, int unicode) {
	if (!configured_) return false;
	++serial_;
	return frame_edit_char(compiler_, state_, index, unicode);
}

int MenuStateFrame::edit_key(int index, int key, bool shift) {
	if (!configured_) return 0;
	++serial_;
	return static_cast<int>(frame_edit_key(compiler_, state_, index, key, shift));
}

} // namespace opennova::menu
