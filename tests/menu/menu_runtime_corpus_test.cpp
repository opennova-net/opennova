// The menu runtime over the shipped screens' hotkey cases the 2026-09-23 grill found in
// its census corpus (set B3), each authored in fixtures/mnu/hotkeys.mnu in the shape of
// the shipped screen that holds it, its labels read through its own string table
// (menutxt.bin, minted here):
//  (a) VK_ENTER is no key: the shipped cmap.mnu WPNAME_OK and loadout.mnu ACCEPT (here
//      MARKER_OK and KIT's ACCEPT) register no row and Enter does not reach them;
//  (b) the {hot} label mnemonics of every class whose parse reads a STRING: the shipped
//      RADIO tab labels (game.mnu, mp.mnu, options.mnu; here SETTINGS's tabs, and PAUSE's
//      tabs and device radios behind its OPTIONS button) register one each, and the
//      letter selects the tab;
//  (c) ESC with an edit focused does nothing (the shipped cmap.mnu WPNAME in its MODAL
//      WAYPOINTNAME_DLG, whose WPNAME_CANCEL answers ESC once the focus is gone; here
//      MARKER_NAME in MARKER_DLG);
//  (d) Enter in an edit commits and then presses the VK_RETURN button (the shipped
//      player.mnu PLAYERNAME -> ACCEPT, whose cross-file jump follows its callbacks, and
//      PRE.MNU's passwords -> ACCEPT; here PROFILE and PREGAME);
//  (e) the popup gate: the shipped cmap.mnu's '=' ZOOMIN (here MAPVIEW's) answers only
//      while the MODAL dialog is closed.
// [orig: UI_DispatchKeyboardEventToChildren @ 0x63ad10; CWnd_ParseVirtualKeyNameW
//  @ 0x6467f0; CUIButtonWidget_ParseXMLAttributes @ 0x657c30;
//  CEditWnd_HandleKeyEvent @ 0x6623a0; CWnd_IsVisibleInHierarchy @ 0x646290]
// Witness record: docs/mnu/menu-re.md ("Hotkeys and the keyboard").

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <formats/mnu/mnu.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/menu/menu_edit.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_screen_inputs.h>
#include <runtime/menu/menu_text_tables.h>

#include "common/file_io.h"
#include "mnu/repo_menus.h"

using opennova::menu::EditKeyResult;
using opennova::menu::MenuEvent;
using opennova::menu::MenuFrameCompiler;
using opennova::menu::MenuFrameSeam;
using opennova::menu::MenuKeyInput;
using opennova::menu::MenuPumpWindow;
using opennova::menu::MenuRectF;
using opennova::menu::MenuRuntime;
using opennova::menu::MenuTableColumn;
using opennova::menu::MenuTableRow;
using opennova::menu::MenuTextTables;
using opennova::strutil::iequals;

namespace {

int failures = 0;

void expect(bool ok, const std::string &what) {
	if (!ok) {
		std::printf("  FAIL %s\n", what.c_str());
		++failures;
	}
}

using Reader = std::function<bool(const std::string &, std::vector<uint8_t> &)>;

// A reader as the engine's file source: a name that reads has stamp 1.
class ReaderFiles : public opennova::FileSource {
public:
	explicit ReaderFiles(Reader read) : read_(std::move(read)) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override { return read_(name, out); }
	uint64_t stamp(const std::string &name) const override {
		std::vector<uint8_t> bytes;
		return read_(name, bytes) ? 1 : 0;
	}

private:
	Reader read_;
};

// The frame seam over the compiler: each screen configured with its own string
// tables, so the mnemonics are the ones the parse registers; an edit's Enter
// commits. There is no geometry here (no mouse).
class CompiledSeam : public MenuFrameSeam {
public:
	CompiledSeam(const opennova::mnu::Document &doc, Reader read) : doc_(doc), files_(std::move(read)) {}

	bool is_configured() const override { return configured_; }
	void configure_screen(const std::string &screen) override {
		const opennova::mnu::Screen *s = doc_.find_screen(screen);
		tables_ = MenuTextTables();
		if (s != nullptr) tables_loader_.load(*s, files_, nullptr, tables_);
		compiler_.set_text_tables(&tables_);
		compiler_.configure(s);
		configured_ = s != nullptr;
	}
	void screen_configured() override {}
	void set_widget_shown_override(int, bool) override {}
	void set_widget_disabled(int, bool) override {}
	void set_widget_checked(int, bool) override {}
	void set_widget_text(int i, const std::string &t, int) override { texts_[i] = t; }
	void set_widget_items(int, const std::vector<std::string> &) override {}
	void set_widget_selection(int, int, int, int) override {}
	void set_widget_scroll_range(int, int, int, int, int) override {}
	void set_widget_selected_set(int, const std::vector<int> &) override {}
	void set_widget_disabled_items(int, const std::vector<uint8_t> &) override {}
	void set_widget_table_rows(int, const std::vector<MenuTableRow> &) override {}
	void set_widget_clip_rect(int, bool, int, int, int, int) override {}
	void set_widget_rect(int, int, int, int, int) override {}
	void set_widget_table_columns(int, bool, const std::vector<MenuTableColumn> &, int) override {}
	void set_widget_hover_item(int, int) override {}
	void set_widget_popup_open(int, bool) override {}
	void set_widget_focused(int, bool) override {}
	void set_widget_caret(int, int) override {}
	int get_widget_caret(int) const override { return 0; }
	std::string get_widget_text(int i) const override {
		const auto it = texts_.find(i);
		return it != texts_.end() ? it->second : std::string();
	}
	int item_count(int) const override { return 0; }
	std::string item_display_text(int, int) const override { return std::string(); }
	bool is_widget_disabled(int) const override { return false; }
	MenuRectF widget_rect(int) const override { return MenuRectF{}; }
	void design_scale(float &sx, float &sy) const override { sx = sy = 1.0f; }
	std::vector<MenuPumpWindow> press_mouse(float, float) override { return {}; }
	int process_mouse(float, float, bool) override { return -1; }
	void release_mouse() override {}
	bool press_popup_mouse(int, float, float) override { return false; }
	bool process_popup_mouse(int, float, float, bool) override { return false; }
	bool process_mouse_wheel(float, float, int) override { return false; }
	void set_cursor_state(bool, float, float) override {}
	void apply_claim_cursor() override {}
	void reset_cursor() override {}
	int combo_popup_row_at(int, float, float) const override { return -1; }
	bool combo_popup_contains(int, float, float) const override { return false; }
	int list_row_at(int, float, float) const override { return -1; }
	int spin_arrow_at(int, float, float) const override { return 0; }
	bool table_hit(int, float, float, int *row, int *column) const override {
		*row = *column = -1;
		return false;
	}
	std::string widget_mnemonic(int i) const override { return compiler_.widget_mnemonic(i); }
	void set_open_popup(int) override {}
	bool edit_char(int i, int unicode) override {
		texts_[i] += static_cast<char>(unicode);
		return true;
	}
	int edit_key(int, int key, bool) override {
		return static_cast<int>(key == opennova::menu::kEditKeyEnter ? EditKeyResult::kCommit : EditKeyResult::kNone);
	}

private:
	const opennova::mnu::Document &doc_;
	ReaderFiles files_;
	opennova::menu::MenuTextTableLoader tables_loader_;
	MenuTextTables tables_;
	MenuFrameCompiler compiler_;
	std::map<int, std::string> texts_;
	bool configured_ = false;
};

// One screen of one menu with the runtime and its recorder.
struct Session {
	opennova::mnu::Document doc;
	std::unique_ptr<CompiledSeam> seam;
	MenuRuntime rt;
	std::vector<MenuEvent> events;

	bool open(const std::string &path, const std::string &screen, const Reader &read) {
		std::string error;
		if (!opennova::mnu::parse_file(path, doc, error)) return false;
		seam = std::make_unique<CompiledSeam>(doc, read);
		rt.set_frame(seam.get());
		rt.set_sink([this](const MenuEvent &e) { events.push_back(e); });
		return rt.open_document(&doc, std::filesystem::path(path).filename().string(), screen);
	}
	std::vector<std::string> activated() const {
		std::vector<std::string> out;
		for (const MenuEvent &e : events)
			if (e.kind == MenuEvent::Kind::WidgetActivated) out.push_back(e.text);
		return out;
	}
	bool show(const char *name) {
		opennova::mnu::Action a;
		a.type = "WINDOW";
		a.state = "SHOW";
		a.target = name;
		return rt.dispatch_action(a);
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

// The labels of hotkeys.mnu's screens: the table its TEXT_RSRC names, minted here (section
// "menu"). A {hot} marker names its next letter the label's mnemonic; the side labels and the
// buttons' carry none.
std::vector<uint8_t> minted_menutxt() {
	const char *const rows[][2] = {
		{ "HK_SIDE_ONE", "Side one" },
		{ "HK_SIDE_TWO", "Side two" },
		{ "HK_BACK", "Back" },
		{ "HK_ACCEPT", "Accept" },
		{ "HK_CANCEL", "Cancel" },
		{ "HK_CLOSE", "Close" },
		{ "HK_RESUME", "Resume" },
		{ "HK_OPTIONS", "Options" },
		{ "HK_TAB_SOUND", "{hot}Sound" },
		{ "HK_TAB_SCREEN", "S{hot}creen" },
		{ "HK_TAB_DISPLAY", "{hot}Display" },
		{ "HK_TAB_CONTROLS", "{hot}Controls" },
		{ "HK_KEYBOARD", "{hot}Keyboard" },
		{ "HK_MOUSE", "{hot}Mouse" },
	};
	opennova::rtxt::File table;
	table.sections.push_back({ opennova::menu::kMenuTextSection, 0 });
	for (const auto &row : rows) {
		opennova::rtxt::Entry e;
		e.key = row[0];
		e.text = row[1];
		e.section_index = 0;
		table.entries.push_back(e);
		++table.sections[0].string_count;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	opennova::rtxt::write(table, bytes, error);
	return bytes;
}

} // namespace

int main() {
	const std::string path = repo_menus::path("fixtures/mnu/hotkeys.mnu");
	const std::vector<uint8_t> menutxt = minted_menutxt();
	const Reader read = [&path, &menutxt](const std::string &name, std::vector<uint8_t> &out) {
		const std::string file = repo_menus::lower(std::filesystem::path(name).filename().string());
		if (file == "menutxt.bin") {
			out = menutxt;
			return true;
		}
		return file == "hotkeys.mnu" && test_io::read_file(path, out);
	};
	opennova::mnu::Document menu;
	std::string error;
	if (menutxt.empty() || !opennova::mnu::parse_file(path, menu, error)) {
		std::fprintf(stderr, "fixtures/mnu/hotkeys.mnu or its minted table does not read: %s\n", error.c_str());
		return 1;
	}

	// The hotkey tables of every screen: the radio mnemonics, and the VK_ENTER rows.
	std::printf("hotkeys.mnu's hotkey tables:\n");
	std::vector<std::string> radio_rows;
	int enter_sources = 0;
	int enter_widgets = 0;
	int enter_rows = 0;
	for (const opennova::mnu::Screen &screen : menu.screens) {
		Session s;
		if (!s.open(path, screen.name, read)) {
			expect(false, "hotkeys.mnu " + screen.name + " opens");
			continue;
		}
		std::string line;
		for (const opennova::menu::MenuHotkeyRow &row : s.rt.hotkey_rows()) {
			const opennova::mnu::Window *w = s.rt.index().window(row.id);
			if (w == nullptr) continue;
			line += " " + w->name + (row.virtual_key ? "=vk" + std::to_string(row.key) : "='" +
							std::string(1, static_cast<char>(row.key)) + "'");
			if (!row.virtual_key && w->type == opennova::mnu::WindowType::Radio)
				radio_rows.push_back(screen.name + "/" + w->name + "='" + std::string(1, static_cast<char>(row.key)) + "'");
		}
		for (int id : s.rt.current_screen_ids()) {
			const opennova::mnu::Window *w = s.rt.index().window(id);
			bool authors_enter = false;
			for (const opennova::mnu::Hotkey &hk : w->hotkeys)
				if (hk.virtual_key && iequals(hk.value, "VK_ENTER")) authors_enter = true;
			if ((screen.name == "KIT" && w->name == "ACCEPT") || w->name == "MARKER_OK") ++enter_sources;
			if (!authors_enter) continue;
			++enter_widgets;
			for (const opennova::menu::MenuHotkeyRow &row : s.rt.hotkey_rows())
				if (row.id == id && row.virtual_key) ++enter_rows;
		}
		std::printf("  %-10s%s\n", screen.name.c_str(), line.c_str());
	}
	// The six {hot} tab labels (SETTINGS's two, PAUSE's two tabs and two device radios), each
	// its letter; the side radios' labels carry no marker and register nothing.
	const std::vector<std::string> expected_radios = { "SETTINGS/RADIO_TAB_SOUND='S'", "SETTINGS/RADIO_TAB_SCREEN='c'",
		"PAUSE/RADIO_TAB_DISPLAY='D'", "PAUSE/RADIO_TAB_CONTROLS='C'", "PAUSE/KEYBOARD='K'", "PAUSE/MOUSE='M'" };
	expect(radio_rows == expected_radios, "the {hot} RADIO labels register their letter, a label with none nothing");
	// VK_ENTER is key 0 (the shipped cmap WPNAME_OK and loadout ACCEPT; here KIT's ACCEPT and
	// MARKER_OK), which registers nothing: the reader leaves it out.
	expect(enter_sources == 2, "the two VK_ENTER buttons are on their screens");
	expect(enter_widgets == 0 && enter_rows == 0, "VK_ENTER registers nothing (KIT ACCEPT, MARKER_OK)");

	// (a) Enter on the kit screen reaches no ACCEPT.
	{
		Session s;
		expect(s.open(path, "KIT", read), "KIT opens");
		expect(!s.rt.handle_key(vk(13)) && s.activated().empty(), "kit: Enter fires nothing");
		expect(s.rt.handle_key(vk(27)) && s.activated() == std::vector<std::string>{ "BACK" }, "kit: ESC is BACK's");
	}
	// (b) A tab's letter selects it, reached the way a player reaches it: PAUSE's OPTIONS button
	// first, then its CONTROLS tab for the device radios.
	for (const char *screen_name : { "SETTINGS", "PAUSE" }) {
		Session probe;
		if (!probe.open(path, screen_name, read)) continue;
		for (const opennova::menu::MenuHotkeyRow &row : probe.rt.hotkey_rows()) {
			const opennova::mnu::Window *w = probe.rt.index().window(row.id);
			if (row.virtual_key || w == nullptr || w->type != opennova::mnu::WindowType::Radio) continue;
			Session s;
			s.open(path, screen_name, read);
			const bool device = w->name == "KEYBOARD" || w->name == "MOUSE";
			if (std::string(screen_name) == "PAUSE") s.rt.activate(s.rt.widget_id("OPTIONS"));
			if (device) s.rt.activate(s.rt.widget_id("RADIO_TAB_CONTROLS"));
			s.events.clear();
			const bool fired = s.rt.handle_key(typed(row.key));
			const std::vector<std::string> got = s.activated();
			const bool selected = fired && !got.empty() && got.front() == w->name && s.rt.is_widget_checked(row.id);
			std::printf("  %-10s %-18s '%c' -> %s\n", screen_name, w->name.c_str(), static_cast<char>(row.key),
					got.empty() ? "(nothing)" : got.front().c_str());
			expect(selected, std::string(screen_name) + " " + w->name + ": its letter selects it");
		}
	}
	// A device radio's letter does nothing while its tab is hidden.
	{
		Session s;
		expect(s.open(path, "PAUSE", read), "PAUSE opens");
		s.rt.activate(s.rt.widget_id("OPTIONS"));
		s.events.clear();
		expect(!s.rt.handle_key(typed('K')) && s.activated().empty(), "pause: a hidden tab's radio answers no letter");
	}
	// (c) + (e) MAPVIEW: the popup gate, and ESC with the edit focused.
	{
		Session s;
		expect(s.open(path, "MAPVIEW", read), "MAPVIEW opens");
		expect(s.rt.handle_key(typed('=')) && s.activated() == std::vector<std::string>{ "ZOOMIN" },
				"mapview: '=' is ZOOMIN's with the dialog closed");
		s.events.clear();
		expect(s.show("MARKER_DLG") && s.rt.open_popup() == s.rt.widget_id("MARKER_DLG"),
				"mapview: the MODAL dialog opens the popup");
		expect(!s.rt.handle_key(typed('=')) && s.activated().empty(), "mapview: '=' is shut out by the popup");
		expect(!s.rt.handle_key(vk(13)) && s.activated().empty(), "mapview: Enter reaches no VK_ENTER MARKER_OK");
		s.rt.focus_edit(s.rt.widget_id("MARKER_NAME"));
		expect(s.rt.handle_key(vk(27)) && s.activated().empty() &&
						s.rt.focused_widget() == s.rt.widget_id("MARKER_NAME"),
				"mapview: ESC with MARKER_NAME focused does nothing");
		s.show("MARKER_DLG"); // a WINDOW row drops the focus
		expect(s.rt.focused_widget() == -1, "mapview: the focus dropped");
		expect(s.rt.handle_key(vk(27)) && s.activated() == std::vector<std::string>{ "MARKER_CANCEL" },
				"mapview: ESC is MARKER_CANCEL's, the CLOSE outside the popup is shut out");
	}
	// (d) Enter in an edit commits, then presses the VK_RETURN button.
	{
		Session s;
		expect(s.open(path, "PROFILE", read), "PROFILE opens");
		s.rt.focus_edit(s.rt.widget_id("CALLSIGN"));
		expect(s.rt.handle_key(vk(13)) && s.rt.focused_widget() == -1, "profile: Enter commits CALLSIGN");
		int activated = -1;
		int requested = -1;
		for (size_t i = 0; i < s.events.size(); ++i) {
			if (s.events[i].kind == MenuEvent::Kind::WidgetActivated && s.events[i].text == "ACCEPT")
				activated = static_cast<int>(i);
			if (s.events[i].kind == MenuEvent::Kind::MenuRequested && s.events[i].text == "main.mnu" &&
					s.events[i].text2 == "STARTUP")
				requested = static_cast<int>(i);
		}
		expect(activated >= 0 && requested > activated,
				"profile: ACCEPT fires, and its jump to main.mnu STARTUP follows its callbacks");
	}
	for (const char *password : { "GAME_PASSWORD", "TEAM_PASSWORD" }) {
		Session s;
		expect(s.open(path, "PREGAME", read), "PREGAME opens");
		s.show(std::string(password) == "GAME_PASSWORD" ? "GAME_PASSWORD_WRAPPER" : "TEAM_PASSWORD_WRAPPER");
		s.show("ABORTRETRY_WRAPPER");
		s.rt.focus_edit(s.rt.widget_id(password));
		expect(s.rt.focused_widget() == s.rt.widget_id(password), std::string("pregame: ") + password + " focuses");
		expect(s.rt.handle_key(vk(13)) && s.activated() == std::vector<std::string>{ "ACCEPT" },
				std::string("pregame: Enter in ") + password + " commits and presses ACCEPT");
	}
	if (failures != 0) {
		std::fprintf(stderr, "\n%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("menu_runtime_corpus: all checks passed\n");
	return 0;
}
