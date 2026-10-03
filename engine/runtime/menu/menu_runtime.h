#pragma once

// THE COMPILED-MENU INTERACTION RUNTIME (ADR 0040 ladder E5): everything the
// shell's menu driver decides that is not device work — the positional id
// tree over a parsed document, navigation and the back stack, the per-widget
// runtime state store and its replay onto a recompiled screen, ACTION
// dispatch, radio groups, the spin cycle, list / table / combo selection, the
// single open dropdown and its exclusive pump, the open MODAL popup, the
// keyboard focus, the per-screen hotkey table and the key routing.
//
// It drives ONE compiled frame through MenuFrameSeam (the embedder implements
// it over the engine draw-list/pump surface plus its redraw and cursor
// device work) and reports what happened through a SYNCHRONOUS event sink:
// observers may re-enter the runtime from the sink (a cross-.mnu jump swaps
// the document), so no document pointer is held across a sink call.
// Retail keeps every loaded screen in one scene; this runtime holds one
// document, and a jump to another file (MenuRequested) or back past the
// file's own history (PopRequested) is the embedder's, raised after the
// activation's callbacks so they read the screen they fired on, as retail's
// still-loaded screen lets them (docs/mnu/menu-re.md, "Activation and the
// ACTION walk").
// [orig: CUIWidget_HandleScriptedAction @0x6497f0; CUIScene_SelectNodeByName
//  @0x63b6b0; UI_DispatchScreenEvent @0x54e6a0; UI_DispatchMouseEvent
//  @0x63ab00; CWnd_ProcessMouseEvent @0x647a00; CComboWnd_HandleEvent
//  @0x65c190; CEditWnd_HandleInputEvent @0x661510;
//  UI_DispatchKeyboardEventToChildren @0x63ad10]

#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_table.h>
#include <runtime/menu/menu_table_row.h>

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
	// The screen's first root window (a screen node's children are all its
	// roots, in document order); -1 when it has none.
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
	virtual void set_widget_table_rows(int index, const std::vector<MenuTableRow> &rows) = 0;
	// The column records code set up (none: the XML ones; menu_table.h
	// MenuTableColumn: a record a count kept, one it started over, an init over
	// either) and the sorted column (-1 none).
	virtual void set_widget_table_columns(int index, bool installed,
			const std::vector<MenuTableColumn> &columns, int sort_column) = 0;
	// CWnd_SetClipRect: the widget's own passes clipped to an absolute design
	// rect (`enabled` false removes the clip).
	virtual void set_widget_clip_rect(int index, bool enabled, int left, int top, int right,
			int bottom) = 0;
	virtual void set_widget_hover_item(int index, int row) = 0;
	virtual void set_widget_popup_open(int index, bool open) = 0;
	virtual void set_widget_focused(int index, bool focused) = 0;
	// CWnd_SetRect: the widget's own (parent-relative) design rect replaced.
	virtual void set_widget_rect(int index, int left, int top, int right, int bottom) = 0;
	virtual void set_widget_caret(int index, int caret) = 0;

	virtual int get_widget_caret(int index) const = 0;
	virtual std::string get_widget_text(int index) const = 0;
	virtual int item_count(int index) const = 0;
	virtual std::string item_display_text(int index, int row) const = 0;
	virtual bool is_widget_disabled(int index) const = 0;
	virtual MenuRectF widget_rect(int index) const = 0; // design space
	// Surface pixels per design unit (1,1 before the surface has a size).
	virtual void design_scale(float &sx, float &sy) const = 0;

	// The per-sample pump: returns the claimed widget index (-1 none);
	// `scroll_owned` reports a sample a scrollbar part took (its press never
	// reaches the owner widget).
	virtual int process_mouse(float x, float y, bool button_down, bool &scroll_owned) = 0;
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
	// The table hit test (MenuFrameCompiler::table_hit, CTableWnd_HitTest): the
	// data row (-1 the header strip) and column (-1 none) under the point;
	// false where retail fails.
	virtual bool table_hit(int index, float x, float y, int *row, int *column) const = 0;
	// The parse-time {hot} mnemonic of a widget (MenuFrameCompiler::
	// widget_mnemonic), empty when none.
	virtual std::string widget_mnemonic(int index) const = 0;
	// The open popup (a shown MODAL window) as a pre-order index, -1 none: the
	// frame's pump then serves its subtree alone.
	virtual void set_open_popup(int index) = 0;
	virtual bool edit_char(int index, int unicode) = 0;
	virtual int edit_key(int index, int key, bool shift) = 0; // EditKeyResult
};

// ---- Events -----------------------------------------------------------------

struct MenuEvent {
	enum class Kind {
		ScreenChanged,   // text = screen name
		MusicVar,        // value = the screen's MUSICVAR (0 when unauthored)
		MenuRequested,   // text = file, text2 = target screen
		// POP_SCREEN with the file's own history empty: the embedder's
		// cross-file history pops, else nothing happens.
		PopRequested,
		UrlRequested,    // text = url (trimmed), flag = the external browser
		// GLB_FILTER / GLB_FILTER_NUM from an edit's keys: id = the receiving
		// control, value = atol(FIELD) (the column), text = the edit's text,
		// flag = numeric (GLB_FILTER_NUM), text2 = TEST.
		FilterRequested,
		Sound,           // text = bank file, text2 = trigger
		ValueChanged,    // text = widget name, text2 = kind, value = row, text3 = value text
		WidgetActivated, // id, text = widget name
		ListActivated,   // id, value = row
		HoverChanged,    // id, flag = hovered
		ShownChanged,    // id, flag = shown
		EditCommitted,   // id, text = widget name: an edit's Enter (event 0x7000002)
		// A press on a table's data row (the 0x8000001 cell event after the
		// table's own selection write): id, text = widget name, value = row,
		// column (-1 none), state = the row's state after the write,
		// cell_value = that column's cell value, flag = the double-click form.
		TableCellClicked,
	};
	Kind kind = Kind::ScreenChanged;
	int id = -1;
	int value = 0;
	int column = -1;
	int32_t state = 0;
	int32_t cell_value = 0;
	bool flag = false;
	std::string text, text2, text3;
};
using MenuEventSink = std::function<void(const MenuEvent &)>;

// One key press as the runtime routes it, the way Windows delivers it: a
// WM_KEYDOWN with the key's virtual-key code, then a WM_CHAR with the
// character it types (the embedder maps its key events).
struct MenuKeyInput {
	int vk = 0;                // the virtual-key code (VK_RETURN 13, VK_ESCAPE 27, ...), 0 none
	int unicode = 0;           // the typed character, 0 when none
	int printable_keycode = 0; // 0x20..0x7E: the character scan's fallback when `unicode` is 0
	bool shift = false;
};

// One row of the current screen's hotkey table [orig: scene_add_widget_event_callback
// @ 0x63a8e0 — {isVirtual, key, widget}].
struct MenuHotkeyRow {
	bool virtual_key = false;
	int key = 0;
	int id = -1;
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
	std::vector<MenuTableRow> table_rows;
	// The columns code installed [orig: init_table_row @ 0x63f9c0 from the shell],
	// the sort-key stack and the sorted column [orig: CTableWnd_SortByColumn
	// @ 0x640900].
	bool has_table_columns = false;
	std::vector<MenuTableColumn> table_columns;
	std::vector<int> table_sort_keys;
	int table_sort_column = -1;
	// A runtime rect (CWnd_SetRect), parent-relative design units.
	bool has_rect = false;
	int rect_left = 0, rect_top = 0, rect_right = 0, rect_bottom = 0;
	// A clip rect (CWnd_SetClipRect), absolute design units.
	bool has_clip = false;
	bool clip_enabled = false;
	int clip_left = 0, clip_top = 0, clip_right = 0, clip_bottom = 0;

	// True while no override has been written (a replay skips the widget).
	bool empty() const {
		return !(has_shown || has_disabled || has_checked || has_text || has_items ||
				has_selected_item || has_scroll_row || has_scroll_range || has_selected_set ||
				has_table_rows || has_table_columns || has_rect || has_clip);
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
	// The mounted game's `/game` code: the STARTUP screen's VERSION label shows that
	// game's version text (gameprofile_version_text_for_code: JO's for an empty or
	// unknown code, none for a game whose binary is unwitnessed).
	void set_game_code(std::string code) { game_code_ = std::move(code); }

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

	// Show a screen (no stack change); unknown screen -> false. By-name
	// lookups find the LAST screen of a name [orig: CUIScene_SelectNodeByName
	// @0x63b6b0 walks the scene newest first]. Closes the open dropdown and
	// the open popup and drops the focus [orig: @ 0x63b6b8 clears
	// g_UIOpenPopupWnd; @ 0x63b7ca clears focus, capture and mouseover].
	bool show_screen(const std::string &name);
	// The SCREEN action's select: the screen by name, pushing the current one
	// on the history (even when it is the same screen) [orig:
	// CUIScene_SelectNodeByName(name, 1) -> UIScene_PushScreenHistory
	// @ 0x63b350]. An unknown name changes nothing.
	bool navigate_to_screen(const std::string &name);
	// POP_SCREEN [orig: UIScene_PopScreenHistory @ 0x63c410]: the file's own
	// history pops; with it empty the embedder's cross-file history does
	// (PopRequested at once, returns false; a POP_SCREEN row holds it until the
	// activation's callbacks ran); with that empty too nothing happens.
	bool pop_screen();

	// ---- addressing (doc-id keyed) ----
	// The embedder's by-name seam: the current screen's control (find_control),
	// else the first screen in document order holding one; -1 = absent.
	int widget_id(const std::string &name) const;
	// Retail's control lookup [orig: UI_FindScreenControl @ 0x63ae80]: the
	// screen of that name (empty: the current screen; a duplicate name finds
	// the last), its root windows in document order, each searched pre-order
	// by NAME (case-insensitive) where a window with no NAME ends its branch
	// [orig: CWnd_FindChildByName @ 0x646850]. -1 = absent.
	int find_control(const std::string &screen, const std::string &name) const;
	std::string widget_name_of(int id) const;
	int widget_kind_of(int id) const; // mnu::WindowType as int, -1 unknown id
	std::string widget_screen_of(int id) const;
	// A named screen's control the way retail finds one: the first screen of that name
	// (case-insensitive), then a pre-order search of its windows, each window before its
	// children, names compared case-insensitively; an UNNAMED window ends the search of
	// its own subtree. -1 when absent.
	// [orig: UI_FindScreenControl @0x63ae80 (the section by stricmp, then its root
	//  windows); CWnd_FindChildByName @0x646850 (`!name || !window->name` returns 0
	//  before the children are searched)]
	int find_screen_control(const std::string &screen, const std::string &name) const;
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
	// Move a widget: its own rect (relative to its parent's origin, design
	// units) replaced until the document reopens [orig: CWnd_SetRect @0x646560,
	// sub_646580 @0x646580 (the same at the widget's own square size)].
	void set_widget_rect(int id, int left, int top, int right, int bottom);
	// Clip a widget's own passes to an absolute design rect until the
	// document reopens; `enabled` false removes it [orig: CWnd_SetClipRect
	// @0x646210].
	void set_widget_clip_rect(int id, bool enabled, int left, int top, int right, int bottom);
	// Persist an edit widget's text for cross-screen reads.
	void remember_widget_text(int id, const std::string &text);
	void set_widget_items(int id, const std::vector<std::string> &items);
	std::vector<std::string> get_widget_items(int id) const;
	int item_count(int id) const;
	std::string item_text(int id, int row) const;
	// The row's displayed text (an authored `type="id"` row resolved through
	// the screen's string table); "" off-screen or out of range.
	std::string item_display_text(int id, int row) const;
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

	// ---- tables (the CTableWnd operations, menu_table_row.h) ----
	// The column count a populate sets before it defines its columns: false
	// below 1 (the resize fails). A count that does not grow the table keeps
	// its records (the authored columns); one that grows it starts every record
	// over, zeroed (menu_table.h MenuTableColumn::kept).
	// [orig: CTableWnd vtable +0x6C -> CTableWnd_ResizeColumnCount @0x63f6c0]
	bool table_set_column_count(int id, int count);
	// CTableWnd_InitRow(column, 0, width, label, justify, vjustify) on one record
	// of the current count: false out of range. The record keeps its cell type,
	// cell offsets, SUBST rows and bitmap scale (menu_table.h).
	// [orig: CTableWnd_InitRow @0x63f9c0 — the bounds @0x63f9c8..0x63f9d9]
	bool table_init_column(int id, int column, int width, const std::string &label,
			int justify, int vjustify);
	// Append a row whose cells are `cells` (column order), cell values 0.
	void table_add_row(int id, const std::vector<std::string> &cells);
	// CTableWnd_AddRow: the landed row index (-1 for a widget that is not a
	// table).
	int table_insert_row(int id, const std::string &text0, int32_t value0, uint32_t flags,
			int insert_index);
	void table_set_cell_text(int id, int row, int col, const std::string &text);
	void table_set_cell_value(int id, int row, int col, int32_t value);
	int32_t table_cell_value(int id, int row, int col) const;
	// CTableWnd_GetRowValue: column 0's cell value.
	int32_t table_row_value(int id, int row) const { return table_cell_value(id, row, 0); }
	// -1 clears the table [orig: CTableWnd_RemoveRow @0x641a40].
	void table_remove_row(int id, int row);
	void table_clear_rows(int id) { table_remove_row(id, -1); }
	int table_row_count(int id) const;
	std::string table_cell_text(int id, int row, int col) const;
	int32_t table_row_state(int id, int row) const;
	// CTableWnd_SetRowSelected with the table's MULTISELECT; -1 = every row.
	void table_set_row_selected(int id, int row, bool selected);
	bool table_row_selected(int id, int row) const {
		return table_row_state(id, row) == kTableRowSelected;
	}
	// A row's colour override; `enable` false drops it, row -1 is every row
	// [orig: sub_640110 — the row's +28 bit 4 and +32].
	void table_set_row_color(int id, int row, bool enable, uint32_t color);
	// The rows in state 3, ascending.
	std::vector<int> table_selected_rows(int id) const;
	// Select `row` alone (every other non-locked row cleared), or toggle it
	// with `additive`.
	void table_select_row(int id, int row, bool additive);
	// Code-installed columns, as the stat fill sets them up: the count
	// (table_set_column_count), then one init per column with its label, width,
	// justification and sort compare (table_init_column; the rest of each entry
	// is not read) [orig: StatScreen_PopulateStatResultsList @ 0x562240 ->
	// CTableWnd_ResizeColumnCount, CTableWnd_InitRow]; the sort-key stack starts
	// over. A count that grows the table (the stat RESULTLIST's, which authors no
	// COLUMN) leaves no authored column behind; an empty set changes nothing.
	void table_set_columns(int id, const std::vector<MenuTableColumn> &columns);
	// A column's sort direction [orig: sub_63EC30 writes the column's +120].
	void table_set_column_ascending(int id, int column, bool ascending);
	// Sort the rows by `column` [orig: CTableWnd_SortByColumn @ 0x640900: the key
	// pushed, the rows sorted by CTableWnd_CompareRows @ 0x63e9c0 with their
	// selection and colours, the column's sort indicator set]; needs installed
	// columns.
	void table_sort_by_column(int id, int column);
	// The column whose header shows the sort indicator, -1 none.
	int table_sort_column(int id) const;

	// ---- activation / actions ----
	// The widget's activation, the click event 0x3000001 [orig:
	// CWnd_EmitEventToNamedHandlerAndCallbacks @ 0x646970 -> the class handler
	// (vtable+32), then the control callbacks]: the class's own step (a
	// checkbox toggles, a radio checks, a combo opens or closes its list), the
	// ACTION rows last-authored first (a widget with no NAME runs none), the
	// event up the parents (an ancestor whose NAME matches the child's byte for
	// byte runs its rows too), then WidgetActivated (the control callbacks the
	// embedder binds by name), then the cross-file requests the rows made (a
	// SCREEN row naming another file, a POP_SCREEN past the file's own
	// history, and every SCREEN or POP_SCREEN row after one of those), in walk
	// order. A row or observer that swaps the document stops the rest.
	void activate(int id);
	// Check a radio and uncheck the radios beside it (the same parent) of the
	// same nonzero GROUP [orig: radio_button_on_click @ 0x656cd0].
	void select_radio(int id);
	// The spin list's step: +1 SelectNext, -1 SelectPrevious, wrapping; emits
	// the value change [orig: CSpinListWnd_SelectNext @ 0x64b910,
	// CSpinListWnd_SelectPrevious @ 0x64b9a0].
	void spin_cycle(int id, int delta);
	// One parsed ACTION row, run as the current screen's [orig:
	// CUIWidget_HandleScriptedAction @0x6497f0]. True when it did something:
	// SCREEN, WINDOW, URL and POP_SCREEN rows; the service verbs (FORM_POST,
	// GLB_*, APPMSG, LAN_*, MNX) are the embedder's, and code 0, GLB_FILTER,
	// GLB_FILTER_NUM and TAB do nothing on activation: false. A cross-file
	// request the row makes is raised after it.
	bool dispatch_action(const mnu::Action &action);
	void play_widget_state_sound(int id, const std::string &state_token);
	void emit_edit_changed(int id);

	// ---- input ----
	// One raw-mouse sample in frame-local coordinates. The press edge is the
	// widget's WM_LBUTTONDOWN event [orig: UI_DispatchMouseEvent @ 0x63ab00]:
	// an edit takes the focus; a list, a table and a spin list's body are
	// activated and then pick (a row, or the spin's next value).
	void process_mouse(float x, float y, bool button_down, uint32_t now_ms);
	// One wheel tick (+1 rows-down, -1 rows-up); true when a target claimed it.
	bool process_wheel(float x, float y, int steps);
	// The frame reported a click on widget `index` (the release over the widget
	// it was pressed on): the pump's click, the SELECTED sound and the
	// activation, or a spin arrow's own click [orig: CWnd_ProcessMouseEvent
	// @ 0x647a00 — the click event 0x3000001 after the child pump].
	void on_widget_clicked(int index);
	// One key press; true when consumed (a hotkey fired, or the focused widget
	// took it) [orig: UI_DispatchKeyboardEventToChildren @ 0x63ad10].
	bool handle_key(const MenuKeyInput &key);
	// A click focus: an edit that is not READONLY [orig:
	// CEditWnd_HandleInputEvent @ 0x661510, g_UIFocusWnd on 0x1000002].
	void focus_edit(int id);
	void close_active_combo_popup();
	bool is_combo_popup_open(int id) const { return open_combo_id_ == id; }
	// The keyboard focus [orig: g_UIFocusWnd @0x31C16D4], any widget a TAB
	// row names, an edit a click focused; -1 none.
	int focused_widget() const { return focus_id_; }
	// The open popup [orig: g_UIOpenPopupWnd], -1 none.
	int open_popup() const { return open_popup_id_; }
	// The current screen's hotkey table in registration order.
	const std::vector<MenuHotkeyRow> &hotkey_rows() const { return hotkeys_; }

private:
	void emit_(const MenuEvent &event) const;
	// Bumped by every open_document: activate() compares it, not the document
	// pointer, so an observer that frees and re-binds a document at the same
	// address cannot have the old widget's ACTION rows run against it.
	uint32_t open_generation_ = 0;
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
	bool table_multiselect_(int id) const;
	int table_column_count_(int id) const;
	// The records at `count` (menu_table.h): kept when it does not grow the
	// table, every one zeroed when it does.
	void table_resize_records_(int id, int count);
	void push_table_columns_(int id);
	void emit_value_changed_for_(int id, int row);
	void on_claim_changed_(int previous, int current);
	// The parent window's id, -1 for a root (or an unknown id).
	int parent_window_(int id) const;
	// [orig: CWnd_FindChildByName @ 0x646850]
	int find_child_by_name_(int id, const std::string &name) const;
	// [orig: CWnd_IsVisibleInHierarchy @ 0x646290]
	bool visible_in_hierarchy_(int id) const;
	// Shown up the whole chain (what the draw walk reaches).
	bool drawn_(int id) const;
	// The popup a draw claims: the last shown MODAL generic window in draw order
	// [orig: CUIElement_Draw @ 0x64a8a0 makes a drawn MODAL window the popup];
	// else the one an action left open. Pushed to the frame.
	void sync_popup_();
	void set_popup_(int id);
	// The ACTION walk of one window for the click event [orig:
	// CUIWidget_HandleScriptedAction @0x6497f0], run as `owner_id`'s; false
	// when the document was swapped under it.
	bool walk_rows_(const mnu::Window &w, int owner_id, bool named, uint32_t generation);
	void run_row_(int owner_id, const mnu::Action &action, bool &handled);
	// Raise the held cross-file requests in order; an observer may swap the
	// document under each, and each carries what it needs.
	void raise_pending_requests_();
	bool window_row_(int owner_id, const mnu::Action &action);
	void set_interactive_recursive_(int id, bool enabled);
	void url_row_(const mnu::Action &action);
	void play_sound_(const mnu::Window &w, const std::string &state_token);
	// The press (WM_LBUTTONDOWN) of the widget at `index`.
	void press_(int index, float x, float y, uint32_t now_ms);
	void list_press_(int id, int row, uint32_t now_ms);
	void table_press_(int id, int row, int column, uint32_t now_ms);
	bool register_click_(int id, int row, uint32_t now_ms);
	// A spin arrow's click: its own SELECTED sound and ACTION rows, then the spin
	// list's step [orig: CSpinListWnd_HandleEvent @ 0x64c370 on
	// SPINLISTWND_UP / SPINLISTWND_DOWN].
	void arrow_click_(int id, int arrow);
	void open_combo_popup_(int id);
	// Focus moves [orig: g_UIFocusWnd]: the edit losing it keeps the text it
	// was typed (the store) without a commit event.
	void set_focus_(int id);
	void drop_focus_();
	void commit_edit_(int id);
	// The key goes to the focused widget once per root window of the screen
	// [orig: the broadcast in UI_DispatchKeyboardEventToChildren @ 0x63ad10
	// calls every root's UI_EmitEventToFocusWnd @ 0x6461f0].
	size_t current_root_count_() const;
	void key_down_to_focus_(const MenuKeyInput &key);
	void char_to_focus_(int unicode);
	void edit_filter_rows_(int id);
	void tab_rows_(int id);
	// The hotkey scan [orig: UI_DispatchKeyboardEventToChildren @ 0x63ad10,
	// dispatch_key_event @ 0x63ac30]; true when a row fired.
	bool scan_hotkeys_(bool virtual_key, int key);
	void build_hotkeys_();

	struct WidgetInfo {
		std::string screen;
		std::string name;
		int kind = -1;
	};

	MenuFrameSeam *frame_ = nullptr;
	MenuEventSink sink_;
	MenuDocIndex index_;
	std::string menu_file_;
	std::string game_code_; // set_game_code
	std::string current_screen_;
	std::vector<std::string> nav_stack_;
	std::unordered_map<std::string, int> screen_ids_; // upper name -> the LAST screen id
	std::vector<std::string> screen_order_;
	std::unordered_map<int, WidgetInfo> id_info_;
	std::vector<MenuHotkeyRow> hotkeys_;
	// The cross-file requests the rows made (MenuRequested, PopRequested), in walk
	// order, raised after the activation's callbacks.
	std::vector<MenuEvent> pending_requests_;
	std::unordered_map<int, MenuWidgetRuntimeState> id_state_;
	MenuWidgetRuntimeState scratch_state_;
	std::vector<int> id_of_index_;
	std::unordered_map<int, int> index_of_id_;

	// keyboard focus [orig: g_UIFocusWnd @0x31C16D4]
	int focus_id_ = -1;
	// single open dropdown [orig: g_UIActiveComboWnd @0x31C16D0]
	int open_combo_id_ = -1;
	// the open popup [orig: g_UIOpenPopupWnd], and the index the frame has
	int open_popup_id_ = -1;
	int frame_popup_index_ = -1;
	int last_claim_ = -1;
	float last_mouse_x_ = 0.0f, last_mouse_y_ = 0.0f;
	bool mouse_down_ = false;
	int last_click_id_ = -1;
	int last_click_row_ = -1;
	uint32_t last_click_ms_ = 0;
};

} // namespace opennova::menu
