// The compiled-menu interaction runtime (menu/menu_runtime.h), pinned where it
// used to live in the GDScript MenuDriver / MenuInputDispatch pair (ADR 0040
// ladder E5) and ported to retail's dispatch by the 2026-09-23 grill (sets A2,
// B1, B3, C2, C3a): the positional id tree, the per-screen control lookup,
// navigation and the history, the state store's authored fallbacks and its
// replay, the activation (the class step, the ACTION rows last-authored first,
// the parent bubble, then the callbacks, then the cross-file requests), the WINDOW /
// URL / SCREEN / POP_SCREEN rows, the MODAL popup, radio groups among siblings,
// the spin arrows, the exclusive combo popup, the hover edges, the press-time
// list / table / spin picks with the double-click latch, the focus, and the key
// routing with the hotkey table.
// [orig: CUIWidget_HandleScriptedAction @0x6497f0; UI_DispatchMouseEvent
//  @0x63ab00; CWnd_ProcessMouseEvent @0x647a00;
//  CEditWnd_HandleInputEvent @0x661510; UI_DispatchKeyboardEventToChildren
//  @0x63ad10; CWnd_IsVisibleInHierarchy @0x646290]
#include <runtime/menu/menu_edit.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/options_screen.h>
#include <base/gameprofile/game_type.h>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
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
	std::map<int, std::vector<MenuTableRow>> table_rows;
	std::map<int, std::string> mnemonics;
	std::set<int> disabled;
	int claim = -1;
	bool scroll_owned = false;
	bool popup_mouse = false;
	bool wheel = true;
	int popup_row = -1;
	bool popup_contains = false;
	int list_row = -1;
	int spin_arrow = 0;
	bool table_ok = true;
	int table_row = -1;
	int table_column = -1;
	int popup_root = -1;
	std::vector<MenuTableColumn> table_columns_seen;
	int edit_key_result = 0;
	int last_edit_key = 0;
	int edit_chars = 0;
	bool edit_char_result = true;
	MenuRectF rect{ 10.0f, 20.0f, 100.0f, 40.0f };

	void note(const std::string &s) { log.push_back(s); }
	bool saw(const std::string &s) const {
		for (const std::string &l : log)
			if (l == s) return true;
		return false;
	}

	bool is_configured() const override { return configured; }
	void configure_screen(const std::string &screen) override {
		note("configure " + screen);
		popup_root = -1;
	}
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
		table_rows[i] = rows;
	}
	void set_widget_table_columns(int i, bool installed, const std::vector<MenuTableColumn> &columns,
			int sort_column) override {
		note("columns " + std::to_string(i) + (installed ? " 1 " : " 0 ") +
				std::to_string(columns.size()) + " " + std::to_string(sort_column));
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
	int process_mouse(float, float, bool, bool &owned) override {
		owned = scroll_owned;
		return claim;
	}
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
		*column = table_column;
		return table_ok;
	}
	std::string item_display_text(int i, int row) const override {
		return "display " + std::to_string(i) + " " + std::to_string(row);
	}
	std::string widget_mnemonic(int i) const override {
		const auto it = mnemonics.find(i);
		return it != mnemonics.end() ? it->second : std::string();
	}
	void set_open_popup(int index) override {
		popup_root = index;
		note("popup_root " + std::to_string(index));
	}
	bool edit_char(int i, int unicode) override {
		++edit_chars;
		texts[i] += static_cast<char>(unicode);
		return edit_char_result;
	}
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

mnu::Hotkey hotkey(const char *value, bool virtual_key) {
	mnu::Hotkey h;
	h.value = value;
	h.virtual_key = virtual_key;
	return h;
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
	mnu::Window main_root = widget("ROOT", mnu::WindowType::Window);
	mnu::Window play = widget("PLAY", mnu::WindowType::Button);
	play.string_data.value = "Play";
	play.actions = { action("WINDOW", "PANEL", "SHOW"), action("screen", "OPTIONS", "", "main.mnu") };
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
	mnu::Window &combo_list = combo.list_box.author(mnu::WindowType::List);
	combo_list.items.present = true;
	combo_list.items.items = { item("a", "10"), item("b", "20"), item("c", "30") };
	combo.items.items = { item("ignored", "0") };
	mnu::Window spin = widget("SPIN", mnu::WindowType::SpinList);
	spin.items.items = { item("x", "1"), item("y", "2"), item("z", "3") };
	mnu::Window &up = spin.spinup.author(mnu::WindowType::Button);
	up.sounds = { mnu::Sound{ "SELECTED", "UP_CLICK", "menu.lwf" } };
	spin.spindown.author(mnu::WindowType::Button);
	mnu::Window name = widget("NAME", mnu::WindowType::Edit);
	mnu::Window ro = widget("RO", mnu::WindowType::Edit);
	ro.readonly = true;
	mnu::Window ext = widget("EXT", mnu::WindowType::Button);
	ext.actions = { action("SCREEN", "X", "", "other.mnu") };
	// A LIST whose ITEMS are MULTISELECT keeps a selection set.
	mnu::Window multi = widget("MULTI", mnu::WindowType::List);
	multi.items.multiselect = true;
	multi.items.items = { item("m0", ""), item("m1", ""), item("m2", "") };
	mnu::Window table = widget("TABLE", mnu::WindowType::Table);
	table.items.multiselect = true;
	mnu::Window quit = widget("QUIT", mnu::WindowType::Button);
	quit.actions = { action("quit_game", "") };
	mnu::Window vol = widget("VOL", mnu::WindowType::Scroll);
	main_root.children = { play, panel, r1, r2, r3, combo, spin, name, ro, ext, multi,
		table, quit, vol };
	main.roots.push_back(main_root);
	mnu::Screen options;
	options.name = "OPTIONS";
	mnu::Window options_root = widget("ROOT2", mnu::WindowType::Window);
	mnu::Window back = widget("BACK", mnu::WindowType::Button);
	back.actions = { action("POP_SCREEN", "") };
	options_root.children = { back, widget("play", mnu::WindowType::Button) };
	options.roots.push_back(options_root);
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
	int first_index(MenuEvent::Kind kind) const {
		for (size_t i = 0; i < events.size(); ++i)
			if (events[i].kind == kind) return static_cast<int>(i);
		return -1;
	}
};

MenuKeyInput vk(int code) {
	MenuKeyInput k;
	k.vk = code;
	return k;
}

MenuKeyInput typed(int ch) {
	MenuKeyInput k;
	k.unicode = ch;
	return k;
}

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
	// The name seam: the current screen's control first (case-insensitive), else
	// the first screen holding one.
	CHECK(rt.widget_id("play") == 3 && rt.widget_id("PLAY") == 3);
	CHECK(rt.widget_id("BACK") == 19); // off-screen: the OPTIONS screen's
	CHECK(rt.widget_id("missing") == -1);
	CHECK(rt.find_control("OPTIONS", "PLAY") == 20 && rt.find_control("", "BACK") == -1);
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

void test_navigation_and_replay() {
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
	// An unknown screen changes nothing and pushes nothing.
	CHECK(!rt.navigate_to_screen("NOWHERE") && rt.current_screen() == "OPTIONS");
	CHECK(rt.pop_screen() && rt.current_screen() == "MAIN");
	// The select pushes even the screen it leaves for itself.
	CHECK(rt.navigate_to_screen("MAIN") && rt.current_screen() == "MAIN");
	CHECK(rt.pop_screen() && rt.current_screen() == "MAIN");
	// POP_SCREEN past the file's own history is the embedder's cross-file pop.
	CHECK(!rt.pop_screen() && rec.count(MenuEvent::Kind::PopRequested) == 1);
	CHECK(rt.current_screen() == "MAIN");
}

// The activation [orig: CWnd_EmitEventToNamedHandlerAndCallbacks @ 0x646970;
// CUIWidget_HandleScriptedAction @ 0x6497f0].
void test_activation_order() {
	const mnu::Document doc = make_document();
	FakeFrame frame;
	MenuRuntime rt;
	Recorder rec;
	rt.set_frame(&frame);
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	rt.open_document(&doc, "main.mnu", "");
	// PLAY authors WINDOW SHOW PANEL, then SCREEN OPTIONS (its own file): the rows
	// run last-authored first (the screen changes, then the WINDOW row still finds
	// PANEL on PLAY's own screen), and the callbacks come after the rows.
	rt.activate(3);
	const int changed = rec.first_index(MenuEvent::Kind::ScreenChanged);
	const int shown = rec.first_index(MenuEvent::Kind::ShownChanged);
	const int activated = rec.first_index(MenuEvent::Kind::WidgetActivated);
	CHECK(changed >= 0 && shown > changed && activated > shown);
	CHECK(rec.events[static_cast<size_t>(activated)].id == 3 &&
			rec.events[static_cast<size_t>(activated)].text == "PLAY");
	CHECK(rt.current_screen() == "OPTIONS" && rt.is_widget_shown(4));
	rt.pop_screen();

	// A cross-file SCREEN row is raised after the callbacks, so they read the
	// screen they fired on.
	rec.events.clear();
	rt.activate(12);
	const int requested = rec.first_index(MenuEvent::Kind::MenuRequested);
	CHECK(rec.first_index(MenuEvent::Kind::WidgetActivated) >= 0 &&
			requested > rec.first_index(MenuEvent::Kind::WidgetActivated));
	CHECK(requested >= 0 && rec.events[static_cast<size_t>(requested)].text == "other.mnu" &&
			rec.events[static_cast<size_t>(requested)].text2 == "X");
	// An unknown TYPE (quit_game) is code 0: nothing; the callbacks still fire.
	rec.events.clear();
	rt.activate(15);
	CHECK(rec.count(MenuEvent::Kind::WidgetActivated) == 1 && rec.events.size() == 1);

	// An observer that swaps the document during the callbacks drops the jump.
	const mnu::Document other = make_document();
	MenuRuntime rt2;
	int requests = 0;
	rt2.set_sink([&](const MenuEvent &e) {
		if (e.kind == MenuEvent::Kind::WidgetActivated) rt2.open_document(&other, "main.mnu", "");
		if (e.kind == MenuEvent::Kind::MenuRequested) ++requests;
	});
	rt2.open_document(&doc, "main.mnu", "");
	rt2.activate(12);
	CHECK(requests == 0 && rt2.document() == &other && rt2.current_screen() == "MAIN");
	// A POP_SCREEN past the file's own history is held like a cross-file jump: the
	// earlier-authored rows and the callbacks run first, then the embedder's pop.
	mnu::Document pops = make_document();
	pops.screens[0].roots[0].children[0].actions = { action("WINDOW", "PANEL", "SHOW"),
		action("POP_SCREEN", "") };
	MenuRuntime rt3;
	Recorder rec3;
	bool panel_shown_at_pop = false;
	rt3.set_sink([&](const MenuEvent &e) {
		rec3.events.push_back(e);
		if (e.kind == MenuEvent::Kind::PopRequested) {
			panel_shown_at_pop = rt3.is_widget_shown(4);
			rt3.open_document(&other, "main.mnu", "");
		}
	});
	rt3.open_document(&pops, "main.mnu", "");
	rec3.events.clear();
	rt3.activate(3);
	const int popped = rec3.first_index(MenuEvent::Kind::PopRequested);
	CHECK(rec3.count(MenuEvent::Kind::WidgetActivated) == 1 && popped >= 0 &&
			popped > rec3.first_index(MenuEvent::Kind::WidgetActivated));
	CHECK(panel_shown_at_pop && rt3.document() == &other);
	// A row that swaps the document stops the walk: an observer reopening on the
	// in-place SCREEN row's screen change leaves PLAY's earlier-authored WINDOW row
	// and the callbacks unrun.
	MenuRuntime rt4;
	int activations = 0;
	bool swapped = false;
	rt4.set_sink([&](const MenuEvent &e) {
		if (e.kind == MenuEvent::Kind::ScreenChanged && e.text == "OPTIONS" && !swapped) {
			swapped = true;
			rt4.open_document(&other, "main.mnu", "");
		}
		if (e.kind == MenuEvent::Kind::WidgetActivated) ++activations;
	});
	rt4.open_document(&doc, "main.mnu", "");
	rt4.activate(3);
	CHECK(swapped && activations == 0 && rt4.document() == &other && !rt4.is_widget_shown(4));
	// The held requests keep the walk's order, and once one is held every later
	// SCREEN and POP_SCREEN row is held behind it (retail runs them on the screen
	// the held one leaves current): OPTIONS selects in place, then after the
	// callbacks the jump to other.mnu and the pop that returns from it.
	mnu::Document mixed = make_document();
	mixed.screens[0].roots[0].children[0].actions = { action("POP_SCREEN", ""),
		action("SCREEN", "X", "", "other.mnu"), action("SCREEN", "OPTIONS", "", "main.mnu") };
	MenuRuntime rt5;
	Recorder rec5;
	rt5.set_sink([&rec5](const MenuEvent &e) { rec5.events.push_back(e); });
	rt5.open_document(&mixed, "main.mnu", "");
	rec5.events.clear();
	rt5.activate(3);
	const int activated5 = rec5.first_index(MenuEvent::Kind::WidgetActivated);
	const int requested5 = rec5.first_index(MenuEvent::Kind::MenuRequested);
	CHECK(rt5.current_screen() == "OPTIONS" && activated5 >= 0 && requested5 > activated5 &&
			rec5.first_index(MenuEvent::Kind::PopRequested) > requested5);
}

// A document for the retail row rules. S: screen 1, ROOT 2, GO 3, TARGET 4,
// NAMELESS 5 (no NAME) > HIDDEN_KID 6, DLG 7 (MODAL, hidden) > DLG_OK 8, EDIT 9,
// BOX 10 > INNER 11; T: screen 12, ROOT_T 13, TARGET 14.
mnu::Document rows_document() {
	mnu::Document doc;
	mnu::Screen s;
	s.name = "S";
	mnu::Window root = widget("ROOT", mnu::WindowType::Window);
	mnu::Window go = widget("GO", mnu::WindowType::Button);
	mnu::Window target = widget("TARGET", mnu::WindowType::Button);
	mnu::Window nameless = widget("", mnu::WindowType::Window);
	nameless.children = { widget("HIDDEN_KID", mnu::WindowType::Button) };
	mnu::Window dlg = widget("DLG", mnu::WindowType::Window);
	dlg.modal = true;
	dlg.hidden = true;
	dlg.children = { widget("DLG_OK", mnu::WindowType::Button) };
	mnu::Window edit = widget("EDIT", mnu::WindowType::Edit);
	mnu::Window box = widget("BOX", mnu::WindowType::Window);
	box.children = { widget("INNER", mnu::WindowType::Button) };
	root.children = { go, target, nameless, dlg, edit, box };
	s.roots.push_back(root);
	mnu::Screen t;
	t.name = "T";
	mnu::Window root_t = widget("ROOT_T", mnu::WindowType::Window);
	root_t.children = { widget("TARGET", mnu::WindowType::Button) };
	t.roots.push_back(root_t);
	doc.screens = { s, t };
	return doc;
}

void test_rows() {
	const mnu::Document doc = rows_document();
	FakeFrame frame;
	MenuRuntime rt;
	Recorder rec;
	rt.set_frame(&frame);
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	CHECK(rt.open_document(&doc, "rows.mnu", "S"));
	// SCREEN: no FILE does nothing; the menu's own file selects in place.
	CHECK(!rt.dispatch_action(action("SCREEN", "T")) && rt.current_screen() == "S");
	CHECK(rt.dispatch_action(action("SCREEN", "T", "", "ROWS.MNU")) && rt.current_screen() == "T");
	// A WINDOW row finds its target on the screen it runs for: here the current.
	CHECK(rt.dispatch_action(action("WINDOW", "TARGET", "HIDE")) && !rt.is_widget_shown(14) &&
			rt.is_widget_shown(4));
	CHECK(rt.pop_screen() && rt.current_screen() == "S");
	// A nameless window ends its branch: its named child is unreachable.
	CHECK(rt.find_control("", "HIDDEN_KID") == -1 && rt.widget_id("HIDDEN_KID") == -1);
	CHECK(!rt.dispatch_action(action("WINDOW", "HIDDEN_KID", "HIDE")));
	// HIDE / SHOW, TOGGLE flips; another STATE does nothing.
	CHECK(rt.dispatch_action(action("WINDOW", "TARGET", "HIDE")) && !rt.is_widget_shown(4));
	CHECK(rt.dispatch_action(action("WINDOW", "TARGET", "HIDE", "", true)) && rt.is_widget_shown(4));
	CHECK(rt.dispatch_action(action("WINDOW", "TARGET", "SHOW", "", true)) && !rt.is_widget_shown(4));
	CHECK(!rt.dispatch_action(action("WINDOW", "TARGET", "toggle")) && !rt.is_widget_shown(4));
	rt.set_widget_shown(4, true);
	// ENABLE / DISABLE reach the whole subtree; TOGGLE goes to the opposite of
	// the target's own.
	CHECK(rt.dispatch_action(action("WINDOW", "BOX", "DISABLE")) && rt.is_widget_disabled(10) &&
			rt.is_widget_disabled(11));
	CHECK(rt.dispatch_action(action("WINDOW", "BOX", "ENABLE", "", true)) &&
			!rt.is_widget_disabled(10) && !rt.is_widget_disabled(11));
	// A MODAL target opens and closes the popup; the pump serves it alone.
	CHECK(rt.open_popup() == -1);
	CHECK(rt.dispatch_action(action("WINDOW", "DLG", "SHOW")) && rt.open_popup() == 7 &&
			frame.popup_root == rt.frame_index(7));
	CHECK(rt.dispatch_action(action("WINDOW", "DLG", "HIDE")) && rt.open_popup() == -1 &&
			frame.popup_root == -1);
	CHECK(rt.dispatch_action(action("WINDOW", "DLG", "SHOW", "", true)) && rt.open_popup() == 7);
	CHECK(rt.dispatch_action(action("WINDOW", "DLG", "SHOW", "", true)) && rt.open_popup() == -1);
	// A shown MODAL window claims the popup when it draws, whoever showed it; a
	// screen change closes it.
	rt.set_widget_shown(7, true);
	rt.handle_key(vk(0x7B)); // any input syncs the draw's claim
	CHECK(rt.open_popup() == 7);
	rt.show_screen("T");
	CHECK(rt.open_popup() == -1 && frame.popup_root == -1);
	rt.set_widget_shown(7, false);
	rt.show_screen("S");
	CHECK(rt.open_popup() == -1);
	// The focus drops for a WINDOW row.
	rt.focus_edit(9);
	CHECK(rt.focused_widget() == 9);
	rt.dispatch_action(action("WINDOW", "TARGET", "SHOW"));
	CHECK(rt.focused_widget() == -1);
	// URL: the row's text trimmed; EXTERNAL_BROWSER or the URL's own flag opens
	// the external browser; a FIELD slot reads that control's text (none: nothing).
	rec.events.clear();
	mnu::Action url = action("URL", "  www.novalogic.com/x \r\n");
	CHECK(rt.dispatch_action(url));
	CHECK(rec.last(MenuEvent::Kind::UrlRequested)->text == "www.novalogic.com/x" &&
			!rec.last(MenuEvent::Kind::UrlRequested)->flag);
	url.external_browser = true;
	rt.dispatch_action(url);
	CHECK(rec.last(MenuEvent::Kind::UrlRequested)->flag);
	rt.dispatch_action(action("URL", "a.com/?EXTERNAL_BROWSER=1"));
	CHECK(rec.last(MenuEvent::Kind::UrlRequested)->flag);
	frame.texts[rt.frame_index(9)] = " typed.example ";
	mnu::Action field = action("URL", "ignored");
	field.field = "EDIT";
	rt.dispatch_action(field);
	CHECK(rec.last(MenuEvent::Kind::UrlRequested)->text == "typed.example");
	field.field = "NO_SUCH";
	rec.events.clear();
	rt.dispatch_action(field);
	CHECK(rec.count(MenuEvent::Kind::UrlRequested) == 0);
	// Codes 0, 7, 8 and 11 do nothing on activation; service verbs are the embedder's.
	CHECK(!rt.dispatch_action(action("GLB_FILTER", "TARGET")) &&
			!rt.dispatch_action(action("TAB", "TARGET")) && !rt.dispatch_action(action("bogus", "")) &&
			!rt.dispatch_action(action("LAN_SEARCH", "TARGET")) &&
			!rt.dispatch_action(action("FORM_POST", "")));
	// The OpenNova-only tokens are gone: pop and quit are code 0.
	rec.events.clear();
	CHECK(!rt.dispatch_action(action("pop", "")) && !rt.dispatch_action(action("quit", "")) &&
			rec.count(MenuEvent::Kind::PopRequested) == 0);
}

// The parent bubble and a nameless widget [orig: @ 0x649805, @ 0x649c5d].
void test_bubble() {
	mnu::Document doc;
	mnu::Screen s;
	s.name = "S";
	mnu::Window root = widget("ROOT", mnu::WindowType::Window);
	mnu::Window twin = widget("TWIN", mnu::WindowType::Window);
	twin.actions = { action("WINDOW", "FLAG", "HIDE") };
	mnu::Window inner = widget("TWIN", mnu::WindowType::Button);
	mnu::Window nameless = widget("", mnu::WindowType::Button);
	nameless.actions = { action("WINDOW", "FLAG2", "HIDE") };
	twin.children = { inner };
	root.children = { twin, nameless, widget("FLAG", mnu::WindowType::Static),
		widget("FLAG2", mnu::WindowType::Static) };
	s.roots.push_back(root);
	doc.screens.push_back(s);
	MenuRuntime rt;
	Recorder rec;
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	rt.open_document(&doc, "b.mnu", "");
	// screen 1, ROOT 2, TWIN 3, TWIN (button) 4, nameless 5, FLAG 6, FLAG2 7.
	rt.activate(4);
	CHECK(!rt.is_widget_shown(6)); // the parent of the same NAME ran its row
	rt.activate(5);
	CHECK(rt.is_widget_shown(7) && rec.count(MenuEvent::Kind::WidgetActivated) == 1);
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
	// Radio exclusivity: the siblings of the same nonzero GROUP only.
	rt.select_radio(6);
	CHECK(rt.is_widget_checked(6) && !rt.is_widget_checked(5) && rt.is_widget_checked(7));
	// The spin step wraps both ways and relays the value.
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

// Press, then release over the claimed widget (the frame's click signal).
void click(MenuRuntime &rt, FakeFrame &frame, int index, uint32_t now) {
	frame.claim = index;
	rt.process_mouse(5, 5, true, now);
	rt.process_mouse(5, 5, false, now);
	rt.on_widget_clicked(index);
}

void test_mouse() {
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
	rt.process_mouse(5, 5, false, 0);
	CHECK(rec.last(MenuEvent::Kind::Sound)->text2 == "MOUSE_OVER" &&
			rec.last(MenuEvent::Kind::HoverChanged)->id == 3 &&
			rec.last(MenuEvent::Kind::HoverChanged)->flag);
	rt.set_widget_disabled(12, true);
	rec.events.clear();
	frame.claim = 10;
	rt.process_mouse(5, 5, false, 0);
	CHECK(rec.count(MenuEvent::Kind::HoverChanged) == 1 && !rec.events.back().flag &&
			rec.last(MenuEvent::Kind::Sound)->text2 == "MOUSE_OUT");
	frame.claim = -1;
	rt.process_mouse(5, 5, false, 0);

	// A disabled widget's click does nothing; any other fires SELECTED and the
	// activation.
	rec.events.clear();
	rt.on_widget_clicked(10);
	CHECK(rec.events.empty());
	rt.on_widget_clicked(13);
	CHECK(rec.count(MenuEvent::Kind::WidgetActivated) == 1);

	// The single open dropdown: a click opens, a second combo click closes.
	frame.log.clear();
	rt.on_widget_clicked(6);
	CHECK(rt.is_combo_popup_open(8) && frame.saw("popup 6 1"));
	rt.on_widget_clicked(6);
	CHECK(!rt.is_combo_popup_open(8) && frame.saw("popup 6 0"));
	// While open it owns the mouse: a row press selects and closes...
	rt.on_widget_clicked(6);
	frame.popup_row = 1;
	frame.claim = 3; // the main pump must NOT run
	rec.events.clear();
	rt.process_mouse(50, 50, true, 0);
	CHECK(!rt.is_combo_popup_open(8) && rt.selected_row(8) == 1);
	CHECK(rec.last(MenuEvent::Kind::ValueChanged)->text2 == "combo" &&
			rec.last(MenuEvent::Kind::ValueChanged)->text3 == "b" &&
			rec.count(MenuEvent::Kind::HoverChanged) == 0);
	rt.process_mouse(50, 50, false, 0);
	// ...a press on the closed cell (design rect 10,20 100x40 at scale 2) is dead...
	rt.on_widget_clicked(6);
	frame.popup_row = -1;
	rt.process_mouse(40, 60, true, 0);
	CHECK(rt.is_combo_popup_open(8));
	rt.process_mouse(40, 60, false, 0);
	// ...and a press outside both dismisses.
	rt.process_mouse(900, 900, true, 0);
	CHECK(!rt.is_combo_popup_open(8));
	rt.process_mouse(900, 900, false, 0);
	// A screen switch closes it too.
	rt.on_widget_clicked(6);
	rt.show_screen("OPTIONS");
	CHECK(!rt.is_combo_popup_open(8));
	rt.show_screen("MAIN");

	// A list picks on the press, activating first: the double-click latch fires
	// on the second press of a row inside 400 ms and re-arms from scratch.
	frame.list_row = 2;
	rec.events.clear();
	frame.claim = 11;
	rt.process_mouse(5, 5, true, 1000);
	CHECK(rec.first_index(MenuEvent::Kind::WidgetActivated) >= 0 &&
			rec.first_index(MenuEvent::Kind::WidgetActivated) < rec.first_index(MenuEvent::Kind::ValueChanged));
	rt.process_mouse(5, 5, false, 1000);
	rt.process_mouse(5, 5, true, 1300);
	CHECK(rec.count(MenuEvent::Kind::ListActivated) == 1 &&
			rec.last(MenuEvent::Kind::ListActivated)->id == 13 &&
			rec.last(MenuEvent::Kind::ListActivated)->value == 2);
	rt.process_mouse(5, 5, false, 1300);
	rt.process_mouse(5, 5, true, 1400);
	rt.process_mouse(5, 5, false, 1400);
	CHECK(rec.count(MenuEvent::Kind::ListActivated) == 1);
	// A press a scrollbar part took never reaches the list.
	rec.events.clear();
	frame.scroll_owned = true;
	rt.process_mouse(5, 5, true, 9000);
	CHECK(rec.count(MenuEvent::Kind::ValueChanged) == 0);
	rt.process_mouse(5, 5, false, 9000);
	frame.scroll_owned = false;
	// MULTISELECT toggles the pressed row in the set, no modifier needed.
	rt.set_selected_set(13, {});
	frame.list_row = 0;
	click(rt, frame, 11, 20000);
	CHECK(rt.selected_set(13) == (std::vector<int>{ 0 }));
	frame.list_row = 2;
	click(rt, frame, 11, 30000);
	CHECK(rt.selected_set(13) == (std::vector<int>{ 0, 2 }));
	// A row the toggle took out neither draws nor reads as selected: the list's
	// selected row is the first still in the set, none once it is empty [orig:
	// UIList_GetSelectedValue @ 0x644660], and the value change reports it.
	frame.log.clear();
	click(rt, frame, 11, 40000);
	CHECK(rt.selected_set(13) == (std::vector<int>{ 0 }) && rt.selected_row(13) == 0 &&
			frame.saw("selection 11 0 -1 0") && !frame.saw("selection 11 2 -1 0") &&
			rec.last(MenuEvent::Kind::ValueChanged)->value == 0);
	frame.list_row = 0;
	frame.log.clear();
	click(rt, frame, 11, 45000);
	CHECK(rt.selected_set(13).empty() && rt.selected_row(13) == -1 &&
			frame.saw("selection 11 -1 -1 0") && rec.last(MenuEvent::Kind::ValueChanged)->value == -1);
	// TABLE: the hit's row selects (MULTISELECT toggles), column -1 or not, and
	// the press raises the cell event with the row's new state and the column's
	// cell value [orig: CTableWnd_HandleNamedEvent @0x642400]; the header strip
	// (row -1) and a failed test pick nothing.
	rt.table_add_row(14, { "a" });
	rt.table_add_row(14, { "b" });
	rt.table_set_cell_value(14, 1, 0, 77);
	frame.table_row = 0;
	frame.table_column = -1;
	click(rt, frame, 12, 50000);
	frame.table_row = 1;
	frame.table_column = 0;
	click(rt, frame, 12, 60000);
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0, 1 }));
	{
		const MenuEvent *cell = rec.last(MenuEvent::Kind::TableCellClicked);
		CHECK(cell != nullptr && cell->id == 14 && cell->text == "TABLE" && cell->value == 1 &&
				cell->column == 0 && cell->state == kTableRowSelected && cell->cell_value == 77 &&
				!cell->flag);
	}
	click(rt, frame, 12, 60100); // the same row again: toggled off, a double click
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0 }));
	{
		const MenuEvent *cell = rec.last(MenuEvent::Kind::TableCellClicked);
		CHECK(cell != nullptr && cell->value == 1 && cell->state == kTableRowDefault && cell->flag);
	}
	frame.table_row = -1;
	frame.table_column = 0;
	click(rt, frame, 12, 70000);
	frame.table_ok = false;
	frame.table_row = 0;
	click(rt, frame, 12, 80000);
	CHECK(rt.table_selected_rows(14) == (std::vector<int>{ 0 }));
	frame.table_ok = true;

	// SPINLIST: a press on the list activates it and steps to the next value; an
	// arrow is its own button: UP steps next and DOWN previous on its click, with
	// the arrow's own SELECTED sound.
	rec.events.clear();
	frame.spin_arrow = 0;
	frame.claim = 7;
	rt.process_mouse(5, 5, true, 0);
	CHECK(rt.selected_row(9) == 1 && rec.count(MenuEvent::Kind::WidgetActivated) == 1);
	rt.process_mouse(5, 5, false, 0);
	frame.spin_arrow = 1;
	rt.process_mouse(5, 5, true, 0); // an arrow press: nothing yet
	CHECK(rt.selected_row(9) == 1);
	rt.process_mouse(5, 5, false, 0);
	rt.on_widget_clicked(7);
	CHECK(rt.selected_row(9) == 2 && rec.last(MenuEvent::Kind::Sound)->text2 == "UP_CLICK");
	frame.spin_arrow = 2;
	rt.on_widget_clicked(7);
	CHECK(rt.selected_row(9) == 1);
	frame.spin_arrow = 0;

	// Edit focus: a press focuses (read-only refuses); focus parks the caret after
	// the text (characters, not bytes).
	frame.claim = 9;
	rt.process_mouse(5, 5, true, 0);
	rt.process_mouse(5, 5, false, 0);
	CHECK(rt.focused_widget() == -1);
	frame.texts[8] = "h\xC3\xA9";
	frame.log.clear();
	frame.claim = 8;
	rt.process_mouse(5, 5, true, 0);
	rt.process_mouse(5, 5, false, 0);
	CHECK(rt.focused_widget() == 10 && frame.saw("focused 8 1") && frame.saw("caret 8 2"));
	// A moved widget keeps its rect across a screen round trip [orig: CWnd_SetRect
	// @0x646560], and the typed text survives it (the focus drops into the store).
	frame.log.clear();
	rt.set_widget_rect(10, 5, 6, 205, 56);
	CHECK(frame.saw("rect 8 5 6 205 56"));
	rt.show_screen("OPTIONS");
	CHECK(rt.focused_widget() == -1 && rt.get_widget_text(10) == "h\xC3\xA9");
	frame.log.clear();
	rt.show_screen("MAIN");
	CHECK(frame.saw("rect 8 5 6 205 56"));
	// An unconfigured frame takes no input.
	frame.configured = false;
	CHECK(!rt.handle_key(vk(27)) && !rt.process_wheel(0, 0, 1));
}

// The hotkey table and the key routing [orig: UI_DispatchKeyboardEventToChildren
// @ 0x63ad10; CWnd_RegisterHotkeysRecursive @ 0x649d90; CEditWnd_HandleKeyEvent
// @ 0x6623a0].
// K: screen 1, ROOT 2, HIDDEN_GROUP 3 (hidden) > HIDDEN_ESC 4, BACK 5, GO 6, SPACE 7,
// TAB_A 8 (radio), OFF_PANEL 9 (disabled) > OFF_BTN 10, NAME 11 (edit: TAB rows and
// a GLB filter), NEXT 12 (edit), MODAL_DLG 13 (MODAL, hidden) > DLG_OK 14, LIST 15.
mnu::Document key_document() {
	mnu::Document doc;
	mnu::Screen s;
	s.name = "K";
	mnu::Window root = widget("ROOT", mnu::WindowType::Window);
	mnu::Window hidden_group = widget("HIDDEN_GROUP", mnu::WindowType::Window);
	hidden_group.hidden = true;
	mnu::Window hidden_esc = widget("HIDDEN_ESC", mnu::WindowType::Button);
	hidden_esc.hotkeys = { hotkey("VK_ESCAPE", true) };
	hidden_group.children = { hidden_esc };
	mnu::Window back = widget("BACK", mnu::WindowType::Button);
	back.hotkeys = { hotkey("vk_escape", true), hotkey("Vx", false), hotkey("V", false) };
	mnu::Window go = widget("GO", mnu::WindowType::Button);
	go.hotkeys = { hotkey("VK_ENTER", true), hotkey("VK_RETURN ", true) };
	mnu::Window space = widget("SPACE", mnu::WindowType::Button);
	space.hotkeys = { hotkey("VK_SPACE", true) };
	mnu::Window tab = widget("TAB_A", mnu::WindowType::Radio);
	tab.group = 1;
	mnu::Window off = widget("OFF_PANEL", mnu::WindowType::Window);
	off.disabled = true;
	mnu::Window off_btn = widget("OFF_BTN", mnu::WindowType::Button);
	off_btn.hotkeys = { hotkey("q", false) };
	off.children = { off_btn };
	mnu::Window name = widget("NAME", mnu::WindowType::Edit);
	mnu::Action filter = action("GLB_FILTER_NUM", "LIST");
	filter.field = "3";
	filter.test = "GE";
	name.actions = { action("TAB", "NEXT"), action("TAB", "GO"), filter };
	mnu::Window next = widget("NEXT", mnu::WindowType::Edit);
	next.actions = { action("TAB", "BACK") };
	mnu::Window dlg = widget("MODAL_DLG", mnu::WindowType::Window);
	dlg.modal = true;
	dlg.hidden = true;
	mnu::Window dlg_ok = widget("DLG_OK", mnu::WindowType::Button);
	dlg_ok.hotkeys = { hotkey("VK_RETURN", true), hotkey("o", false) };
	dlg.children = { dlg_ok };
	root.children = { hidden_group, back, go, space, tab, off, name, next, dlg,
		widget("LIST", mnu::WindowType::List) };
	s.roots.push_back(root);
	doc.screens.push_back(s);
	return doc;
}

void test_keys() {
	const mnu::Document doc = key_document();
	FakeFrame frame;
	// Keyed by frame index: BACK 3 ("B{hot}ack"), SPACE 5 (a second 'v' row, later
	// in the table), TAB_A 6 ("{hot}Tab": a radio's STRING registers too).
	frame.mnemonics[3] = "a";
	frame.mnemonics[5] = "v";
	frame.mnemonics[6] = "t";
	MenuRuntime rt;
	Recorder rec;
	rt.set_frame(&frame);
	rt.set_sink([&rec](const MenuEvent &e) { rec.events.push_back(e); });
	CHECK(rt.open_document(&doc, "k.mnu", ""));
	// The table: pre-order, a window's HOTKEYs then its mnemonic; VK_ENTER and a
	// trailing space name no key; a (key, widget) pair registers once.
	const std::vector<MenuHotkeyRow> &rows = rt.hotkey_rows();
	std::vector<std::string> shape;
	for (const MenuHotkeyRow &row : rows)
		shape.push_back(std::to_string(row.id) + (row.virtual_key ? "v" : "c") + std::to_string(row.key));
	CHECK(shape == (std::vector<std::string>{ "4v27", "5v27", "5c86", "5c97", "7v32", "7c118", "8c116",
				"10c113", "14v13", "14c111" }));
	const auto activated = [&rec]() {
		const MenuEvent *e = rec.last(MenuEvent::Kind::WidgetActivated);
		return e != nullptr ? e->text : std::string();
	};
	// ESC: the hidden subtree's row is skipped, BACK's fires.
	rec.events.clear();
	CHECK(rt.handle_key(vk(27)) && activated() == "BACK");
	// VK_ENTER is dead: GO has no row; Enter goes unanswered.
	rec.events.clear();
	CHECK(!rt.handle_key(vk(13)) && rec.count(MenuEvent::Kind::WidgetActivated) == 0);
	// VK_SPACE fires; the typed ' ' matches no character row.
	rec.events.clear();
	MenuKeyInput space = vk(32);
	space.unicode = ' ';
	CHECK(rt.handle_key(space) && activated() == "SPACE");
	// A character row is its first character, both sides folded: 'v' finds BACK
	// (the "Vx" row) first.
	rec.events.clear();
	CHECK(rt.handle_key(typed('v')) && activated() == "BACK" &&
			rec.count(MenuEvent::Kind::WidgetActivated) == 1);
	// A radio's label mnemonic selects it.
	CHECK(rt.handle_key(typed('T')) && rt.is_widget_checked(8));
	// A row under a disabled window is skipped; then nothing: not consumed.
	rec.events.clear();
	CHECK(!rt.handle_key(typed('q')) && rec.count(MenuEvent::Kind::WidgetActivated) == 0);
	// A synthetic key with no character falls back to its printable keycode.
	MenuKeyInput printable;
	printable.printable_keycode = 'A';
	CHECK(rt.handle_key(printable) && activated() == "BACK");
	// A MODAL popup: only its subtree's rows fire.
	rt.set_widget_shown(13, true);
	rec.events.clear();
	CHECK(rt.handle_key(vk(13)) && activated() == "DLG_OK" && rt.open_popup() == 13);
	CHECK(!rt.handle_key(vk(27)) && !rt.handle_key(typed('v')));
	rt.dispatch_action(action("WINDOW", "MODAL_DLG", "HIDE"));
	CHECK(rt.open_popup() == -1);

	// The focus: while an edit has it the scan is skipped and ESC does nothing (the
	// edit took the key); typing goes to it and runs its GLB filter rows.
	rt.focus_edit(11);
	rec.events.clear();
	CHECK(rt.handle_key(vk(27)) && rec.count(MenuEvent::Kind::WidgetActivated) == 0 &&
			rt.focused_widget() == 11);
	CHECK(rt.handle_key(typed('7')) && frame.texts[rt.frame_index(11)] == "7");
	const MenuEvent *filter = rec.last(MenuEvent::Kind::FilterRequested);
	CHECK(filter != nullptr && filter->id == rt.widget_id("LIST") && filter->value == 3 &&
			filter->text == "7" && filter->flag && filter->text2 == "GE");
	// Tab runs its TAB rows: the first authored decides the focus.
	CHECK(rt.handle_key(vk(9)) && rt.focused_widget() == 12);
	// Enter in an edit commits, drops the focus, then scans VK_RETURN (no row
	// here: VK_ENTER is dead and DLG_OK's row sits under the hidden dialog).
	rec.events.clear();
	frame.edit_key_result = static_cast<int>(EditKeyResult::kCommit);
	CHECK(rt.handle_key(vk(13)) && frame.last_edit_key == kEditKeyEnter && rt.focused_widget() == -1);
	CHECK(rec.last(MenuEvent::Kind::ValueChanged) != nullptr &&
			rec.last(MenuEvent::Kind::ValueChanged)->text == "NEXT" &&
			rec.count(MenuEvent::Kind::WidgetActivated) == 0);
	// The commit reaches the edit's own callback (event 0x7000002).
	CHECK(rec.last(MenuEvent::Kind::EditCommitted) != nullptr &&
			rec.last(MenuEvent::Kind::EditCommitted)->id == 12 &&
			rec.last(MenuEvent::Kind::EditCommitted)->text == "NEXT");
	rt.set_widget_shown(13, true);
	rt.dispatch_action(action("WINDOW", "MODAL_DLG", "SHOW"));
	rt.focus_edit(12);
	rec.events.clear();
	CHECK(rt.handle_key(vk(13)) && activated() == "DLG_OK");
	// The keydown still runs the edit's GLB filter rows after the commit, and the
	// VK_RETURN click comes after them [orig: CEditWnd_HandleInputEvent
	// @ 0x661510 falls from the key handler into the filter walk; the scan's
	// winner clicks on the next pump].
	rt.focus_edit(11);
	rec.events.clear();
	CHECK(rt.handle_key(vk(13)) && activated() == "DLG_OK");
	const int committed = rec.first_index(MenuEvent::Kind::ValueChanged);
	const int commit_event = rec.first_index(MenuEvent::Kind::EditCommitted);
	const int filtered = rec.first_index(MenuEvent::Kind::FilterRequested);
	CHECK(committed >= 0 && commit_event > committed && filtered > commit_event &&
			filtered < rec.first_index(MenuEvent::Kind::WidgetActivated) &&
			rec.events[static_cast<size_t>(filtered)].id == rt.widget_id("LIST"));
	frame.edit_key_result = 0;
	rt.dispatch_action(action("WINDOW", "MODAL_DLG", "HIDE"));
	// Other edit keys change the text and relay it.
	rt.focus_edit(11);
	frame.edit_key_result = static_cast<int>(EditKeyResult::kChanged);
	rec.events.clear();
	CHECK(rt.handle_key(vk(8)) && frame.last_edit_key == kEditKeyBackspace &&
			rec.last(MenuEvent::Kind::ValueChanged)->text == "NAME");
	frame.edit_key_result = 0;
	// A TAB row may name any control: NEXT's names BACK. A button with the focus
	// takes the keys (nothing happens), so the scan stays off.
	rt.handle_key(vk(9));
	CHECK(rt.focused_widget() == 12);
	rt.handle_key(vk(9));
	CHECK(rt.focused_widget() == 5);
	rec.events.clear();
	CHECK(rt.handle_key(vk(27)) && rec.count(MenuEvent::Kind::WidgetActivated) == 0);
	CHECK(rt.handle_key(vk(9)) && rt.focused_widget() == 5); // BACK has no TAB rows
	// A WINDOW row drops it.
	rt.dispatch_action(action("WINDOW", "GO", "SHOW"));
	CHECK(rt.focused_widget() == -1);
}

// Every root window of the screen delivers the key to the focus [orig: the
// broadcast of UI_DispatchKeyboardEventToChildren @ 0x63ad10 calls each root's
// UI_EmitEventToFocusWnd @ 0x6461f0]: with two roots a typed character lands twice.
void test_two_root_keys() {
	mnu::Document doc;
	mnu::Screen s;
	s.name = "S";
	mnu::Window first = widget("FIRST", mnu::WindowType::Window);
	first.children = { widget("EDIT", mnu::WindowType::Edit) };
	s.roots = { first, widget("SECOND", mnu::WindowType::Window) };
	doc.screens.push_back(s);
	FakeFrame frame;
	MenuRuntime rt;
	rt.set_frame(&frame);
	rt.open_document(&doc, "two.mnu", "");
	rt.focus_edit(3);
	CHECK(rt.handle_key(typed('x')) && frame.edit_chars == 2);
}

// Duplicate screen names: the last is the one every lookup finds.
void test_duplicate_screens() {
	mnu::Document doc;
	mnu::Screen a;
	a.name = "DUP";
	mnu::Window ra = widget("RA", mnu::WindowType::Window);
	ra.children = { widget("ONLY_A", mnu::WindowType::Button) };
	a.roots.push_back(ra);
	mnu::Screen b;
	b.name = "dup";
	mnu::Window rb = widget("RB", mnu::WindowType::Window);
	rb.children = { widget("ONLY_B", mnu::WindowType::Button) };
	b.roots.push_back(rb);
	doc.screens = { a, b };
	FakeFrame frame;
	MenuRuntime rt;
	rt.set_frame(&frame);
	CHECK(rt.open_document(&doc, "d.mnu", "DUP"));
	CHECK(rt.current_screen() == "dup" && frame.saw("configure dup"));
	CHECK(rt.find_control("DUP", "ONLY_B") >= 0 && rt.find_control("DUP", "ONLY_A") == -1);
	CHECK(rt.widget_id("ONLY_B") >= 0 && rt.frame_index(rt.widget_id("ONLY_B")) >= 0);
	CHECK(rt.frame_index(rt.widget_id("ONLY_A")) == -1);
}


mnu::Document flow_document(bool options = false) {
	mnu::Document doc;
	mnu::Screen screen;
	screen.name = "MAIN";
	mnu::Window root = widget("ROOT", mnu::WindowType::Window);
	root.children = {
		widget("IA_LIST", mnu::WindowType::List),
		widget("BRIEFING", mnu::WindowType::Static),
		widget("ACCEPT", mnu::WindowType::Button),
		widget("MISSION_LIST", mnu::WindowType::List),
		widget("SELECTED_MISSIONS", mnu::WindowType::Table),
		widget("START_GAME", mnu::WindowType::Button),
		widget("MAIN_WRAPPER", mnu::WindowType::Window),
		widget("OPTIONS_WRAPPER", mnu::WindowType::Window)};
	// The host screen's rotation table authors three columns: the mission, its
	// mode, its Switch cell.
	root.children[4].table_data.column.has_count = true;
	root.children[4].table_data.column.count = 3;
	auto filter = widget("GAME_TYPE", mnu::WindowType::SpinList);
	filter.items.present = true;
	filter.items.items = {item("All", "255"), item("Team", "1")};
	root.children.push_back(filter);
	auto country = widget("GAME_LOCATION", mnu::WindowType::SpinList);
	country.items.present = true;
	country.items.items = {item("CAN Canada", "0"), item("USA United States", "1"),
		item("USA second", "2")};
	root.children.push_back(country);
	if (options) {
		root.children.push_back(widget("CONTROL_MAPPING", mnu::WindowType::Table));
		root.children.push_back(widget("KEYBOARD", mnu::WindowType::Radio));
		root.children.push_back(widget("MOUSE", mnu::WindowType::Radio));
	}
	screen.roots.push_back(root);
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
	CHECK(host.selected_launch_options() == (std::vector<int32_t>{1, 0}));
	// The Switch cell toggles a team, non-objective row's launch option and
	// redraws; a DM row keeps 0 [orig: HostDialog_SelectedMissionsTableEvent
	// @0x558061..0x5580AB].
	host.toggle_switch(menu, 0);
	host.toggle_switch(menu, 1);
	CHECK(menu.table_cell_text(table, 0, 2) == "0" && menu.table_cell_text(table, 1, 2) == "0");
	CHECK(host.selected_launch_options() == (std::vector<int32_t>{0, 0}));
	host.toggle_switch(menu, 0);
	CHECK(menu.table_cell_text(table, 0, 2) == "1");
	CHECK(host.selected_launch_options() == (std::vector<int32_t>{1, 0}));
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

// The shipped MISSION_LIST is a LIST whose ITEMS are MULTISELECT: a click keeps a
// selection set, ADD takes the picked mission, and the rebuilt rows drop the set (its
// indexes named the old rows), so a second ADD adds nothing and no row stays
// highlighted [orig: HostDialog_AddRemoveSelectedMissions @0x557c10 reads
// CListWnd_IsRowSelected @ 0x645150, false for a hidden row].
void test_host_dialog_multiselect() {
	auto doc = flow_document();
	for (mnu::Window &w : doc.screens[0].roots[0].children)
		if (w.name == "MISSION_LIST") w.items.multiselect = true;
	FakeFrame frame;
	MenuRuntime menu;
	menu.set_frame(&frame);
	menu.open_document(&doc, "mp.mnu", "");
	HostDialog host;
	host.seed(menu, {{"team.bms", "Team", "", game_type::kTeamDeathmatch},
			{"obj.bms", "Objective", "", game_type::kObjectiveCoop},
			{"dm.bms", "Deathmatch", "", game_type::kDeathmatch}});
	const int list = menu.widget_id("MISSION_LIST");
	const int index = menu.frame_index(list);
	CHECK(index >= 0 && menu.item_count(list) == 3);
	const auto key = [](const char *, const char *, const char *fallback) { return std::string(fallback); };
	frame.list_row = 0;
	frame.claim = index;
	menu.process_mouse(5, 5, true, 1000);
	menu.process_mouse(5, 5, false, 1000);
	CHECK(menu.selected_rows(list) == (std::vector<int>{ 0 }));
	frame.log.clear();
	host.add_selected(menu, key);
	CHECK(host.selected_missions() == (std::vector<std::string>{ "team.bms" }));
	CHECK(menu.item_count(list) == 2 && menu.selected_rows(list).empty());
	CHECK(frame.saw("set " + std::to_string(index))); // the frame's highlight cleared too
	host.add_selected(menu, key);
	CHECK(host.selected_missions() == (std::vector<std::string>{ "team.bms" }));
}

// A screen with several root windows: every root is indexed in document order, a
// control in a later root is found and mapped to its frame index, and a name both
// roots use finds the first.
void test_multiple_roots() {
	mnu::Document doc;
	mnu::Screen screen;
	screen.name = "MULTI";
	mnu::Window first = widget("FIRST", mnu::WindowType::Window);
	first.children = { widget("SHARED", mnu::WindowType::Button),
		widget("ONLY_FIRST", mnu::WindowType::Button) };
	mnu::Window second = widget("SECOND", mnu::WindowType::Window);
	second.children = { widget("SHARED", mnu::WindowType::Button),
		widget("ONLY_SECOND", mnu::WindowType::Static) };
	screen.roots = { first, second };
	doc.screens = { screen };
	FakeFrame frame;
	MenuRuntime rt;
	rt.set_frame(&frame);
	CHECK(rt.open_document(&doc, "multi.mnu", ""));
	// screen 1, FIRST 2, SHARED 3, ONLY_FIRST 4, SECOND 5, SHARED 6, ONLY_SECOND 7.
	CHECK(rt.index().node(1) != nullptr &&
			rt.index().node(1)->child_ids == (std::vector<int>{ 2, 5 }));
	CHECK(rt.index().screen_root_id(1) == 2 && rt.index().node(5)->parent_id == 1);
	CHECK(rt.widget_id("ONLY_SECOND") == 7 && rt.widget_screen_of(7) == "MULTI");
	CHECK(rt.widget_id("SHARED") == 3);
	CHECK(rt.current_screen_ids().size() == 6 && rt.frame_index(7) == 5 && rt.frame_index(5) == 3);
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

// A populate's column layout: the count first (below 1 refused), then one init
// per column (out of range refused); the layout reaches the frame, is replayed
// when the screen is shown again, and bounds the cell writes like an authored
// COLUMN COUNT. [orig: CTableWnd_ResizeColumnCount @0x63f6c0; CTableWnd_InitRow
// @0x63f9c0; StatScreen_PopulateStatResultsList @0x5622fa..0x56237a]
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

// The column records a count leaves [orig: CTableWnd_ResizeColumnCount @0x63f6c0]: one
// that does not grow the table keeps them (the authored columns, which an init then
// sets up in place, keeping their cell type, offsets and SUBST rows [orig:
// CTableWnd_InitRow @0x63f9c0]); one that grows it starts every record over, zeroed,
// the authored ones too (the grow path copies the old count in bytes @0x63f724).
void test_table_column_records() {
	mnu::Document doc = make_document();
	mnu::Window &authored = doc.screens[0].roots[0].children[11];
	CHECK(authored.name == "TABLE");
	authored.table_data.column.has_count = true;
	authored.table_data.column.count = 3;
	FakeFrame frame;
	MenuRuntime rt;
	seed_counts(frame);
	rt.set_frame(&frame);
	rt.open_document(&doc, "main.mnu", "");
	// An init over the authored count: the three records stand, the one set up in place.
	CHECK(rt.table_init_column(14, 1, 80, "Kills", -1, -1));
	const std::vector<MenuTableColumn> &seen = frame.table_columns_seen;
	CHECK(seen.size() == 3 && seen[0].kept && !seen[0].defined && seen[2].kept && !seen[2].defined);
	CHECK(seen.size() == 3 && seen[1].kept && seen[1].defined && seen[1].label == "Kills" &&
			seen[1].width == 80 && seen[1].justify == 1 && seen[1].vjustify == 16 &&
			seen[1].body_justify == 1 && seen[1].body_vjustify == 16 && seen[1].ascending &&
			!seen[1].numeric_sort);
	// A smaller count and the same one keep the records.
	CHECK(rt.table_set_column_count(14, 2));
	CHECK(seen.size() == 2 && seen[0].kept && seen[1].kept && seen[1].defined && seen[1].label == "Kills");
	CHECK(rt.table_set_column_count(14, 2));
	CHECK(seen.size() == 2 && seen[1].kept && seen[1].label == "Kills");
	// A larger one starts every record over: zero, the authored ones lost.
	CHECK(rt.table_set_column_count(14, 4));
	bool zeroed = seen.size() == 4;
	for (const MenuTableColumn &c : seen)
		zeroed = zeroed && !c.kept && !c.defined && c.label.empty() && c.width == 0 && c.justify == 0 &&
				c.vjustify == 0 && c.cell_type == 0 && !c.ascending;
	CHECK(zeroed);
	// An init over a started record defines it; the others stay zero.
	CHECK(rt.table_init_column(14, 3, 75, "Score", -1, 0x20));
	CHECK(seen.size() == 4 && !seen[3].kept && seen[3].defined && seen[3].label == "Score" &&
			seen[3].width == 75 && !seen[2].defined);

	// The stat fill's set-up is the same two steps: a set as wide as the authored
	// table sets the authored records up in place; a wider one starts them over. An
	// init reads its label, width, justification and sort compare, not the rest of
	// the entry (the cell type and the cell justification are the record's).
	std::vector<MenuTableColumn> set(3);
	for (MenuTableColumn &c : set) {
		c.width = 60;
		c.justify = -1;
		c.vjustify = 32;
		c.body_justify = 2;
		c.cell_type = 1;
	}
	set[2].numeric_sort = true;
	MenuRuntime same;
	FakeFrame same_frame;
	seed_counts(same_frame);
	same.set_frame(&same_frame);
	same.open_document(&doc, "main.mnu", "");
	same.table_set_columns(14, set);
	const std::vector<MenuTableColumn> &kept = same_frame.table_columns_seen;
	CHECK(kept.size() == 3 && kept[0].kept && kept[0].defined && kept[0].justify == 1 &&
			kept[0].vjustify == 32 && kept[0].body_justify == 1 && kept[0].body_vjustify == 32 &&
			kept[0].cell_type == 0 && kept[2].numeric_sort && !kept[1].numeric_sort);
	set.push_back(set[0]);
	MenuRuntime wider;
	FakeFrame wider_frame;
	seed_counts(wider_frame);
	wider.set_frame(&wider_frame);
	wider.open_document(&doc, "main.mnu", "");
	wider.table_set_columns(14, set);
	const std::vector<MenuTableColumn> &fresh = wider_frame.table_columns_seen;
	CHECK(fresh.size() == 4 && !fresh[0].kept && fresh[0].defined && fresh[3].defined &&
			fresh[0].width == 60 && fresh[0].cell_type == 0);
	// An empty set is a count below 1: nothing is set up.
	MenuRuntime none;
	FakeFrame none_frame;
	seed_counts(none_frame);
	none.set_frame(&none_frame);
	none.open_document(&doc, "main.mnu", "");
	none.table_set_columns(14, {});
	CHECK(none_frame.table_columns_seen.empty());
}

// The stat table's fill [orig: StatScreen_PopulateStatResultsList @ 0x562240]: installed
// columns, rows with a colour override and a selection, then the sort on a
// numeric column, descending [orig: CTableWnd_SortByColumn @ 0x640900 ->
// CTableWnd_CompareRows @ 0x63e9c0]: the rows move with their colours and selection,
// and the sorted column is the one showing the indicator.
void test_table_columns_and_sort() {
	mnu::Document doc = make_document();
	MenuRuntime rt;
	FakeFrame frame;
	rt.set_frame(&frame);
	rt.open_document(&doc, "main.mnu", "");
	const int table = rt.widget_id("TABLE");
	CHECK(table > 0);
	const int index = rt.frame_index(table);
	std::vector<MenuTableColumn> columns(3);
	columns[0].label = "Name";
	columns[0].width = 150;
	columns[1].label = "Squad";
	columns[1].width = 100;
	columns[2].label = "Kills";
	columns[2].width = 100;
	columns[2].numeric_sort = true;
	rt.table_set_columns(table, columns);
	CHECK(frame.saw("columns " + std::to_string(index) + " 1 3 -1"));
	rt.table_add_row(table, { "ann", "-", "5" });
	rt.table_add_row(table, { "bob", "-", "12" });
	rt.table_add_row(table, { "cat", "-", "x" });
	rt.table_add_row(table, { "dan", "-", "" });
	rt.table_set_row_color(table, 1, true, 0xFF00BFFFu);
	rt.table_select_row(table, 0, false);
	rt.table_set_column_ascending(table, 2, false);
	rt.table_sort_by_column(table, 2);
	// Descending numeric: "x" reads 0x7FFFFFFF, "" reads atol("") = 0.
	CHECK(rt.table_cell_text(table, 0, 0) == "cat");
	CHECK(rt.table_cell_text(table, 1, 0) == "bob");
	CHECK(rt.table_cell_text(table, 2, 0) == "ann");
	CHECK(rt.table_cell_text(table, 3, 0) == "dan");
	CHECK(rt.table_selected_rows(table) == std::vector<int>{ 2 });
	CHECK(rt.table_sort_column(table) == 2);
	CHECK(frame.saw("columns " + std::to_string(index) + " 1 3 2"));
	const std::vector<MenuTableRow> &rows = frame.table_rows[index];
	CHECK(rows.size() == 4 && (rows[1].flags & kTableRowFlagColor) != 0 &&
			rows[1].color == 0xFF00BFFFu && (rows[0].flags & kTableRowFlagColor) == 0);
	// A text column sorts by stricmp, ascending.
	rt.table_sort_by_column(table, 0);
	CHECK(rt.table_cell_text(table, 0, 0) == "ann" && rt.table_cell_text(table, 3, 0) == "dan");
	CHECK(rt.table_sort_column(table) == 0);
	// The key stack keeps the earlier key behind the new one.
	std::vector<int> keys(3, -1);
	table_push_sort_key(keys, 2, 3);
	table_push_sort_key(keys, 0, 3);
	CHECK(keys.size() == 3 && keys[0] == 0 && keys[1] == 2 && keys[2] == -1);
}

// The STARTUP screen's activate sets its VERSION label to the build's version text,
// found the way retail finds it: the first screen of that name (case-insensitive),
// pre-order, each window before its children, an unnamed window's subtree unsearched
// (retail's main.mnu carries two VERSION statics: the empty right-justified one the
// version fills, then the copyright line, which keeps its text).
// [orig: UI_OnStartupScreenActivate @0x5557f0; UI_FindScreenControl @0x63ae80;
//  CWnd_FindChildByName @0x646850]
void test_startup_version() {
	mnu::Document doc;
	mnu::Screen other;
	other.name = "OTHER";
	other.roots = { widget("ROOT0", mnu::WindowType::Window) };
	mnu::Window other_version = widget("VERSION", mnu::WindowType::Static);
	other_version.string_data.value = "other";
	other.roots[0].children = { other_version };
	mnu::Screen startup;
	startup.name = "Startup";
	startup.roots = { widget("MAIN", mnu::WindowType::Window) };
	mnu::Window unnamed;
	unnamed.type = mnu::WindowType::Window;
	mnu::Window hidden = widget("VERSION", mnu::WindowType::Static);
	hidden.string_data.value = "hidden";
	unnamed.children = { hidden };
	mnu::Window buttons = widget("BUTTONS", mnu::WindowType::Window);
	mnu::Window version = widget("version", mnu::WindowType::Static);
	mnu::Window copyright = widget("VERSION", mnu::WindowType::Static);
	copyright.string_data.value = "(c) 2009, NovaLogic, Inc.";
	buttons.children = { version, copyright };
	startup.roots[0].children = { unnamed, buttons };
	doc.screens = { other, startup };
	// OTHER: screen 1, ROOT0 2, VERSION 3; Startup: screen 4, MAIN 5, unnamed 6,
	// hidden VERSION 7, BUTTONS 8, version 9, copyright VERSION 10.

	// Frameless first: every read takes the state store or the authored text.
	MenuRuntime rt;
	CHECK(rt.open_document(&doc, "main.mnu", "OTHER"));
	CHECK(rt.find_screen_control("STARTUP", "VERSION") == 9);
	CHECK(rt.find_screen_control("startup", "Version") == 9);
	CHECK(rt.find_screen_control("OTHER", "VERSION") == 3);
	CHECK(rt.find_screen_control("STARTUP", "MAIN") == 5);
	CHECK(rt.find_screen_control("STARTUP", "ROOT0") == -1);
	CHECK(rt.find_screen_control("NOPE", "VERSION") == -1);
	CHECK(rt.find_screen_control("STARTUP", "") == -1);
	// Another screen's activate leaves every label alone.
	CHECK(rt.get_widget_text(3) == "other" && rt.get_widget_text(9).empty());

	CHECK(rt.show_screen("STARTUP"));
	CHECK(rt.get_widget_text(9) == "V1.7.5.7");
	CHECK(rt.get_widget_text(10) == "(c) 2009, NovaLogic, Inc.");
	CHECK(rt.get_widget_text(7) == "hidden");
	CHECK(rt.get_widget_text(3) == "other");
	// The frame draws it: version is the Startup screen's pre-order index 4, the
	// only label written.
	FakeFrame frame;
	MenuRuntime framed;
	framed.set_frame(&frame);
	CHECK(framed.open_document(&doc, "main.mnu", "Startup"));
	CHECK(framed.frame_index(9) == 4);
	CHECK(frame.texts.size() == 1 && frame.texts.count(4) == 1 && frame.texts[4] == "V1.7.5.7");

	// Each game shows its own binary's text; an unwitnessed one shows none, never JO's
	// [orig: Game_ParseCommandLineAndInit @0x4a7d5c (JO 1.7.5.7); jodemo.exe @0x48b440
	//  (1.0.0.9); DFX.EXE @0x4a6b93 (1.7.1.9); dfx2.exe @0x4a7d8c (1.7.5.7)].
	for (const auto &[code, text] : std::vector<std::pair<std::string, std::string>>{
				 {"jo", "V1.7.5.7"}, {"", "V1.7.5.7"}, {"JODEMO", "V1.0.0.9"}, {"dfx", "V1.7.1.9"},
				 {"dfx2", "V1.7.5.7"}, {"bhd", ""}}) {
		MenuRuntime game;
		game.set_game_code(code);
		CHECK(game.open_document(&doc, "main.mnu", "OTHER") && game.show_screen("STARTUP"));
		CHECK(game.get_widget_text(9) == text);
	}

	// A STARTUP screen whose VERSION sits only under an unnamed window draws none.
	mnu::Document bare;
	mnu::Screen bare_startup;
	bare_startup.name = "STARTUP";
	bare_startup.roots.resize(1);
	bare_startup.roots[0].type = mnu::WindowType::Window;
	bare_startup.roots[0].children = { widget("VERSION", mnu::WindowType::Static) };
	bare.screens = { bare_startup };
	MenuRuntime bare_rt;
	CHECK(bare_rt.open_document(&bare, "main.mnu", ""));
	CHECK(bare_rt.find_screen_control("STARTUP", "VERSION") == -1);
	CHECK(bare_rt.get_widget_text(3).empty());
}

int main() {
	test_table_columns_and_sort();
	test_index_and_frameless();
	test_startup_version();
	test_navigation_and_replay();
	test_activation_order();
	test_rows();
	test_bubble();
	test_radio_spin_tables_scroll();
	test_table_row_operations();
	test_table_runtime_columns();
	test_table_column_records();
	test_mouse();
	test_keys();
	test_two_root_keys();
	test_duplicate_screens();
	test_shell_flow();
	test_host_dialog();
	test_host_dialog_multiselect();
	test_multiple_roots();
	test_options_screen();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_runtime_test OK\n");
	return 0;
}
