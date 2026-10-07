#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <editor/preview/texture_header.h>
#include <editor/preview/viewport_follow.h>
#include <formats/mnu/mnu.h>
#include <runtime/controls/binding_set.h>
#include <runtime/menu/menu_commands.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/menu_frame_assets.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_state_frame.h>
#include <runtime/menu/options_screen.h>

namespace opennova::editor {

// A menu tried as the game runs it (ADR 0046 DI-35; CONTEXT.md "Try mode"): the menu viewport's
// picture behaving as the game's menu, through the runtime's own driver (menu::MenuRuntime: the
// windows' ACTION rows, the screen history across files, the toggles, the spin lists, the lists and
// their combos, the edit fields, the focus and the keys, the windows' sounds) over the frame's own pump
// (menu::MenuStateFrame, headless at design scale), and what the game's code does with the controls it
// binds by name (menu::MenuCommands; the shell's per-open wiring: the mission lists filled with the
// project's missions, the Options policy and its controls table at the game's defaults).
//
// A sandbox: every value it changes (a check, a pick, a slider, a typed text, a key binding) lives in
// the runtime's own store and its sandbox bindings until a Reset, never in the user's configuration or
// a file of the project. What leaves the menu (a mission started, a quit, a game joined or hosted, a
// browser opened, a profile saved) is not done: it is said, an outcome.
//
// What it reads is the caller's, handed in on every call (MenuTrySource): a jump to another menu reads
// that menu as the game reads it by name.
struct MenuTrySource {
	// The project's files (the open documents standing in): the screens' string tables, fonts and textures.
	const FileSource *files = nullptr;
	// The shell's %VAR% list.
	const std::map<std::string, std::string> *vars = nullptr;
	// The menu the game reads by its file name (null: none the game would read).
	std::function<std::shared_ptr<const mnu::Document>(const std::string &file)> menu;
	// The rows of the game's mission catalog (the lists it fills with its missions).
	std::function<std::vector<menu::MissionChoice>()> missions;
};

// What the game would have done, which Try says instead: a command the code binds by name (its
// menu::MenuCommand token), a URL ("url"), a service verb's row ("service"), a jump to a menu or a screen
// the game would not find ("missing_menu", "missing_screen": the game changes nothing), or what the
// remap capture took ("binding").
struct MenuTryOutcome {
	uint64_t seq = 0;
	std::string kind;
	std::string control; // the control whose activation it was ("" none)
	std::string file; // where: the menu and the screen
	std::string screen;
	std::string words; // what the game does, in words
};
// A screen shown, in order.
struct MenuTryVisit {
	std::string file;
	std::string screen;
};
// A value the sandbox holds: the control (on its menu and screen), its kind ("checkbox", "radio",
// "list", "combo", "spinlist", "scroll", "edit", "table", "binding") and its value as the game reads it.
struct MenuTrySetting {
	std::string file;
	std::string screen;
	std::string control;
	std::string kind;
	std::string value;
};
// A window sound the runtime's pump played: the menu, the screen, the window (its widget id in the
// runtime's tree and its NAME), the pump's state (menu::MenuSoundState) and the row's bank and set.
struct MenuTrySound {
	std::string file;
	std::string screen;
	int id = -1;
	std::string window;
	int state = 0;
	std::string bank;
	std::string trigger;
};

class MenuTry {
public:
	MenuTry();
	~MenuTry();
	MenuTry(const MenuTry &) = delete;
	MenuTry &operator=(const MenuTry &) = delete;

	// Tries the menu `file` from its screen `screen` (empty: its first), in a mission when the game opens
	// that file in one (menu::is_mission_menu_file). False, with why, when the game would show nothing of it.
	bool start(const MenuTrySource &source, const std::string &file, const std::string &screen, std::string &error);
	// Back to where it started, everything the sandbox holds dropped.
	bool reset(const MenuTrySource &source, std::string &error);
	// The menus changed (an edit, a file saved): each read again, the menu shown opened again at its screen
	// (its windows' state dropped: the game reads a menu anew), the history and the breadcrumb kept.
	void reload(const MenuTrySource &source);

	// One sample of the game's mouse (design units, the left button), at `now_ms` (the double click's clock).
	void mouse(const MenuTrySource &source, float x, float y, bool down, uint32_t now_ms);
	// The game's click there: the mouse onto the point, pressed and let go.
	void click(const MenuTrySource &source, float x, float y, uint32_t now_ms);
	// One wheel notch (+1 down, -1 up); true when something scrolled.
	bool wheel(const MenuTrySource &source, float x, float y, int steps);
	// One key press as Windows delivers it (WM_KEYDOWN then WM_CHAR): the Options remap capture's while it
	// is armed, else the runtime's. True when something took it.
	bool key(const MenuTrySource &source, const menu::MenuKeyInput &key);
	// The frame's clock (a focused edit's caret).
	void set_time_ms(uint32_t ms) { frame_.set_time_ms(ms); }

	bool started() const { return started_; }
	bool in_mission() const { return in_mission_; }
	const std::string &start_file() const { return start_file_; }
	const std::string &start_screen() const { return start_screen_; }
	// The menu shown, its image (the game's reading of it) and its screen in it (null before a start).
	const std::string &file() const { return file_; }
	const std::string &screen_name() const { return runtime_.current_screen(); }
	const std::shared_ptr<const mnu::Document> &image() const { return image_; }
	const mnu::Screen *screen() const { return screen_; }
	// The frame: what the device draws (the runtime's state on the screen's compile).
	const menu::MenuFrameCompiler &compiler() const { return frame_.compiler(); }
	const menu::MenuFrameState &state() const { return frame_.state(); }
	const menu::MenuFrameAssets &assets() const { return assets_; }
	const menu::MenuRuntime &runtime() const { return runtime_; }
	// Moves with each screen configured (the device configures again) and with each write to its state
	// (the device takes the state again).
	uint64_t configures() const { return frame_.configures(); }
	uint64_t state_serial() const { return frame_.serial() + serial_; }
	// The files its screens read (the string tables, fonts and textures of the screen shown, the menus).
	FileStamps reads(const FileSource &files) const;

	const std::vector<MenuTryVisit> &breadcrumb() const { return breadcrumb_; }
	const std::vector<MenuTryOutcome> &outcomes() const { return outcomes_; }
	const std::vector<MenuTrySetting> &settings() const { return settings_; }
	// The sounds the pump played since the last take, oldest first.
	std::vector<MenuTrySound> take_sounds();
	// The window's widget id under the game's mouse now (-1 none).
	int hovered() const;

	static constexpr size_t kOutcomesKept = 32;
	static constexpr size_t kBreadcrumbKept = 64;

private:
	// The menu `file` read as the game reads it (kept by name until a reload); null for none.
	std::shared_ptr<const mnu::Document> menu_(const std::string &file);
	// Opens `file` at `screen` (the runtime's open_document) and runs the shell's per-open wiring: the
	// Options surface prepared, the commands bound, the mission lists filled.
	bool open_(const std::string &file, const std::string &screen);
	void prepare_();
	void on_event_(const menu::MenuEvent &event);
	void on_command_(menu::MenuCommand command, const std::string &control);
	void outcome_(const std::string &kind, const std::string &control, const std::string &words);
	void setting_(const std::string &control, const std::string &kind, const std::string &value);
	bool configure_(const std::string &screen, menu::MenuFrameCompiler &compiler);
	// The cross-file history's pop (POP_SCREEN past the file's own, HIDDEN_BACK): true when it showed one.
	bool pop_history_();

	struct Held {
		std::shared_ptr<const mnu::Document> image;
		bool read = false;
	};
	const MenuTrySource *source_ = nullptr; // the call's, while one runs
	menu::MenuRuntime runtime_;
	menu::MenuStateFrame frame_;
	menu::MenuFrameAssets assets_;
	TextureHeaderProbe decoder_;
	menu::MenuCommands commands_;
	menu::MenuFlow flow_;
	menu::OptionsScreen options_;
	controls::BindingSet bindings_; // the sandbox's: the game's defaults
	std::map<std::string, Held> menus_; // by file name, lower case
	std::shared_ptr<const mnu::Document> image_;
	const mnu::Screen *screen_ = nullptr;
	std::string file_;
	std::string start_file_;
	std::string start_screen_;
	bool started_ = false;
	bool in_mission_ = false;
	std::vector<MenuTryVisit> breadcrumb_;
	std::vector<MenuTryOutcome> outcomes_;
	uint64_t outcome_seq_ = 0;
	std::vector<MenuTrySetting> settings_;
	std::vector<MenuTrySound> sounds_;
	uint64_t serial_ = 0; // moves with a write the frame does not see (a history row, a setting)
};

// The envelope's `try` (the menu viewport's body while Try is on): {on, file, screen, in_mission, start
// {file, screen}, breadcrumb [{file, screen}], history [{file, screen}], focus, popup, hovered (each a
// control's NAME, null none), outcomes [{seq, kind, control, file, screen, words}] (the last
// kOutcomesKept), last (the latest outcome, null none), settings [{file, screen, control, kind, value}]}.
io::JsonValue menu_try_to_json(const MenuTry &menu_try);
// The screen's windows as Try holds them now: [{index, name, type, rect [l, t, r, b], shown, disabled,
// hovered, pressed, checked, focused, text, selected, items, value, range [min, max], rows [[cell...]],
// selected_rows}], each state member present where the window's type has it, the text in UTF-8.
io::JsonValue menu_try_widgets_json(const MenuTry &menu_try);

// The game's key by its name: a HOTKEY's virtual-key name (VK_RETURN, VK_ESCAPE, VK_SPACE), the edit
// field's (VK_BACK, VK_TAB, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END, VK_DELETE, VK_PRIOR,
// VK_NEXT) and their plain names (Enter, Escape, Space, Backspace, Tab, Left, Right, Up, Down, Home, End,
// Delete, PageUp, PageDown), without case; or one printable character, its virtual-key code where it has
// one (a letter's capital, a digit) and the character typed. False for another.
bool menu_try_key_from_name(const std::string &name, bool shift, menu::MenuKeyInput &out);

} // namespace opennova::editor
