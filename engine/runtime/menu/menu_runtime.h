#pragma once

// THE COMPILED-MENU INTERACTION RUNTIME (ADR 0040 ladder E5): everything the
// shell's menu driver decides that is not device work — the positional id
// tree over a parsed document, navigation and the back stack, the per-widget
// runtime state store and its replay onto a recompiled screen, ACTION
// dispatch, radio groups, the spin cycle, list / table / combo selection, the
// single open dropdown and its exclusive pump, edit focus, and the key
// routing (edit keys, VK hotkeys, character hotkeys).
//
// It drives ONE compiled frame through MenuFrameSeam (the embedder implements
// it over the engine draw-list/pump surface plus its redraw and cursor
// device work) and reports what happened through a SYNCHRONOUS event sink:
// observers may re-enter the runtime from the sink (a cross-.mnu jump swaps
// the document under a widget activation), so no document pointer is held
// across a sink call.
// [orig: CUIWidget_HandleScriptedAction @0x6497f0; CUIScene_SelectNodeByName
//  @0x63b6b0; UI_DispatchScreenEvent @0x54e6a0; dispatch_mouse_event
//  @0x63ab00; widget_process_mouse_event @0x647a00; combobox_handle_event
//  @0x65c190; edit_widget_handle_input_event @0x661510]

#include <formats/mnu/mnu.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::menu {

// The canonical item-row container of a list-like widget: a combo whose
// authored LIST_BOX carries an ITEMS block stores its rows there; everything
// else (and a styling-only LIST_BOX) uses the window-level ITEMS block. Null
// for a widget kind with no item list.
const mnu::Items *menu_items_container(const mnu::Window *window);

// ---- The positional id tree -------------------------------------------------

// Screens and windows numbered in ONE pre-order walk at load (ids start at 1:
// a screen container takes an id, then its root window's subtree), unique
// across both within one document. Pointers stay valid while the document is
// alive and unmodified (documents are read-only at run time).
class MenuDocIndex {
public:
	struct Node {
		int id = 0;
		int parent_id = 0; // 0 for a screen container
		int screen_index = -1;
		const mnu::Window *window = nullptr; // null for the screen container
		std::vector<int> child_ids;
	};

	void build(const mnu::Document &doc);
	void clear();

	const mnu::Document *document() const { return doc_; }
	int node_count() const { return static_cast<int>(nodes_.size()); }
	const Node *node(int id) const;
	const mnu::Window *window(int id) const;
	// The screen container ids, in document order.
	const std::vector<int> &screen_ids() const { return screen_ids_; }
	const mnu::Screen *screen(int screen_id) const;
	int screen_root_id(int screen_id) const;

private:
	int add_window_(const mnu::Window &w, int parent_id, int screen_index);

	const mnu::Document *doc_ = nullptr;
	std::vector<Node> nodes_; // nodes_[id - 1]
	std::vector<int> screen_ids_;
};

// ---- The frame seam ---------------------------------------------------------

struct MenuRectF {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	bool contains(float px, float py) const {
		return px >= x && py >= y && px < x + w && py < y + h;
	}
};

// What the runtime needs from the compiled frame. Indices are the current
// screen's pre-order widget indices; positions are frame-local surface pixels.
class MenuFrameSeam {
public:
	virtual ~MenuFrameSeam() = default;

	virtual bool is_configured() const = 0;
	// (Re)compile the named screen of the bound document: the device loads art,
	// fonts and the screen's text table.
	virtual void configure_screen(const std::string &screen) = 0;
	// The saved state was replayed onto the fresh compile: the device mounts
	// what rides it (marquee data, credits scrollers) and resets the cursor.
	virtual void screen_configured() = 0;

	virtual void set_widget_shown_override(int index, bool shown) = 0;
	virtual void set_widget_disabled(int index, bool disabled) = 0;
	virtual void set_widget_checked(int index, bool checked) = 0;
	virtual void set_widget_text(int index, const std::string &text) = 0;
	virtual void set_widget_items(int index, const std::vector<std::string> &items) = 0;
	virtual void set_widget_selection(int index, int selected, int hover, int scroll_row) = 0;
	virtual void set_widget_scroll_range(int index, int minimum, int maximum, int page,
			int value) = 0;
	virtual void set_widget_selected_set(int index, const std::vector<int> &rows) = 0;
	virtual void set_widget_table_rows(int index,
			const std::vector<std::vector<std::string>> &rows) = 0;
	virtual void set_widget_hover_item(int index, int row) = 0;
	virtual void set_widget_popup_open(int index, bool open) = 0;
	virtual void set_widget_focused(int index, bool focused) = 0;
	virtual void set_widget_caret(int index, int caret) = 0;

	virtual int get_widget_caret(int index) const = 0;
	virtual std::string get_widget_text(int index) const = 0;
	virtual int item_count(int index) const = 0;
	virtual bool is_widget_disabled(int index) const = 0;
	virtual MenuRectF widget_rect(int index) const = 0; // design space
	// Surface pixels per design unit (1,1 before the surface has a size).
	virtual void design_scale(float &sx, float &sy) const = 0;

	// The per-sample pump: returns the claimed widget index (-1 none).
	virtual int process_mouse(float x, float y, bool button_down) = 0;
	virtual bool process_popup_mouse(int index, float x, float y, bool button_down) = 0;
	virtual bool process_mouse_wheel(float x, float y, int steps) = 0;
	virtual void set_cursor_state(bool visible, float x, float y) = 0;
	// The claim's cursor becomes the live cursor / back to the stock cursor.
	virtual void apply_claim_cursor() = 0;
	virtual void reset_cursor() = 0;

	virtual int combo_popup_row_at(int index, float x, float y) const = 0;
	virtual bool combo_popup_contains(int index, float x, float y) const = 0;
	virtual int list_row_at(int index, float x, float y) const = 0;
	virtual int spin_arrow_at(int index, float x, float y) const = 0; // 0 / 1 up / 2 down
	virtual int table_row_at(int index, float x, float y) const = 0;
	virtual int hotkey_widget(const std::string &key, bool virtual_key) const = 0;
	virtual bool edit_char(int index, int unicode) = 0;
	virtual int edit_key(int index, int key, bool shift) = 0; // EditKeyResult
};

// ---- Events -----------------------------------------------------------------

struct MenuEvent {
	enum class Kind {
		ScreenChanged,   // text = screen name
		MusicVar,        // value = the screen's MUSICVAR (0 when unauthored)
		MenuRequested,   // text = file, text2 = target screen
		QuitRequested,
		UrlRequested,    // text = url
		Sound,           // text = bank file, text2 = trigger
		ValueChanged,    // text = widget name, text2 = kind, value = row, text3 = value text
		WidgetActivated, // id, text = widget name
		ListActivated,   // id, value = row
		HoverChanged,    // id, flag = hovered
		ShownChanged,    // id, flag = shown
	};
	Kind kind = Kind::ScreenChanged;
	int id = -1;
	int value = 0;
	bool flag = false;
	std::string text, text2, text3;
};
using MenuEventSink = std::function<void(const MenuEvent &)>;

// One key press as the runtime routes it (the embedder maps its key events).
struct MenuKeyInput {
	enum class Key { None, Escape, Enter, Backspace, End, Home, Left, Right, Delete };
	Key key = Key::None;
	int unicode = 0;           // the typed character, 0 when none
	int printable_keycode = 0; // 0x20..0x7E fallback when `unicode` is 0
	bool shift = false;
};

// The standalone CScrollWnd range a companion seeds.
struct MenuScrollRangeState {
	int minimum = 0, maximum = 0, page = 0, value = 0;
};

// Runtime state of one document widget id. Every override carries its own
// latch: an override nobody wrote falls back to the document's authored
// flags / text / items.
struct MenuWidgetRuntimeState {
	bool has_shown = false, shown = false;
	bool has_disabled = false, disabled = false;
	bool has_checked = false, checked = false;
	bool has_text = false;
	std::string text;
	bool has_items = false;
	std::vector<std::string> items;
	bool has_selected_item = false;
	int selected_item = 0;
	bool has_scroll_row = false;
	int scroll_row = 0;
	bool has_scroll_range = false;
	MenuScrollRangeState scroll_range;
	bool has_selected_set = false;
	std::vector<int> selected_set;
	bool has_table_rows = false;
	std::vector<std::vector<std::string>> table_rows;
	std::vector<int> table_selected;

	// True while no override has been written (a replay skips the widget).
	bool empty() const {
		return !(has_shown || has_disabled || has_checked || has_text || has_items ||
				has_selected_item || has_scroll_row || has_scroll_range || has_selected_set ||
				has_table_rows);
	}
};

// ---- The runtime ------------------------------------------------------------

class MenuRuntime {
public:
	// The list/table double-click window, milliseconds (the embedder's
	// platform default; retail's is the OS double-click time).
	static constexpr uint32_t kDoubleClickMs = 400;

	void set_frame(MenuFrameSeam *frame) { frame_ = frame; }
	MenuFrameSeam *frame() const { return frame_; }
	void set_sink(MenuEventSink sink) { sink_ = std::move(sink); }

	// Bind a parsed document (null unbinds) and show `target_screen` (empty or
	// unknown = the first screen). Every per-document cache is rebuilt and the
	// runtime widget state is dropped. False when there is no screen to show.
	bool open_document(const mnu::Document *doc, const std::string &menu_file,
			const std::string &target_screen);

	const mnu::Document *document() const { return index_.document(); }
	const MenuDocIndex &index() const { return index_; }
	const std::string &menu_file() const { return menu_file_; }
	const std::string &current_screen() const { return current_screen_; }
	const std::vector<std::string> &screen_names() const { return screen_order_; }
	// The current screen's container id, -1 when none.
	int current_screen_id() const;

	// Show a screen (no stack change); unknown screen -> false. Closes the open
	// dropdown first [orig: CUIScene_SelectNodeByName @0x63b6b0 closes
	// g_ui_active_combo_wnd].
	bool show_screen(const std::string &name);
	// The in-menu forward move (same-file SCREEN actions): pushes the current
	// screen for pop_screen.
	bool navigate_to_screen(const std::string &name);
	// Back within the file; popping past the root is the shell's back/quit
	// (QuitRequested, returns false).
	bool pop_screen();

	// ---- addressing (document-wide, doc-id keyed) ----
	// Case-insensitive, first match in document order; -1 = absent.
	int widget_id(const std::string &name) const;
	std::string widget_name_of(int id) const;
	int widget_kind_of(int id) const; // mnu::WindowType as int, -1 unknown id
	std::string widget_screen_of(int id) const;
	// The current screen's pre-order index of a doc id; -1 off-screen or frameless.
	int frame_index(int id) const;
	int id_at_index(int index) const;
	// Every doc id on the current screen, in pre-order.
	const std::vector<int> &current_screen_ids() const { return id_of_index_; }

	// ---- state ----
	void set_widget_shown(int id, bool shown);
	bool is_widget_shown(int id) const;
	void set_widget_disabled(int id, bool disabled);
	bool is_widget_disabled(int id) const;
	void set_widget_checked(int id, bool checked);
	bool is_widget_checked(int id) const;
	void set_widget_text(int id, const std::string &text);
	std::string get_widget_text(int id) const;
	// Persist an edit widget's text for cross-screen reads.
	void remember_widget_text(int id, const std::string &text);
	void set_widget_items(int id, const std::vector<std::string> &items);
	std::vector<std::string> get_widget_items(int id) const;
	int item_count(int id) const;
	std::string item_text(int id, int row) const;
	// The authored item `value=` attribute of a row.
	std::string item_value(int id, int row) const;
	// Retail's select-by-value seed [orig: SpinList_SelectItemByValue
	// @0x64ba50]: the row whose authored value equals `value`, row 0 on a miss;
	// nothing when the widget has no item list.
	void select_row_by_value(int id, const std::string &value, bool emit);
	void select_row(int id, int row, bool emit);
	int selected_row(int id) const;
	std::vector<int> selected_rows(int id) const;
	std::vector<int> selected_set(int id) const;
	void set_selected_set(int id, const std::vector<int> &rows);
	void set_scroll_row(int id, int row);
	void set_widget_scroll_range(int id, int minimum, int maximum, int page, int value);
	bool get_widget_scroll_range(int id, MenuScrollRangeState &out) const;
	// The engine pump's CScrollWnd interaction result (already applied to the
	// frame): mirror it into the store and relay the value change.
	void on_frame_scroll_value(int index, int value);

	void table_add_row(int id, const std::vector<std::string> &cells);
	void table_remove_row(int id, int row);
	void table_clear_rows(int id);
	int table_row_count(int id) const;
	std::string table_cell_text(int id, int row, int col) const;
	std::vector<int> table_selected_rows(int id) const;
	void table_select_row(int id, int row, bool additive);

	// ---- activation / actions ----
	// WidgetActivated fires BEFORE the scripted ACTION list. Retail runs ACTIONs
	// first [orig: CUIWidget_HandleScriptedAction @0x6497f0: ACTION walk, then
	// widget[63]->vtable+32] but keeps every screen alive; the shell replaces
	// the document on a cross-.mnu jump, so observers read their still-live
	// controls first, and the dispatch is skipped when an observer swapped the
	// document under the event.
	void activate(int id);
	// Check one radio and uncheck its GROUP siblings on the same screen.
	void select_radio(int id);
	// Wrap-around cycle (the spin arrows' step); emits the value change.
	void spin_cycle(int id, int delta);
	// One parsed ACTION row [orig: CUIWidget_HandleScriptedAction @0x6497f0].
	// True when handled; the service verbs (FORM_POST, GLB_*, APPMSG, LAN_*,
	// MNX) are the shell's and return false.
	bool dispatch_action(const mnu::Action &action);
	// WINDOW action: show/hide/enable/disable a named widget of the CURRENT
	// screen, with the retail TOGGLE flag inverting the current state.
	bool handle_window_action(const std::string &target, const std::string &state_lower,
			bool toggle);
	void play_widget_state_sound(int id, const std::string &state_token);
	void emit_edit_changed(int id);

	// ---- input ----
	// One raw-mouse sample in frame-local coordinates.
	void process_mouse(float x, float y, bool button_down);
	// One wheel tick (+1 rows-down, -1 rows-up); true when a target claimed it.
	bool process_wheel(float x, float y, int steps);
	// The frame reported a click on widget `index` (its pump's press edge).
	void on_widget_clicked(int index, uint32_t now_ms, bool ctrl_down);
	// One key press; true when consumed. Order: focused edit, the virtual-key
	// hotkey scan, the character scan.
	bool handle_key(const MenuKeyInput &key, uint32_t now_ms, bool ctrl_down);
	void focus_edit(int id);
	void close_active_combo_popup();
	bool is_combo_popup_open(int id) const { return open_combo_id_ == id; }
	int focused_widget() const { return focus_id_; }

private:
	void emit_(const MenuEvent &event) const;
	MenuWidgetRuntimeState &state_of_(int id);
	const MenuWidgetRuntimeState *saved_state_(int id) const;
	void index_document_();
	void index_widget_subtree_(const std::string &screen_name, int id);
	void rebuild_index_maps_();
	void map_widget_subtree_(int id);
	void configure_frame_();
	void replay_state_();
	void on_screen_shown_();
	void push_table_rows_(int id);
	void push_table_selection_(int id);
	void emit_value_changed_for_(int id, int row);
	void on_claim_changed_(int previous, int current);
	void activate_widget_(int id, int index, float x, float y, uint32_t now_ms, bool ctrl_down);
	void list_click_(int id, int kind, int row, uint32_t now_ms, bool ctrl_down);
	void table_click_(int id, int row, uint32_t now_ms, bool ctrl_down);
	bool register_click_(int id, int row, uint32_t now_ms);
	void open_combo_popup_(int id);
	void clear_edit_focus_();
	bool route_edit_key_(const MenuKeyInput &key);
	bool trigger_hotkey_target_(int index, uint32_t now_ms, bool ctrl_down);

	struct WidgetInfo {
		std::string screen;
		std::string name;
		int kind = -1;
	};

	MenuFrameSeam *frame_ = nullptr;
	MenuEventSink sink_;
	MenuDocIndex index_;
	std::string menu_file_;
	std::string current_screen_;
	std::vector<std::string> nav_stack_;
	std::unordered_map<std::string, int> screen_ids_; // upper name -> screen id
	std::vector<std::string> screen_order_;
	std::unordered_map<std::string, int> name_to_id_; // upper name -> first doc id
	std::unordered_map<int, WidgetInfo> id_info_;
	std::unordered_map<int, MenuWidgetRuntimeState> id_state_;
	MenuWidgetRuntimeState scratch_state_;
	std::vector<int> id_of_index_;
	std::unordered_map<int, int> index_of_id_;

	// keyboard/edit focus [orig: g_ui_focus_wnd @0x31C16D4]
	int focus_id_ = -1;
	// single open dropdown [orig: g_ui_active_combo_wnd @0x31C16D0]
	int open_combo_id_ = -1;
	int last_claim_ = -1;
	float last_mouse_x_ = 0.0f, last_mouse_y_ = 0.0f;
	bool mouse_down_ = false;
	int last_click_id_ = -1;
	int last_click_row_ = -1;
	uint32_t last_click_ms_ = 0;
};

} // namespace opennova::menu
