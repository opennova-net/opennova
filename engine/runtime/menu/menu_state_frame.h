#pragma once

// The frame a menu's runtime drives, as state: what each MenuFrameSeam write
// leaves in a MenuFrameState (the per-widget runtime overrides the compiler
// draws, menu_frame.h), and the seam over a compiler and its state alone.
// Godot's MenuFrame (godot/src/mnu) writes its state through these functions
// and rasterizes what the compiler draws of it; MenuStateFrame is the same
// frame with nothing drawn, so a menu runs headless (the editor's menu
// preview in Try mode, ADR 0046 DI-35) through the runtime's own driver and
// the frame's own pump, at design scale.

#include <runtime/menu/menu_edit.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_runtime.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::menu {

// The row of `index` in the state, made on first use; the row of `index`, null for none.
MenuWidgetState &frame_widget(MenuFrameState &state, int index);
const MenuWidgetState *find_frame_widget(const MenuFrameState &state, int index);

// The seam's writes, each the override the compiler draws (menu_frame.h MenuWidgetState).
void frame_set_shown(MenuFrameState &state, int index, bool shown);
void frame_set_disabled(MenuFrameState &state, int index, bool disabled);
void frame_set_checked(MenuFrameState &state, int index, bool checked);
void frame_set_focused(MenuFrameState &state, int index, bool focused);
void frame_set_caret(MenuFrameState &state, int index, int caret);
void frame_set_text(MenuFrameState &state, int index, const std::string &text);
// CWnd_SetRect: the widget's own rect, parent-relative design edges.
void frame_set_rect(MenuFrameState &state, int index, int left, int top, int right, int bottom);
// The open dropdown's hovered row; false when it already was.
bool frame_set_hover_item(MenuFrameState &state, int index, int row);
void frame_set_selection(MenuFrameState &state, int index, int selected, int hover, int scroll_row);
// A standalone CScrollWnd's range, its value clamped into it (a minimum past the maximum is 0..0).
void frame_set_scroll_range(MenuFrameState &state, int index, int minimum, int maximum, int page, int value);
void frame_set_popup_open(MenuFrameState &state, int index, bool open);
void frame_set_items(MenuFrameState &state, int index, const std::vector<std::string> &items);
void frame_set_selected_set(MenuFrameState &state, int index, const std::vector<int> &rows);
void frame_set_disabled_items(MenuFrameState &state, int index, const std::vector<uint8_t> &rows);
void frame_set_table_rows(MenuFrameState &state, int index, const std::vector<MenuTableRow> &rows);
// CWnd_SetClipRect, absolute design edges (`enabled` false removes it).
void frame_set_clip_rect(MenuFrameState &state, int index, bool enabled, int left, int top, int right,
		int bottom);
void frame_set_table_columns(MenuFrameState &state, int index, bool installed,
		const std::vector<MenuTableColumn> &columns, int sort_column);

// A widget's text as the frame holds it (the runtime's, else the authored), and its caret (-1 none).
std::string frame_widget_text(const MenuFrameCompiler &compiler, const MenuFrameState &state, int index);
int frame_widget_caret(const MenuFrameState &state, int index);
// The edit's input over its text and caret (menu_edit.h, the witnessed ops): a typed character
// inserted (false: not insertable, or the field refused it), a special key applied.
bool frame_edit_char(const MenuFrameCompiler &compiler, MenuFrameState &state, int index, int unicode);
EditKeyResult frame_edit_key(const MenuFrameCompiler &compiler, MenuFrameState &state, int index, int key,
		bool shift);

// The frame seam over a compiler and its state, nothing drawn (MenuRuntime's seam, menu_runtime.h):
// the runtime's writes land in the state as Godot's MenuFrame lands them, and the mouse runs the
// compiler's own pump at design scale, its click and its press's capture the game frame's
// (MenuClickLatch, menu_click.h), each click (the window and its spin arrow part) and each scroll
// value reported as the frame's signals report them (the embedder hands them to the runtime:
// on_widget_clicked, on_frame_scroll_value). What a screen loads is the embedder's: the
// configure hook configures the compiler for a screen by name (its document, fonts, string tables
// and textures), true when it did.
class MenuStateFrame final : public MenuFrameSeam {
public:
	using Configure = std::function<bool(const std::string &screen, MenuFrameCompiler &compiler)>;
	using Clicked = std::function<void(int index, int part)>;
	using Scrolled = std::function<void(int index, int value)>;

	void set_configure(Configure configure) { configure_ = std::move(configure); }
	void set_clicked(Clicked clicked) { clicked_ = std::move(clicked); }
	void set_scrolled(Scrolled scrolled) { scrolled_ = std::move(scrolled); }

	MenuFrameCompiler &compiler() { return compiler_; }
	const MenuFrameCompiler &compiler() const { return compiler_; }
	const MenuFrameState &state() const { return state_; }
	// The state's clock (a focused edit's caret blink, menu_caret_shown).
	void set_time_ms(uint32_t ms) { state_.time_ms = ms; }
	// Moves with every write to the state and every sample that changes what the pump holds (a
	// window under the mouse or let go, pressed, its spin arrow, the cursor's claim).
	uint64_t serial() const { return serial_; }
	// How many screens it configured.
	uint64_t configures() const { return configures_; }
	// Nothing configured: the state starts over (what the compiler holds is the embedder's to let go).
	void clear();

	bool is_configured() const override { return configured_; }
	void configure_screen(const std::string &screen) override;
	void screen_configured() override {}
	void set_widget_shown_override(int index, bool shown) override;
	void set_widget_disabled(int index, bool disabled) override;
	void set_widget_checked(int index, bool checked) override;
	void set_widget_text(int index, const std::string &text) override;
	void set_widget_items(int index, const std::vector<std::string> &items) override;
	void set_widget_selection(int index, int selected, int hover, int scroll_row) override;
	void set_widget_scroll_range(int index, int minimum, int maximum, int page, int value) override;
	void set_widget_selected_set(int index, const std::vector<int> &rows) override;
	void set_widget_disabled_items(int index, const std::vector<uint8_t> &rows) override;
	void set_widget_table_rows(int index, const std::vector<MenuTableRow> &rows) override;
	void set_widget_table_columns(int index, bool installed, const std::vector<MenuTableColumn> &columns,
			int sort_column) override;
	void set_widget_clip_rect(int index, bool enabled, int left, int top, int right, int bottom) override;
	void set_widget_hover_item(int index, int row) override;
	void set_widget_popup_open(int index, bool open) override;
	void set_widget_focused(int index, bool focused) override;
	void set_widget_rect(int index, int left, int top, int right, int bottom) override;
	void set_widget_caret(int index, int caret) override;

	int get_widget_caret(int index) const override { return frame_widget_caret(state_, index); }
	std::string get_widget_text(int index) const override;
	int item_count(int index) const override;
	std::string item_display_text(int index, int row) const override;
	bool is_widget_disabled(int index) const override;
	MenuRectF widget_rect(int index) const override;
	void design_scale(float &sx, float &sy) const override { sx = sy = 1.0f; }

	std::vector<MenuPumpWindow> press_mouse(float x, float y) override;
	int process_mouse(float x, float y, bool button_down) override;
	void release_mouse() override { click_.release(); }
	bool press_popup_mouse(int index, float x, float y) override;
	bool process_popup_mouse(int index, float x, float y, bool button_down) override;
	bool process_mouse_wheel(float x, float y, int steps) override;
	void set_cursor_state(bool visible, float x, float y) override;
	void apply_claim_cursor() override {}
	void reset_cursor() override {}

	int combo_popup_row_at(int index, float x, float y) const override;
	bool combo_popup_contains(int index, float x, float y) const override;
	int list_row_at(int index, float x, float y) const override;
	int spin_arrow_at(int index, float x, float y) const override;
	bool table_hit(int index, float x, float y, int *row, int *column) const override;
	std::string widget_mnemonic(int index) const override;
	std::string widget_string(int index, const std::string &key) const override;
	void set_open_popup(int index) override;
	bool edit_char(int index, int unicode) override;
	int edit_key(int index, int key, bool shift) override;

private:
	MenuFrameCompiler compiler_;
	MenuFrameState state_;
	MenuClickLatch click_;
	Configure configure_;
	Clicked clicked_;
	Scrolled scrolled_;
	bool configured_ = false;
	uint64_t serial_ = 0;
	uint64_t configures_ = 0;
};

} // namespace opennova::menu
