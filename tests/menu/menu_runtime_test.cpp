// The compiled-menu interaction runtime (menu/menu_runtime.h), pinned where it
// used to live in the GDScript MenuDriver / MenuInputDispatch pair (ADR 0040
// ladder E5): the positional id tree, the first-match name seam, navigation
// and the back stack, the state store's authored fallbacks and its replay,
// ACTION dispatch (and the skipped dispatch when an observer swaps the
// document), radio groups, the spin wrap, the exclusive combo popup, the
// hover edges, the double-click latch, CTRL multi-select, edit focus and the
// key routing.
// [orig: CUIWidget_HandleScriptedAction @0x6497f0; UI_DispatchMouseEvent
//  @0x63ab00; CWnd_ProcessMouseEvent @0x647a00;
//  CEditWnd_HandleInputEvent @0x661510]
#include <runtime/menu/menu_edit.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/options_screen.h>
#include <base/gameprofile/game_type.h>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

struct FakeFrame : MenuFrameSeam {
	std::vector<std::string> log;
	bool configured = true;
	std::map<int, std::string> texts;
	std::map<int, int> carets;
	std::map<int, int> counts;
	std::set<int> disabled;
	int claim = -1;
	bool popup_mouse = false;
	bool wheel = true;
	int popup_row = -1;
	bool popup_contains = false;
	int list_row = -1;
	int spin_arrow = 0;
	int table_row = -1;
	int table_col = -1;
	std::vector<MenuTableRow> table_rows_seen;
	std::vector<MenuTableColumnDef> table_columns_seen;
	int hotkey = -1;
	std::string hotkey_asked;
	int edit_key_result = 0;
	int last_edit_key = 0;
	bool edit_char_result = true;
	MenuRectF rect{ 10.0f, 20.0f, 100.0f, 40.0f };

	void note(const std::string &s) { log.push_back(s); }
	bool saw(const std::string &s) const {
		for (const std::string &l : log)
			if (l == s) return true;
		return false;
	}

	bool is_configured() const override { return configured; }
	void configure_screen(const std::string &screen) override { note("configure " + screen); }
	void screen_configured() override { note("configured"); }
	void set_widget_shown_override(int i, bool v) override {
		note("shown " + std::to_string(i) + (v ? " 1" : " 0"));
	}
	void set_widget_disabled(int i, bool v) override {
		note("disabled " + std::to_string(i) + (v ? " 1" : " 0"));
		if (v)
			disabled.insert(i);
		else
			disabled.erase(i);
	}
	void set_widget_checked(int i, bool v) override {
		note("checked " + std::to_string(i) + (v ? " 1" : " 0"));
	}
	void set_widget_text(int i, const std::string &t) override {
		note("text " + std::to_string(i) + " " + t);
		texts[i] = t;
	}
	void set_widget_items(int i, const std::vector<std::string> &items) override {
		note("items " + std::to_string(i) + " " + std::to_string(items.size()));
		counts[i] = static_cast<int>(items.size());
	}
	void set_widget_selection(int i, int sel, int hover, int scroll) override {
		note("selection " + std::to_string(i) + " " + std::to_string(sel) + " " +
				std::to_string(hover) + " " + std::to_string(scroll));
	}
	void set_widget_scroll_range(int i, int mn, int mx, int page, int value) override {
		note("range " + std::to_string(i) + " " + std::to_string(mn) + " " + std::to_string(mx) +
				" " + std::to_string(page) + " " + std::to_string(value));
	}
	void set_widget_selected_set(int i, const std::vector<int> &rows) override {
		std::string s = "set " + std::to_string(i);
		for (int r : rows) s += " " + std::to_string(r);
		note(s);
	}
	void set_widget_table_rows(int i, const std::vector<MenuTableRow> &rows) override {
		note("rows " + std::to_string(i) + " " + std::to_string(rows.size()));
		table_rows_seen = rows;
	}
	void set_widget_table_columns(int i, const std::vector<MenuTableColumnDef> &columns) override {
		note("columns " + std::to_string(i) + " " + std::to_string(columns.size()));
		table_columns_seen = columns;
	}
	void set_widget_clip_rect(int i, bool enabled, int l, int t, int r, int b) override {
		note("clip " + std::to_string(i) + (enabled ? " 1 " : " 0 ") + std::to_string(l) + " " +
				std::to_string(t) + " " + std::to_string(r) + " " + std::to_string(b));
	}
	void set_widget_hover_item(int i, int row) override {
		note("hover " + std::to_string(i) + " " + std::to_string(row));
	}
	void set_widget_popup_open(int i, bool open) override {
		note("popup " + std::to_string(i) + (open ? " 1" : " 0"));
	}
	void set_widget_focused(int i, bool f) override {
		note("focused " + std::to_string(i) + (f ? " 1" : " 0"));
	}
	void set_widget_rect(int i, int l, int t, int r, int b) override {
		note("rect " + std::to_string(i) + " " + std::to_string(l) + " " + std::to_string(t) +
				" " + std::to_string(r) + " " + std::to_string(b));
	}
	void set_widget_caret(int i, int caret) override {
		note("caret " + std::to_string(i) + " " + std::to_string(caret));
		carets[i] = caret;
	}
	int get_widget_caret(int i) const override {
		const auto it = carets.find(i);
		return it != carets.end() ? it->second : -1;
	}
	std::string get_widget_text(int i) const override {
		const auto it = texts.find(i);
		return it != texts.end() ? it->second : std::string();
	}
	int item_count(int i) const override {
		const auto it = counts.find(i);
		return it != counts.end() ? it->second : 0;
	}
	bool is_widget_disabled(int i) const override { return disabled.count(i) != 0; }
	MenuRectF widget_rect(int) const override { return rect; }
	void design_scale(float &sx, float &sy) const override {
		sx = 2.0f;
		sy = 2.0f;
	}
	int process_mouse(float, float, bool) override { return claim; }
	bool process_popup_mouse(int, float, float, bool) override { return popup_mouse; }
	bool process_mouse_wheel(float, float, int) override { return wheel; }
	void set_cursor_state(bool, float, float) override {}
	void apply_claim_cursor() override { note("cursor"); }
	void reset_cursor() override { note("reset_cursor"); }
	int combo_popup_row_at(int, float, float) const override { return popup_row; }
	bool combo_popup_contains(int, float, float) const override { return popup_contains; }
	int list_row_at(int, float, float) const override { return list_row; }
	int spin_arrow_at(int, float, float) const override { return spin_arrow; }
	bool table_hit(int, float, float, int *row, int *column) const override {
		*row = table_row;
		*column = table_col;
		return true;
	}
	std::string item_display_text(int i, int row) const override {
		return "display " + std::to_string(i) + " " + std::to_string(row);
	}
	int hotkey_widget(const std::string &key, bool) const override {
		const_cast<FakeFrame *>(this)->hotkey_asked = key;
		return hotkey;
	}
	bool edit_char(int, int) override { return edit_char_result; }
	int edit_key(int, int key, bool) override {
		last_edit_key = key;
		return edit_key_result;
	}
};

mnu::Window widget(const char *name, mnu::WindowType type) {
	mnu::Window w;
	w.name = name;
	w.type = type;
	return w;
}

mnu::Action action(const char *type, const char *target, const char *state = "",
		const char *file = "", bool toggle = false) {
	mnu::Action a;
	a.type = type;
	a.target = target;
	a.state = state;
	a.file = file;
	a.toggle = toggle;
	return a;
}

mnu::Item item(const char *text, const char *value) {
	mnu::Item i;
	i.text = text;
	i.value = value;
	return i;
}

// MAIN: screen 1, ROOT 2, PLAY 3, PANEL 4, R1 5, R2 6, R3 7, COMBO 8, SPIN 9,
// NAME 10, RO 11, EXT 12, MULTI 13, TABLE 14, QUIT 15, VOL 16;
// OPTIONS: screen 17, ROOT 18, BACK 19, PLAY (duplicate name) 20.
mnu::Document make_document() {
	mnu::Document doc;
	mnu::Screen main;
	main.name = "MAIN";
	main.has_music_var = true;
	main.music_var = 3;
	main.root_window = widget("ROOT", mnu::WindowType::Window);
	mnu::Window play = widget("PLAY", mnu::WindowType::Button);
	play.string_data.value = "Play";
	play.actions = { action("WINDOW", "PANEL", "SHOW"), action("screen", "OPTIONS") };
	play.sounds = { mnu::Sound{ "selected", "CLICK_SELECT", "menu.lwf" },
		mnu::Sound{ "MOUSEIN", "MOUSE_OVER", "menu.lwf" },
		mnu::Sound{ "mouseout", "MOUSE_OUT", "menu.lwf" } };
	mnu::Window panel = widget("PANEL", mnu::WindowType::Window);
	panel.hidden = true;
	mnu::Window r1 = widget("R1", mnu::WindowType::Radio);
	r1.group = 1;
	r1.checked = true;
	mnu::Window r2 = widget("R2", mnu::WindowType::Radio);
	r2.group = 1;
	mnu::Window r3 = widget("R3", mnu::WindowType::Radio);
	r3.group = 2;
	r3.checked = true;
	mnu::Window combo = widget("COMBO", mnu::WindowType::Combo);
	combo.list_box.present = true;
	combo.list_box.items.present = true;
	combo.list_box.items.items = { item("a", "10"), item("b", "20"), item("c", "30") };
	combo.items.items = { item("ignored", "0") };
	mnu::Window spin = widget("SPIN", mnu::WindowType::SpinList);
	spin.items.items = { item("x", "1"), item("y", "2"), item("z", "3") };
	mnu::Window name = widget("NAME", mnu::WindowType::Edit);
	mnu::Window ro = widget("RO", mnu::WindowType::Edit);
	ro.readonly = true;
	mnu::Window ext = widget("EXT", mnu::WindowType::Button);
	ext.actions = { action("SCREEN", "X", "", "other.mnu") };
	mnu::Window multi = widget("MULTI", mnu::WindowType::Multi);
	multi.items.items = { item("m0", ""), item("m1", ""), item("m2", "") };
	mnu::Window table = widget("TABLE", mnu::WindowType::Table);
	table.items.multiselect = true;
	mnu::Window quit = widget("QUIT", mnu::WindowType::Button);
	quit.actions = { action("quit_game", "") };
	mnu::Window vol = widget("VOL", mnu::WindowType::Scroll);
	main.root_window.children = { play, panel, r1, r2, r3, combo, spin, name, ro, ext, multi,
		table, quit, vol };
	mnu::Screen options;
	options.name = "OPTIONS";
	options.root_window = widget("ROOT2", mnu::WindowType::Window);
	mnu::Window back = widget("BACK", mnu::WindowType::Button);
	back.actions = { action("POP_SCREEN", "") };
	options.root_window.children = { back, widget("play", mnu::WindowType::Button) };
	doc.screens = { main, options };
	return doc;
}

// The compiled frame answers the authored row counts of the MAIN screen's
// list-like widgets (COMBO 6, SPIN 7, MULTI 11).
void seed_counts(FakeFrame &frame) {
	frame.counts[6] = 3;
	frame.counts[7] = 3;
	frame.counts[11] = 3;
}

struct Recorder {
	std::vector<MenuEvent> events;
	int count(MenuEvent::Kind kind) const {
		int n = 0;
		for (const MenuEvent &e : events)
			if (e.kind == kind) ++n;
		return n;
	}
	const MenuEvent *last(MenuEvent::Kind kind) const {
		for (auto it = events.rbegin(); it != events.rend(); ++it)
			if (it->kind == kind) return &*it;
		return nullptr;
	}
};

void test_index_and_frameless() {
	const mnu::Document doc = make_document();
	MenuRuntime rt;
	Recorder rec;
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	CHECK(!rt.open_document(nullptr, "main.mnu", ""));
	CHECK(rt.open_document(&doc, "main.mnu", "nope"));
	CHECK(rt.current_screen() == "MAIN");
	CHECK(rt.index().node_count() == 20);
	CHECK(rt.index().screen_ids() == (std::vector<int>{ 1, 17 }));
	CHECK(rt.index().screen_root_id(17) == 18);
	// The tree the document binding reads through: a screen container has no
	// parent and one child (its root window); a root window's parent is its
	// screen; children keep authored order; a window id is not a screen.
	CHECK(rt.index().node(1)->parent_id == 0 && rt.index().node(1)->window == nullptr &&
			rt.index().node(1)->child_ids == (std::vector<int>{ 2 }));
	CHECK(rt.index().node(2)->parent_id == 1 && rt.index().node(3)->parent_id == 2 &&
			rt.index().node(2)->child_ids.size() == 14 && rt.index().node(2)->child_ids[1] == 4);
	CHECK(rt.index().screen(1) != nullptr && rt.index().screen(2) == nullptr &&
			rt.index().screen_root_id(2) == -1 && rt.index().node(0) == nullptr &&
			rt.index().node(21) == nullptr);
	// The name seam: case-insensitive, first match in document order.
	CHECK(rt.widget_id("play") == 3 && rt.widget_id("PLAY") == 3);
	CHECK(rt.widget_id("missing") == -1);
	CHECK(rt.widget_name_of(8) == "COMBO" && rt.widget_screen_of(19) == "OPTIONS");
	CHECK(rt.widget_kind_of(8) == static_cast<int>(mnu::WindowType::Combo));
	CHECK(rt.widget_kind_of(99) == -1);
	CHECK(rt.screen_names() == (std::vector<std::string>{ "MAIN", "OPTIONS" }));
	// The screen event pair: the change, then the MUSICVAR push.
	CHECK(rec.events.size() == 2 && rec.events[0].kind == MenuEvent::Kind::ScreenChanged &&
			rec.events[0].text == "MAIN" && rec.events[1].kind == MenuEvent::Kind::MusicVar &&
			rec.events[1].value == 3);
	// Frameless: every read takes its state-store / authored fallback.
	CHECK(rt.frame_index(3) == -1);
	CHECK(!rt.is_widget_shown(4) && rt.is_widget_shown(3) && rt.is_widget_shown(-1));
	rt.set_widget_shown(4, true);
	CHECK(rt.is_widget_shown(4));
	CHECK(rt.is_widget_checked(5) && !rt.is_widget_checked(6) && !rt.is_widget_disabled(3));
	CHECK(rt.get_widget_text(3) == "Play");
	rt.set_widget_text(3, "Go");
	CHECK(rt.get_widget_text(3) == "Go");
	// The combo's rows live in its authored LIST_BOX.
	CHECK(rt.item_count(8) == 3 && rt.item_text(8, 1) == "b" && rt.item_value(8, 2) == "30");
	CHECK(rt.item_text(8, 9).empty() && rt.item_count(3) == 0);
	CHECK(rt.selected_row(8) == 0 && rt.selected_row(3) == -1);
	rt.select_row_by_value(8, "20", false);
	CHECK(rt.selected_row(8) == 1);
	rt.select_row_by_value(8, "nope", false); // a miss selects row 0
	CHECK(rt.selected_row(8) == 0);
	rt.select_row_by_value(3, "x", false); // no item list: nothing
	CHECK(rt.selected_row(3) == -1);
	// Runtime rows replace the authored ones and reset the selection.
	rt.set_widget_items(8, { "q", "r" });
	CHECK(rt.item_count(8) == 2 && rt.item_text(8, 1) == "r" && rt.selected_row(8) == 0);
	CHECK(rt.get_widget_items(9) == (std::vector<std::string>{ "x", "y", "z" }));
	// A write to the absent widget never grows the store.
	rt.set_widget_text(-1, "ghost");
	CHECK(rt.get_widget_text(-1).empty());
}

void test_navigation_replay_and_actions() {
	const mnu::Document doc = make_document();
	FakeFrame frame;
	MenuRuntime rt;
	Recorder rec;
	rt.set_frame(&frame);
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	CHECK(rt.open_document(&doc, "MAIN.MNU", "main"));
	CHECK(frame.saw("configure MAIN") && frame.saw("configured"));
	CHECK(rt.frame_index(3) == 1 && rt.id_at_index(6) == 8 && rt.id_at_index(99) == -1);
	CHECK(rt.current_screen_ids().size() == 15);
	// A write to an off-screen widget lands in the store only...
	frame.log.clear();
	rt.set_widget_disabled(19, true);
	CHECK(frame.log.empty() && rt.is_widget_disabled(19));
	// ...and replays when its screen compiles.
	CHECK(rt.navigate_to_screen("OPTIONS"));
	CHECK(frame.saw("disabled 1 1"));
	CHECK(rec.last(MenuEvent::Kind::MusicVar)->value == 0); // unauthored MUSICVAR pushes 0
	// BACK pops to MAIN; popping past the root is the shell's quit.
	rt.activate(19);
	CHECK(rt.current_screen() == "MAIN"); // the authored spelling, whatever the caller used
	CHECK(!rt.pop_screen() && rec.count(MenuEvent::Kind::QuitRequested) == 1);

	// activate: the activation event first, then the ACTION rows in order.
	rec.events.clear();
	rt.activate(3);
	CHECK(rec.events.size() >= 3 && rec.events[0].kind == MenuEvent::Kind::WidgetActivated &&
			rec.events[0].id == 3 && rec.events[0].text == "PLAY");
	CHECK(rec.events[1].kind == MenuEvent::Kind::ShownChanged && rec.events[1].id == 4 &&
			rec.events[1].flag);
	CHECK(rt.current_screen() == "OPTIONS" && rt.is_widget_shown(4));
	rt.pop_screen();

	// A cross-file SCREEN action is the shell's; QUIT and URL relay.
	rec.events.clear();
	rt.activate(12);
	CHECK(rec.last(MenuEvent::Kind::MenuRequested) != nullptr &&
			rec.last(MenuEvent::Kind::MenuRequested)->text == "other.mnu" &&
			rec.last(MenuEvent::Kind::MenuRequested)->text2 == "X");
	CHECK(rt.dispatch_action(action("URL", "http://x")) &&
			rec.last(MenuEvent::Kind::UrlRequested)->text == "http://x");
	CHECK(!rt.dispatch_action(action("FORM_POST", "X"))); // a service verb is not the runtime's
	// The same-file spelling (the menu's own filename, any case) navigates.
	CHECK(rt.dispatch_action(action("screen", "OPTIONS", "", "main.mnu")));
	CHECK(rt.current_screen() == "OPTIONS");
	// WINDOW actions address the CURRENT screen only.
	CHECK(!rt.handle_window_action("PANEL", "show", false));
	rt.pop_screen();
	CHECK(rt.handle_window_action("PANEL", "hide", false) && !rt.is_widget_shown(4));
	CHECK(rt.handle_window_action("PANEL", "hide", true) && rt.is_widget_shown(4));
	CHECK(rt.handle_window_action("PANEL", "toggle", false) && !rt.is_widget_shown(4));
	CHECK(rt.handle_window_action("PLAY", "disable", false) && rt.is_widget_disabled(3));
	CHECK(rt.handle_window_action("PLAY", "enable", true) && !rt.is_widget_disabled(3));
	CHECK(!rt.handle_window_action("PLAY", "explode", false));
	// TAB focuses a shown, enabled edit target.
	CHECK(rt.dispatch_action(action("TAB", "NAME")) && rt.focused_widget() == 10);

	// An observer that swaps the document under the activation skips the rows.
	const mnu::Document other = make_document();
	MenuRuntime rt2;
	int shown_events = 0;
	rt2.set_sink([&](const MenuEvent &e) {
		if (e.kind == MenuEvent::Kind::WidgetActivated) rt2.open_document(&other, "main.mnu", "");
		if (e.kind == MenuEvent::Kind::ShownChanged) ++shown_events;
	});
	rt2.open_document(&doc, "main.mnu", "");
	rt2.activate(3);
	CHECK(shown_events == 0 && rt2.document() == &other && rt2.current_screen() == "MAIN");
}

void test_radio_spin_tables_scroll() {
	const mnu::Document doc = make_document();
	FakeFrame frame;
	MenuRuntime rt;
	Recorder rec;
	seed_counts(frame);
	rt.set_frame(&frame);
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	rt.open_document(&doc, "main.mnu", "");
	// Radio exclusivity: same screen, same GROUP only.
	rt.select_radio(6);
	CHECK(rt.is_widget_checked(6) && !rt.is_widget_checked(5) && rt.is_widget_checked(7));
	// The spin cycle wraps both ways and relays the value.
	rt.spin_cycle(9, -1);
	CHECK(rt.selected_row(9) == 2);
	const MenuEvent *v = rec.last(MenuEvent::Kind::ValueChanged);
	CHECK(v != nullptr && v->text == "SPIN" && v->text2 == "spinlist" && v->value == 2 &&
			v->text3 == "z");
	rt.spin_cycle(9, 1);
	CHECK(rt.selected_row(9) == 0);
	// Tables: rows, removal re-indexing the selection, the additive toggle.
	rt.table_add_row(14, { "a", "b" });
	rt.table_add_row(14, { "c", "d" });
	rt.table_add_row(14, { "e", "f" });
	CHECK(rt.table_row_count(14) == 3 && rt.table_cell_text(14, 1, 1) == "d");
	CHECK(rt.table_cell_text(14, 9, 0).empty() && rt.table_cell_text(14, 0, 9).empty());
	rt.table_select_row(14, 0, false);
	rt.table_select_row(14, 2, true);
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0, 2 }));
	rt.table_remove_row(14, 1);
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0, 1 }) && rt.table_row_count(14) == 2);
	rt.table_select_row(14, 1, true); // toggles off
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0 }));
	rt.table_clear_rows(14);
	CHECK(rt.table_row_count(14) == 0 && rt.table_selected_rows(14).empty());
	// The standalone scroll range: normalization, the mirrored pump value.
	rt.set_widget_scroll_range(16, 9, 1, 2, 5);
	MenuScrollRangeState range;
	CHECK(rt.get_widget_scroll_range(16, range) && range.minimum == 0 && range.maximum == 0 &&
			range.value == 0);
	rt.set_widget_scroll_range(16, 0, 10, 2, 4);
	rec.events.clear();
	rt.on_frame_scroll_value(rt.frame_index(16), 4); // unchanged: nothing
	CHECK(rec.events.empty());
	rt.on_frame_scroll_value(rt.frame_index(16), 99);
	CHECK(rec.events.size() == 1 && rec.events[0].text == "VOL" && rec.events[0].text2 == "scroll" &&
			rec.events[0].value == 10 && rec.events[0].text3 == "10");
	// A list-like widget's scroll arrives as its scroll row.
	frame.log.clear();
	rt.on_frame_scroll_value(rt.frame_index(13), 2);
	CHECK(frame.saw("selection 11 -1 -1 2"));
	CHECK(!rt.get_widget_scroll_range(3, range));
}

// The CTableWnd row operations over the store: AddRow's landed index and its
// column-0 value, the COLUMN COUNT bound on SetCellText / SetCellValue,
// SetRowSelected's single-select clear and locked-row immunity, the colour
// override, RemoveRow(-1).
// [orig: CTableWnd_InsertRow @0x641c30; CTableWnd_SetCellText @0x63edf0;
//  CTableWnd_SetRowSelected @0x63f5f0; sub_640110 @0x640110;
//  CTableWnd_RemoveRow @0x641a40]
void test_table_row_operations() {
	std::vector<MenuTableRow> rows;
	CHECK(table_insert_row(rows, "x", 5, 0, -1) == 0);
	CHECK(table_insert_row(rows, "y", 6, 0x4u, 9) == 1); // past the end appends
	CHECK(table_insert_row(rows, "z", 7, 0, 0) == 0);    // inserted at the front
	CHECK(rows[0].cell(0) == "z" && rows[1].value(0) == 5 && rows[2].flags == 0x4u);
	CHECK(!table_set_cell_text(rows, 0, 3, 3, "out")); // the column count bounds
	CHECK(table_set_cell_text(rows, 0, 2, 3, "in") && rows[0].cell(2) == "in" &&
			rows[0].cell(1).empty());
	CHECK(!table_set_cell_value(rows, 5, 0, 3, 1) && table_set_cell_value(rows, 1, 1, 3, 9) &&
			table_cell_value(rows, 1, 1) == 9 && table_cell_value(rows, 1, 0) == 5);
	// Single select: the non-locked rows clear; a locked row keeps its state.
	rows[2].state = kTableRowLocked;
	CHECK(table_set_row_selected(rows, 0, true, false));
	CHECK(table_set_row_selected(rows, 1, true, false));
	CHECK(rows[0].state == kTableRowDefault && rows[1].state == kTableRowSelected &&
			rows[2].state == kTableRowLocked);
	CHECK(!table_click_select(rows, 2, false) && rows[2].state == kTableRowLocked);
	// Multiselect: kept, toggled.
	CHECK(table_set_row_selected(rows, 0, true, true) && rows[1].state == kTableRowSelected);
	CHECK(table_click_select(rows, 0, true) && rows[0].state == kTableRowDefault);
	// -1 writes every row, locked included.
	table_set_row_selected(rows, -1, true, false);
	CHECK(rows[2].state == kTableRowSelected);
	table_set_row_color(rows, 1, true, 0xFF00FF00u);
	CHECK((rows[1].flags & kTableRowFlagColor) != 0 && rows[1].color == 0xFF00FF00u);
	table_set_row_color(rows, -1, false, 0);
	CHECK((rows[1].flags & kTableRowFlagColor) == 0 && rows[1].color == 0xFF00FF00u);
	rows[1].flags |= 0x2u;
	CHECK(rows[1].hidden() && !rows[0].hidden());
	table_remove_row(rows, 1);
	CHECK(rows.size() == 2 && rows[1].cell(0) == "y");
	table_remove_row(rows, -1);
	CHECK(rows.empty());
}

void test_input() {
	const mnu::Document doc = make_document();
	FakeFrame frame;
	MenuRuntime rt;
	Recorder rec;
	seed_counts(frame);
	rt.set_frame(&frame);
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	rt.open_document(&doc, "main.mnu", "");

	// Hover edges: MOUSEIN entering, MOUSEOUT + MOUSEIN moving, nothing into a
	// disabled widget.
	frame.claim = 1;
	rt.process_mouse(5, 5, false);
	CHECK(rec.last(MenuEvent::Kind::Sound)->text2 == "MOUSE_OVER" &&
			rec.last(MenuEvent::Kind::HoverChanged)->id == 3 &&
			rec.last(MenuEvent::Kind::HoverChanged)->flag);
	rt.set_widget_disabled(12, true);
	rec.events.clear();
	frame.claim = 10;
	rt.process_mouse(5, 5, false);
	CHECK(rec.count(MenuEvent::Kind::HoverChanged) == 1 && !rec.events.back().flag &&
			rec.last(MenuEvent::Kind::Sound)->text2 == "MOUSE_OUT");
	frame.claim = -1;
	rt.process_mouse(5, 5, false);

	// A disabled widget's click does nothing; a button's fires SELECTED + activation.
	rec.events.clear();
	rt.on_widget_clicked(10, 0, false);
	CHECK(rec.events.empty());
	rt.on_widget_clicked(13, 0, false);
	CHECK(rec.count(MenuEvent::Kind::WidgetActivated) == 1 &&
			rec.count(MenuEvent::Kind::QuitRequested) == 1);

	// The single open dropdown: a click opens, a second combo click closes.
	frame.log.clear();
	rt.on_widget_clicked(6, 0, false);
	CHECK(rt.is_combo_popup_open(8) && frame.saw("popup 6 1"));
	rt.on_widget_clicked(6, 0, false);
	CHECK(!rt.is_combo_popup_open(8) && frame.saw("popup 6 0"));
	// While open it owns the mouse: a row press selects and closes...
	rt.on_widget_clicked(6, 0, false);
	frame.popup_row = 1;
	frame.claim = 3; // the main pump must NOT run
	rec.events.clear();
	rt.process_mouse(50, 50, true);
	CHECK(!rt.is_combo_popup_open(8) && rt.selected_row(8) == 1);
	CHECK(rec.last(MenuEvent::Kind::ValueChanged)->text2 == "combo" &&
			rec.last(MenuEvent::Kind::ValueChanged)->text3 == "b" &&
			rec.count(MenuEvent::Kind::HoverChanged) == 0);
	rt.process_mouse(50, 50, false);
	// ...a press on the closed cell (design rect 10,20 100x40 at scale 2) is dead...
	rt.on_widget_clicked(6, 0, false);
	frame.popup_row = -1;
	rt.process_mouse(40, 60, true);
	CHECK(rt.is_combo_popup_open(8));
	rt.process_mouse(40, 60, false);
	// ...and a press outside both dismisses.
	rt.process_mouse(900, 900, true);
	CHECK(!rt.is_combo_popup_open(8));
	rt.process_mouse(900, 900, false);
	// A screen switch closes it too.
	rt.on_widget_clicked(6, 0, false);
	rt.show_screen("OPTIONS");
	CHECK(!rt.is_combo_popup_open(8));
	rt.show_screen("MAIN");

	// The double-click latch: the second click of a row inside 400 ms
	// activates, and the third re-arms from scratch.
	frame.list_row = 2;
	rec.events.clear();
	rt.on_widget_clicked(11, 1000, false);
	rt.on_widget_clicked(11, 1300, false);
	CHECK(rec.count(MenuEvent::Kind::ListActivated) == 1 &&
			rec.last(MenuEvent::Kind::ListActivated)->id == 13 &&
			rec.last(MenuEvent::Kind::ListActivated)->value == 2);
	rt.on_widget_clicked(11, 1400, false);
	CHECK(rec.count(MenuEvent::Kind::ListActivated) == 1);
	rt.on_widget_clicked(11, 2000, false); // 600 ms later: not a double
	CHECK(rec.count(MenuEvent::Kind::ListActivated) == 1);
	// MULTI: a plain click replaces the set, CTRL toggles into it.
	frame.list_row = 0;
	rt.on_widget_clicked(11, 5000, false);
	CHECK(rt.selected_set(13) == (std::vector<int>{ 0 }));
	frame.list_row = 2;
	rt.on_widget_clicked(11, 6000, true);
	CHECK(rt.selected_set(13) == (std::vector<int>{ 0, 2 }));
	rt.on_widget_clicked(11, 7000, true);
	CHECK(rt.selected_set(13) == (std::vector<int>{ 0 }) && rt.selected_row(13) == 2);
	// TABLE: a MULTISELECT table toggles the pressed row with or without
	// CTRL, then raises the cell event with the row's new state and the
	// column's cell value [orig: CTableWnd_HandleNamedEvent @0x642400].
	rt.table_add_row(14, { "a" });
	rt.table_add_row(14, { "b" });
	rt.table_set_cell_value(14, 1, 0, 77);
	frame.table_row = 0;
	frame.table_col = 0;
	rt.on_widget_clicked(12, 8000, false);
	frame.table_row = 1;
	rt.on_widget_clicked(12, 9000, false);
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0, 1 }));
	{
		const MenuEvent *cell = rec.last(MenuEvent::Kind::TableCellClicked);
		CHECK(cell != nullptr && cell->id == 14 && cell->text == "TABLE" && cell->value == 1 &&
				cell->column == 0 && cell->state == kTableRowSelected && cell->cell_value == 77 &&
				!cell->flag);
	}
	rt.on_widget_clicked(12, 9100, false); // the same row again: toggled off, a double click
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0 }));
	{
		const MenuEvent *cell = rec.last(MenuEvent::Kind::TableCellClicked);
		CHECK(cell != nullptr && cell->value == 1 && cell->state == kTableRowDefault && cell->flag);
	}
	// SPINLIST arrows.
	frame.spin_arrow = 2;
	rt.on_widget_clicked(7, 0, false);
	CHECK(rt.selected_row(9) == 2);

	// Edit focus: read-only refuses; focus parks the caret after the text
	// (characters, not bytes); leaving persists the text and relays it.
	rt.on_widget_clicked(9, 0, false);
	CHECK(rt.focused_widget() == -1);
	frame.texts[8] = "h\xC3\xA9";
	frame.log.clear();
	rt.on_widget_clicked(8, 0, false);
	CHECK(rt.focused_widget() == 10 && frame.saw("focused 8 1") && frame.saw("caret 8 2"));
	MenuKeyInput key;
	key.key = MenuKeyInput::Key::Backspace;
	frame.edit_key_result = static_cast<int>(EditKeyResult::kChanged);
	rec.events.clear();
	CHECK(rt.handle_key(key, 0, false) && frame.last_edit_key == kEditKeyBackspace);
	CHECK(rec.last(MenuEvent::Kind::ValueChanged)->text == "NAME" &&
			rec.last(MenuEvent::Kind::ValueChanged)->text2 == "edit" &&
			rec.last(MenuEvent::Kind::ValueChanged)->value == -1);
	key = MenuKeyInput();
	key.unicode = 'a';
	CHECK(rt.handle_key(key, 0, false)); // a typed character is the edit's
	key = MenuKeyInput();
	key.key = MenuKeyInput::Key::Enter;
	frame.edit_key_result = static_cast<int>(EditKeyResult::kCommit);
	rec.events.clear();
	CHECK(rt.handle_key(key, 0, false) && rt.focused_widget() == -1 && frame.saw("focused 8 0"));
	// The commit reaches the edit's own callback (event 0x7000002).
	CHECK(rec.last(MenuEvent::Kind::EditCommitted) != nullptr &&
			rec.last(MenuEvent::Kind::EditCommitted)->id == 10 &&
			rec.last(MenuEvent::Kind::EditCommitted)->text == "NAME");
	// A moved widget keeps its rect across a screen round trip.
	frame.log.clear();
	rt.set_widget_rect(10, 5, 6, 205, 56);
	CHECK(frame.saw("rect 8 5 6 205 56"));
	// The committed text survives a screen round trip through the store.
	rt.show_screen("OPTIONS");
	CHECK(rt.get_widget_text(10) == "h\xC3\xA9");
	frame.log.clear();
	rt.show_screen("MAIN");
	CHECK(frame.saw("rect 8 5 6 205 56"));

	// Hotkeys: the virtual-key scan, then the character scan (with the
	// printable-keycode fallback); a disabled target consumes without firing;
	// an edit target takes focus.
	key = MenuKeyInput();
	key.key = MenuKeyInput::Key::Escape;
	frame.hotkey = -1;
	CHECK(!rt.handle_key(key, 0, false) && frame.hotkey_asked == "VK_ESCAPE");
	frame.hotkey = 10; // EXT, disabled above
	rec.events.clear();
	CHECK(rt.handle_key(key, 0, false) && rec.count(MenuEvent::Kind::WidgetActivated) == 0);
	frame.hotkey = 13;
	CHECK(rt.handle_key(key, 0, false) && rec.count(MenuEvent::Kind::WidgetActivated) == 1);
	key = MenuKeyInput();
	key.printable_keycode = '=';
	frame.hotkey = 8;
	CHECK(rt.handle_key(key, 0, false) && frame.hotkey_asked == "=" && rt.focused_widget() == 10);
	// An unconfigured frame takes no input.
	frame.configured = false;
	CHECK(!rt.handle_key(key, 0, false) && !rt.process_wheel(0, 0, 1));
}


mnu::Document flow_document(bool options = false) {
	mnu::Document doc;
	mnu::Screen screen;
	screen.name = "MAIN";
	screen.root_window = widget("ROOT", mnu::WindowType::Window);
	screen.root_window.children = {
		widget("IA_LIST", mnu::WindowType::List),
		widget("BRIEFING", mnu::WindowType::Static),
		widget("ACCEPT", mnu::WindowType::Button),
		widget("MISSION_LIST", mnu::WindowType::Multi),
		widget("SELECTED_MISSIONS", mnu::WindowType::Table),
		widget("START_GAME", mnu::WindowType::Button),
		widget("MAIN_WRAPPER", mnu::WindowType::Window),
		widget("OPTIONS_WRAPPER", mnu::WindowType::Window)};
	auto filter = widget("GAME_TYPE", mnu::WindowType::SpinList);
	filter.items.present = true;
	filter.items.items = {item("All", "255"), item("Team", "1")};
	screen.root_window.children.push_back(filter);
	auto country = widget("GAME_LOCATION", mnu::WindowType::SpinList);
	country.items.present = true;
	country.items.items = {item("CAN Canada", "0"), item("USA United States", "1"),
		item("USA second", "2")};
	screen.root_window.children.push_back(country);
	if (options) {
		screen.root_window.children.push_back(widget("CONTROL_MAPPING", mnu::WindowType::Table));
		screen.root_window.children.push_back(widget("KEYBOARD", mnu::WindowType::Radio));
		screen.root_window.children.push_back(widget("MOUSE", mnu::WindowType::Radio));
	}
	doc.screens.push_back(screen);
	return doc;
}

void test_shell_flow() {
	auto doc = flow_document();
	MenuRuntime menu;
	menu.open_document(&doc, "sp.mnu", "");
	MenuFlow flow;
	flow.set_mission_controls({"ia_list", "CA_MISSION_LIST"}, {"BRIEFING"}, {"ACCEPT"});
	const int list = menu.widget_id("IA_LIST"), accept = menu.widget_id("ACCEPT");
	const std::vector<MissionChoice> choices = {
		{"coop.bms", "Co-op title *", "Briefing", game_type::kCoop},
		{"dm.bms", "DM title", "Other", game_type::kDeathmatch}};
	flow.seed_missions(menu, list, choices);
	CHECK(menu.item_count(list) == 1 && menu.item_text(list, 0) == "Co-op title *");
	CHECK(menu.selected_row(list) == -1 && menu.is_widget_disabled(accept));
	CHECK(menu.get_widget_text(menu.widget_id("BRIEFING")).empty());
	flow.select_mission(menu, list, 0, "display title");
	CHECK(flow.selected_mission() == "coop.bms" && !menu.is_widget_disabled(accept));
	CHECK(menu.get_widget_text(menu.widget_id("BRIEFING")) == "Briefing");
	flow.seed_missions(menu, list, choices);
	CHECK(menu.selected_row(list) == -1 && menu.is_widget_disabled(accept));
	flow.clear_selected_mission();
	flow.activate_mission(list, 0);
	CHECK(flow.selected_mission() == "coop.bms");
	flow.clear_rows();
	flow.select_mission(menu, list, 0, "fallback.bms");
	CHECK(flow.selected_mission() == "fallback.bms");
	flow.seed_missions(menu, menu.widget_id("MISSION_LIST"), choices);
	CHECK(menu.item_count(menu.widget_id("MISSION_LIST")) == 2);

	using Pick = MenuFlow::ExpansionPick;
	CHECK(flow.request_expansion("", "old", true) == Pick::Ignored);
	CHECK(flow.request_expansion("old", "old", true) == Pick::Ignored);
	CHECK(flow.request_expansion("new", "old", false) == Pick::NeedsPackedRoot);
	CHECK(!flow.has_pending_expansion());
	CHECK(flow.request_expansion("new", "old", true) == Pick::Queued);
	CHECK(flow.has_pending_expansion());
	CHECK(flow.request_expansion("old", "old", true) == Pick::Ignored);
	CHECK(flow.take_expansion_reload() == "new");
	CHECK(!flow.has_pending_expansion() && flow.take_expansion_reload().empty());
	flow.request_expansion("first", "", true);
	flow.request_expansion("last", "", true);
	CHECK(flow.take_expansion_reload() == "last");
}

void test_host_dialog() {
	auto doc = flow_document();
	MenuRuntime menu;
	menu.open_document(&doc, "mp.mnu", "");
	HostDialog host;
	const std::vector<MissionChoice> rows = {
		{"sp.bms", "Training", "", game_type::kCoop},
		{"team.bms", "Team", "", game_type::kTeamDeathmatch},
		{"obj.bms", "Objective", "", game_type::kObjectiveCoop},
		{"dm.bms", "Deathmatch", "", game_type::kDeathmatch}};
	host.seed(menu, rows);
	const int list = menu.widget_id("MISSION_LIST"), table = menu.widget_id("SELECTED_MISSIONS");
	CHECK(menu.item_count(list) == 3 && menu.selected_row(list) == -1);
	CHECK(!host.can_start() && menu.is_widget_disabled(menu.widget_id("START_GAME")));
	menu.set_selected_set(list, {0, 2});
	host.add_selected(menu, [](const char *section, const char *, const char *) {
		CHECK(std::string(section) == "GateTypeAbbrev");
		return std::string("localized");
	});
	CHECK(host.selected_missions() == (std::vector<std::string>{"team.bms", "dm.bms"}));
	CHECK(menu.table_cell_text(table, 0, 1) == "localized");
	CHECK(menu.table_cell_text(table, 0, 2) == "1" && menu.table_cell_text(table, 1, 2) == "0");
	CHECK(menu.item_count(list) == 1 && host.can_start());
	CHECK(!menu.is_widget_disabled(menu.widget_id("START_GAME")));
	menu.select_row(menu.widget_id("GAME_TYPE"), 1, false);
	host.filter(menu);
	CHECK(menu.item_count(list) == 0);
	menu.table_select_row(table, 0, false);
	menu.table_select_row(table, 1, true);
	host.remove_selected(menu);
	CHECK(!host.can_start() && menu.table_row_count(table) == 0);
	CHECK(menu.item_count(list) == 1 && menu.item_text(list, 0) == "Team");
	const int country = menu.widget_id("GAME_LOCATION");
	HostDialog::select_location(menu, country, "usa");
	CHECK(menu.selected_row(country) == 1);
	HostDialog::select_location(menu, country, "unknown");
	CHECK(menu.selected_row(country) == 1);
	host.seed(menu, rows);
	CHECK(!host.can_start() && host.selected_missions().empty());
}

void test_options_screen() {
	auto doc = flow_document(true);
	MenuRuntime menu;
	menu.open_document(&doc, "options.mnu", "");
	controls::BindingSet bindings;
	OptionsScreen options;
	options.prepare(menu, bindings);
	const int table = menu.widget_id("CONTROL_MAPPING");
	CHECK(options.is_surface() && menu.table_row_count(table) > 40);
	CHECK(menu.table_cell_text(table, 0, 2) == "W or Up");
	options.arm(menu, bindings, table, 0);
	CHECK(menu.table_cell_text(table, 0, 2).empty());
	RemapInput input;
	input.kind = RemapInput::Kind::Key;
	input.vk = 'Y';
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::Consumed);
	CHECK(menu.table_cell_text(table, 0, 2).empty()); // key release keeps capture
	input.pressed = true;
	input.vk = 0; // unmapped device key
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::Consumed);
	input.vk = 0xDE; // rejected retail capture
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::Consumed);
	input.vk = 'Y';
	CHECK(options.consume(menu, bindings, input) ==
		(OptionsScreen::Consumed | OptionsScreen::PersistBindings));
	CHECK(menu.table_cell_text(table, 0, 2) == "Y or Up");
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::None);
	options.arm(menu, bindings, table, 0);
	input.escape = true;
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::Consumed);
	CHECK(menu.table_cell_text(table, 0, 2) == "Y or Up");
	options.arm(menu, bindings, table, 0);
	options.end_remap(menu, bindings, true); // screen change
	CHECK(menu.table_cell_text(table, 0, 2) == "Y or Up");
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::None);
	menu.table_select_row(table, 0, false);
	CHECK(options.activate(menu, bindings, "CLEAR_KEY") == OptionsScreen::PersistBindings);
	CHECK(menu.table_cell_text(table, 0, 2).empty());
	CHECK(options.activate(menu, bindings, "DEFAULTS") == OptionsScreen::PersistBindings);
	CHECK(menu.table_cell_text(table, 0, 2) == "W or Up");
	options.activate(menu, bindings, "MOUSE");
	options.arm(menu, bindings, table, 0);
	input = RemapInput();
	input.kind = RemapInput::Kind::Mouse;
	input.mouse_mask = controls::kMouseRight;
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::None); // release falls through
	input.pressed = true;
	CHECK(options.consume(menu, bindings, input) ==
		(OptionsScreen::Consumed | OptionsScreen::PersistBindings));
	CHECK(bindings.record(bindings.action_index_for_row(0))->mouse_mask == controls::kMouseRight);
	options.activate(menu, bindings, "JOYSTICK");
	options.arm(menu, bindings, table, 0);
	CHECK(options.consume(menu, bindings, input) == OptionsScreen::None);
	CHECK(options.activate(menu, bindings, "OPT_ACCEPT") == OptionsScreen::CommitPreview);
	CHECK(options.activate(menu, bindings, "OPT_CANCEL") == OptionsScreen::RestorePreview);
	OptionsScreen::show_ingame_main(menu);
	CHECK(menu.is_widget_shown(menu.widget_id("MAIN_WRAPPER")));
	CHECK(!menu.is_widget_shown(menu.widget_id("OPTIONS_WRAPPER")));

	auto non_options = flow_document();
	menu.open_document(&non_options, "sp.mnu", "");
	options.prepare(menu, bindings);
	CHECK(!options.is_surface());
	CHECK(options.activate(menu, bindings, "DEFAULTS") == OptionsScreen::None);
	CHECK(options.activate(menu, bindings, "OPT_CANCEL") == OptionsScreen::None);
	CHECK(bindings.record(bindings.action_index_for_row(0))->mouse_mask == controls::kMouseRight);
}

} // namespace

// A populate's column layout: the count first (below 1 refused; existing
// columns kept), then one init per column (out of range refused); the layout
// reaches the frame, is replayed when the screen is shown again, and bounds
// the cell writes like an authored COLUMN COUNT.
// [orig: resize_column_count @0x63f6c0; CTableWnd_InitRow @0x63f9c0;
//  StatScreen_PopulateStatResultsList @0x5622fa..0x56237a]
void test_table_runtime_columns() {
	const mnu::Document doc = make_document();
	FakeFrame frame;
	MenuRuntime rt;
	seed_counts(frame);
	rt.set_frame(&frame);
	rt.open_document(&doc, "main.mnu", "");
	CHECK(!rt.table_set_column_count(14, 0));
	CHECK(!rt.table_set_column_count(10, 3)); // not a table
	CHECK(rt.table_set_column_count(14, 4));
	CHECK(frame.table_columns_seen.size() == 4);
	CHECK(rt.table_init_column(14, 0, 150, "Name", -1, 0x20));
	CHECK(rt.table_init_column(14, 3, 75, "Score", -1, 0x20));
	CHECK(!rt.table_init_column(14, 4, 75, "Past", -1, 0x20));
	CHECK(frame.table_columns_seen.size() == 4 && frame.table_columns_seen[0].defined &&
			frame.table_columns_seen[0].width == 150 && frame.table_columns_seen[0].label == "Name" &&
			!frame.table_columns_seen[1].defined && frame.table_columns_seen[3].vjustify == 0x20);
	rt.table_add_row(14, { "ljim", "-", "0", "7" });
	rt.table_set_cell_text(14, 0, 3, "9");
	CHECK(rt.table_cell_text(14, 0, 3) == "9"); // column 3 exists in the runtime count
	frame.table_columns_seen.clear();
	CHECK(rt.navigate_to_screen("OPTIONS") && rt.navigate_to_screen("MAIN"));
	// Replayed with the rows when the screen shows again.
	CHECK(frame.table_columns_seen.size() == 4 && frame.table_columns_seen[3].label == "Score");
}

int main() {
	test_index_and_frameless();
	test_navigation_replay_and_actions();
	test_radio_spin_tables_scroll();
	test_table_row_operations();
	test_table_runtime_columns();
	test_input();
	test_shell_flow();
	test_host_dialog();
	test_options_screen();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_runtime_test OK\n");
	return 0;
}
