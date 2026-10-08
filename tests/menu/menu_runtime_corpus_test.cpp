// The menu runtime over the 2026-09-23 grill's census corpus: the sixteen .mnu loose
// at the OPENNOVA_JO_ASSETS root, each screen's labels read through its own string
// tables (menutxt.bin), checked against the grill's shipped hotkey cases (set B3):
//  (a) VK_ENTER is no key: cmap.mnu WPNAME_OK and loadout.mnu ACCEPT register no row
//      and Enter does not reach them;
//  (b) the {hot} label mnemonics of every class whose parse reads a STRING: the 18
//      shipped RADIO tab labels (game.mnu, mp.mnu, options.mnu) register one each,
//      and the letter selects the tab;
//  (c) ESC with an edit focused does nothing (cmap.mnu WPNAME in its MODAL
//      WAYPOINTNAME_DLG, whose WPNAME_CANCEL answers ESC once the focus is gone);
//  (d) Enter in an edit commits and then presses the VK_RETURN button (player.mnu
//      PLAYERNAME -> ACCEPT, whose cross-file jump follows its callbacks; PRE.MNU's
//      passwords -> ACCEPT);
//  (e) the popup gate: cmap.mnu's '=' ZOOMIN answers only while the MODAL dialog is
//      closed.
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
#include "common/retail_paths.h"

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
	void set_widget_text(int i, const std::string &t) override { texts_[i] = t; }
	void set_widget_items(int, const std::vector<std::string> &) override {}
	void set_widget_selection(int, int, int, int) override {}
	void set_widget_scroll_range(int, int, int, int, int) override {}
	void set_widget_selected_set(int, const std::vector<int> &) override {}
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

} // namespace

int main() {
	const std::string root = retail::assets();
	std::map<std::string, std::string> files; // lowercased name -> path
	std::vector<std::string> menus;
	std::error_code ec;
	if (!root.empty()) {
		for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
			if (!entry.is_regular_file(ec)) continue;
			const std::string name = entry.path().filename().string();
			files[retail::lower_ascii(name)] = entry.path().generic_string();
			if (name.size() > 4 && retail::lower_ascii(name.substr(name.size() - 4)) == ".mnu")
				menus.push_back(entry.path().generic_string());
		}
	}
	if (menus.empty() || files.count("menutxt.bin") == 0)
		return retail::skip("OPENNOVA_JO_ASSETS/*.mnu and menutxt.bin (the grill's census corpus)");
	std::sort(menus.begin(), menus.end());
	const Reader read = [&files](const std::string &name, std::vector<uint8_t> &out) {
		const auto found = files.find(retail::lower_ascii(std::filesystem::path(name).filename().string()));
		return found != files.end() && test_io::read_file(found->second, out);
	};
	const auto path_of = [&files](const char *name) {
		const auto found = files.find(retail::lower_ascii(name));
		return found != files.end() ? found->second : std::string();
	};

	// The hotkey tables of every screen: the radio mnemonics, and the VK_ENTER rows.
	std::printf("the census corpus's hotkey tables (%s):\n", root.c_str());
	int radio_rows = 0;
	int enter_widgets = 0;
	int enter_rows = 0;
	for (const std::string &path : menus) {
		opennova::mnu::Document doc;
		std::string error;
		if (!opennova::mnu::parse_file(path, doc, error)) {
			expect(false, path + " parses");
			continue;
		}
		for (const opennova::mnu::Screen &screen : doc.screens) {
			Session s;
			if (!s.open(path, screen.name, read)) {
				expect(false, path + " " + screen.name + " opens");
				continue;
			}
			std::string line;
			for (const opennova::menu::MenuHotkeyRow &row : s.rt.hotkey_rows()) {
				const opennova::mnu::Window *w = s.rt.index().window(row.id);
				if (w == nullptr) continue;
				line += " " + w->name + (row.virtual_key ? "=vk" + std::to_string(row.key) : "='" +
								std::string(1, static_cast<char>(row.key)) + "'");
				if (!row.virtual_key && w->type == opennova::mnu::WindowType::Radio) ++radio_rows;
			}
			for (int id : s.rt.current_screen_ids()) {
				const opennova::mnu::Window *w = s.rt.index().window(id);
				bool authors_enter = false;
				for (const opennova::mnu::Hotkey &hk : w->hotkeys)
					if (hk.virtual_key && iequals(hk.value, "VK_ENTER")) authors_enter = true;
				if (!authors_enter) continue;
				++enter_widgets;
				for (const opennova::menu::MenuHotkeyRow &row : s.rt.hotkey_rows())
					if (row.id == id && row.virtual_key) ++enter_rows;
			}
			std::printf("  %-12s %-18s%s\n", std::filesystem::path(path).filename().string().c_str(),
					screen.name.c_str(), line.c_str());
		}
	}
	std::printf("  radio mnemonic rows %d (grill: 18); widgets authoring VK_ENTER %d, their rows %d\n",
			radio_rows, enter_widgets, enter_rows);
	expect(radio_rows == 18, "the 18 shipped RADIO tab labels register their {hot} letter");
	expect(enter_widgets == 2 && enter_rows == 0, "VK_ENTER registers nothing (cmap WPNAME_OK, loadout ACCEPT)");

	// (a) Enter on the loadout screen reaches no ACCEPT.
	{
		Session s;
		expect(s.open(path_of("loadout.mnu"), "LOADOUT", read), "loadout.mnu opens");
		expect(!s.rt.handle_key(vk(13)) && s.activated().empty(), "loadout: Enter fires nothing");
		expect(s.rt.handle_key(vk(27)) && s.activated() == std::vector<std::string>{ "BACK" },
				"loadout: ESC is BACK's");
	}
	// (b) A tab's letter selects it: options.mnu's and game.mnu's tabs.
	for (const char *file : { "options.mnu", "game.mnu", "mp.mnu" }) {
		opennova::mnu::Document doc;
		std::string error;
		opennova::mnu::parse_file(path_of(file), doc, error);
		for (const opennova::mnu::Screen &screen : doc.screens) {
			Session probe;
			if (!probe.open(path_of(file), screen.name, read)) continue;
			for (const opennova::menu::MenuHotkeyRow &row : probe.rt.hotkey_rows()) {
				const opennova::mnu::Window *w = probe.rt.index().window(row.id);
				if (row.virtual_key || w == nullptr || w->type != opennova::mnu::WindowType::Radio) continue;
				Session s;
				s.open(path_of(file), screen.name, read);
				// Reach the tab the way a player does, through the authored actions: the
				// pause menu's OPTIONS button, then the CONTROLS tab for the device radios.
				const bool device = w->name == "KEYBOARD" || w->name == "MOUSE" || w->name == "JOYSTICK";
				if (std::string(file) == "game.mnu") s.rt.activate(s.rt.widget_id("OPTIONS"));
				if (device) s.rt.activate(s.rt.widget_id("RADIO_TAB_CONTROLS"));
				s.events.clear();
				const bool fired = s.rt.handle_key(typed(row.key));
				const std::vector<std::string> got = s.activated();
				const bool selected = fired && !got.empty() && got.front() == w->name && s.rt.is_widget_checked(row.id);
				std::printf("  %-12s %-10s '%c' -> %s\n", file, w->name.c_str(), static_cast<char>(row.key),
						got.empty() ? "(nothing)" : got.front().c_str());
				expect(selected, std::string(file) + " " + w->name + ": its letter selects it");
			}
		}
	}
	// (c) + (e) cmap.mnu: the popup gate, and ESC with the edit focused.
	{
		Session s;
		expect(s.open(path_of("cmap.mnu"), "CMAP", read), "cmap.mnu opens");
		expect(s.rt.handle_key(typed('=')) && s.activated() == std::vector<std::string>{ "ZOOMIN" },
				"cmap: '=' is ZOOMIN's with the dialog closed");
		s.events.clear();
		expect(s.show("WAYPOINTNAME_DLG") && s.rt.open_popup() == s.rt.widget_id("WAYPOINTNAME_DLG"),
				"cmap: the MODAL dialog opens the popup");
		expect(!s.rt.handle_key(typed('=')) && s.activated().empty(), "cmap: '=' is shut out by the popup");
		expect(!s.rt.handle_key(vk(13)) && s.activated().empty(), "cmap: Enter reaches no VK_ENTER WPNAME_OK");
		s.rt.focus_edit(s.rt.widget_id("WPNAME"));
		expect(s.rt.handle_key(vk(27)) && s.activated().empty() &&
						s.rt.focused_widget() == s.rt.widget_id("WPNAME"),
				"cmap: ESC with WPNAME focused does nothing");
		s.show("WAYPOINTNAME_DLG"); // a WINDOW row drops the focus
		expect(s.rt.focused_widget() == -1, "cmap: the focus dropped");
		expect(s.rt.handle_key(vk(27)) && s.activated() == std::vector<std::string>{ "WPNAME_CANCEL" },
				"cmap: ESC is WPNAME_CANCEL's, the OK outside the popup is shut out");
	}
	// (d) Enter in an edit commits, then presses the VK_RETURN button.
	{
		Session s;
		expect(s.open(path_of("player.mnu"), "PLAYER_INFO", read), "player.mnu opens");
		s.rt.focus_edit(s.rt.widget_id("PLAYERNAME"));
		expect(s.rt.handle_key(vk(13)) && s.rt.focused_widget() == -1, "player: Enter commits PLAYERNAME");
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
				"player: ACCEPT fires, and its jump to main.mnu STARTUP follows its callbacks");
	}
	for (const char *password : { "GAME_PASSWORD", "TEAM_PASSWORD" }) {
		Session s;
		expect(s.open(path_of("PRE.MNU"), "PRE_GAME_MENU", read), "PRE.MNU opens");
		s.show(std::string(password) == "GAME_PASSWORD" ? "GAME_PASSWORD_WRAPPER" : "TEAM_PASSWORD_WRAPPER");
		s.show("ABORTRETRY_WRAPPER");
		s.rt.focus_edit(s.rt.widget_id(password));
		expect(s.rt.focused_widget() == s.rt.widget_id(password), std::string("PRE: ") + password + " focuses");
		expect(s.rt.handle_key(vk(13)) && s.activated() == std::vector<std::string>{ "ACCEPT" },
				std::string("PRE: Enter in ") + password + " commits and presses ACCEPT");
	}
	if (failures != 0) {
		std::fprintf(stderr, "\n%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("menu_runtime_corpus: all checks passed\n");
	return 0;
}
