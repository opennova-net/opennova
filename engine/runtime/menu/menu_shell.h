#pragma once

// The menu shell: the game's menu mode over one MenuRuntime, what the original's front end does
// around its UI scene [orig: Menu_InitShellResources @ 0x552500 (the shell made: main.mnu
// @ 0x552656 and its start screen @ 0x552668); Menu_UpdateFrame @ 0x5528a0 (the mode's update);
// Menu_TeardownShellAndCloseBinkVideos @ 0x54e430 (the shell destroyed); Game_ReloadExpansionAndMods
// @ 0x552710 (the Mods pick's reload)]. It runs, over the documents its host reads by name:
// - the menu mode's enter and leave across a mission, the screen the mission was started from
//   shown again on the return (screen_history.h, D-MNU-28), and the in-game menu over a mission;
// - each open's wiring: the commands the game binds by control NAME (menu_commands.h), the lists
//   it fills (the missions, menu_flow.h; the Mods, mod_list.h), the Options screens over the
//   player's profile record and options (options_screen.h, with the dialog's preview baseline),
//   and the screen controller that owns the document (menu_screen_controller.h);
// - what a control's command asks: a jump to another menu file and its history, the back and
//   the quit, the mission's start, the Mods pick's deferred reload, a URL, the updater;
// - the update gate (ADR 0067 d3 as amended 2026-10-28, OpenNova's deliberate divergence: retail's
//   UPDATE is always there and its multiplayer never held back).
// What leaves the menu (a mission started, a quit, NovaWorld) and every device leg (reading a menu,
// mounting a game, the music, the backdrop movies, the browser) is the host's (MenuShellHost). The
// game's binding (godot/src/mnu/menu_driver) and the editor's Try mode (editor/preview/menu_try,
// which says what the game would do instead of doing it) run this one shell. Witness record:
// docs/mnu/menu-re.md, docs/mnu/menu-wiring.md "Shell wiring".

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <formats/mnu/mnu.h>
#include <formats/playersav/player_sav.h>
#include <runtime/menu/menu_commands.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_screen_controller.h>
#include <runtime/menu/mod_list.h>
#include <runtime/menu/options_screen.h>

namespace opennova::menu {

// What leaves the menu, the host's to do.
enum class ShellExit : uint8_t {
	StartMission, // text: the selected mission's file
	Exit, // text: what ends the process (the exit's log line)
	ReturnToMenu, // the mission ends, back to the screen it was started from
	Restart, // the single-player mission starts again
	Resume, // the in-game menu's back with no screen behind it: the mission resumes
	NovaWorld, // the NovaWorld login opens
};

// What the shell did not do, and why: the game logs it, Try says it.
enum class ShellNotice : uint8_t {
	MenuMissing, // text: the menu file the game does not find (the screen stays)
	ScreenMissing, // text: the file, text2: the screen it does not hold (the screen stays)
	NothingSelected, // a start with no mission selected starts nothing
	ExpansionUnchanged, // text: the pick, the game running: ACCEPT takes nothing
	ExpansionQueued, // text: the pick ("" the base game): the game switches at the frame's tail
	ExpansionNeedsPackedInstall, // text: the pick; a loose mount takes no expansion
	UpdateNotStarted, // the updater did not start: the menu stays
};

class MenuShellHost {
public:
	virtual ~MenuShellHost() = default;

	// --- what the shell reads ---
	// The menu `file` as the game reads it by name (the VFS, its archives first); null when it
	// finds none or the file does not parse.
	virtual std::shared_ptr<const mnu::Document> read_menu(const std::string &file) = 0;
	// The mission catalog's rows (the lists the game fills with its missions).
	virtual std::vector<MissionChoice> missions() = 0;
	// The game folder's expansion records (vfs_expansion_records, the scan the game makes at boot),
	// the game running ("" the base game) and whether the game is a packed install (a loose mount
	// takes no expansion).
	virtual std::vector<ExpansionRecord> expansions() = 0;
	virtual std::string expansion() = 0;
	virtual bool packed_install() = 0;
	// Whether the updater's check found an update waiting (an unknown answer is false).
	virtual bool update_waiting() { return false; }
	// The player profile's current record the Options screens seed from and their Accepts write
	// (null: no profile, the screens at the catalog's defaults).
	virtual playersav::ProfileRecord *profile_record() { return nullptr; }
	// The player's options as the Options screens' words, and an edit of them (applied and saved
	// at once, D-MNU-19). False: no options owner (the words stay the controls' own).
	virtual bool player_words(OptionsScreen::Words &out) {
		(void)out;
		return false;
	}
	virtual void set_player_words(const OptionsScreen::Words &words) { (void)words; }

	// --- what leaves the menu ---
	virtual void leave(ShellExit exit, const std::string &control, const std::string &text) = 0;
	// The updater started (the game then quits); false when it did not.
	virtual bool launch_update(const std::string &control) = 0;
	// The external browser opened on `url` (its scheme added where the authored link is bare).
	virtual void open_browser(const std::string &url) = 0;
	// A URL without the external browser: retail's in-game page fetch, which the game does not host.
	virtual void fetch_page(const std::string &url) { (void)url; }
	// A service verb's row (FORM_POST, GLB_*, LAN_*, MNX, APPMSG) or a GLB filter: the NovaWorld and
	// LAN layers' (the screen controllers serve the game's by control name).
	virtual void service(const MenuEvent &event) { (void)event; }
	// A command the code binds on a delegate's control (the LAN screens', PLAYER_INFO's, an in-mission
	// screen's), which its screen controller runs; the shell does nothing with it.
	virtual void delegate_command(MenuCommand command, const std::string &control) {
		(void)command;
		(void)control;
	}
	virtual void notice(ShellNotice notice, const std::string &control, const std::string &text,
			const std::string &text2) {
		(void)notice;
		(void)control;
		(void)text;
		(void)text2;
	}

	// --- the Options screens' effects outward ---
	// The in-game Accept wrote the controls words and the bindings into the record: the live
	// bindings and the session's words take them, then the profile saves (OptionsScreen::ApplyControls).
	virtual void controls_accepted() {}
	// The in-game Accept on a dialog holding XHAIR_APPEARANCE: the crosshair art reloads in the HUD
	// loader's alpha mode (OptionsScreen::ReloadCrosshairAlpha, D-HUD-48).
	virtual void crosshair_reload() {}
	// The effects of one activation on an Options surface (OptionsScreen::Effect bits), after the
	// shell ran them.
	virtual void options_effects(int effects, const std::string &control) {
		(void)effects;
		(void)control;
	}

	// --- the Mods pick's reload, its device legs in the original's order ---
	// The pick that switches the game saves the player profile first, under the game it leaves.
	virtual void save_profile() {}
	// The menu's teardown: its cookie jar saved, the kept NovaWorld gate response forgotten.
	virtual void save_cookie_jar() {}
	virtual void forget_gate_response() {}
	// Every archive closed and the game `name` ("" the base game) mounted anew; on a failure the
	// game `previous` mounted again and false.
	virtual bool mount_game(const std::string &name, const std::string &previous) = 0;
	// The shell's resources read again from the mount: the text tables, the stylesheets, the menu
	// music context and the backdrop movies.
	virtual void reload_shell_resources() {}
	// The menu boots anew over the new game (the profile read again under it).
	virtual void game_reloaded() {}
};

class MenuShell {
public:
	MenuShell();
	~MenuShell();
	MenuShell(const MenuShell &) = delete;
	MenuShell &operator=(const MenuShell &) = delete;

	// The runtime the shell drives (the embedder sets its frame seam and version line).
	MenuRuntime &runtime() { return runtime_; }
	const MenuRuntime &runtime() const { return runtime_; }
	void set_host(MenuShellHost *host) { host_ = host; }
	// The embedder's own handling of every runtime event, after the shell's (sounds, the music var,
	// signals; Try's records).
	void set_sink(MenuEventSink sink) { sink_ = std::move(sink); }
	// The main menu and the in-game menu, by file name (JO's main.mnu and game.mnu).
	void set_menu_files(const std::string &main_menu, const std::string &ingame_menu);
	const std::string &main_menu_file() const { return main_menu_; }

	// --- the menu mode ---
	// The menu mode entered: from a mission's return on the same game (`root_changed` false after a
	// leave_menu_mode) the screen the mission was started from shows again, the history under it
	// kept; any other entry opens the main menu's first screen with an empty history. A changed
	// root reads every menu again. False when the menu shows nothing.
	bool enter_menu_mode(bool root_changed);
	// A mission's start leaves the menu: the screen it was started from kept under a mark. Once per
	// stay in the menu.
	void leave_menu_mode();
	// The in-game menu over a running mission: the in-game screens' earlier rows trimmed, the
	// in-game menu file opened.
	bool open_ingame();
	bool in_game() const { return in_game_; }
	bool in_menu_mode() const { return menu_mode_; }
	// Opens `file` at `screen` (empty: its first) and wires it. False (the screen kept, with a
	// notice) when the file does not load or holds no screens.
	bool open_menu(const std::string &file, const std::string &screen);
	// A document its embedder opened itself (a screen a mission opens by name): no shell wiring;
	// a claimed controller drives it (claim). Its document is the embedder's to keep alive.
	bool show_document(const mnu::Document *document, const std::string &file, const std::string &screen);
	// Every menu read again at its next open (the root changed under the shell).
	void forget_menus();
	// The menu file shown (as opened), "" before any.
	const std::string &file() const { return file_; }
	// Moves with every open (the shell's or the embedder's).
	uint64_t open_serial() const { return open_serial_; }

	// --- the runtime's events ---
	// The menu mode's frame tail: a Mods pick queued this frame reloads the game now
	// [orig: Menu_UpdateFrame @ 0x5528a0 -> Game_ReloadExpansionAndMods @ 0x552710].
	void frame_tail();
	bool has_pending_reload() const { return flow_.has_pending_expansion(); }

	// --- the update gate (ADR 0067 d3) ---
	// The host's answer changed: the gate applied again to the open document.
	void apply_update_gate();

	// --- screen controllers ---
	// A delegate the shell hands every document it opens whose owner (MenuCommands::owner) is the
	// controller's owner(); the first added of a name wins.
	void add_controller(MenuScreenController *controller);
	void remove_controller(MenuScreenController *controller);
	// The document open now is `controller`'s (opened with the shell's file and the current screen),
	// until the next open or release().
	bool claim(MenuScreenController *controller);
	void release();
	MenuScreenController *controller() const { return wired_; }

	// --- the Options screens ---
	// One device input while the remap capture runs (OptionsScreen::consume); Consumed when taken.
	int consume_options_input(const RemapInput &input);
	// The capture pump's joystick poll, once each menu frame.
	void poll_options_joystick(const controls::JoystickBits &bits);
	const OptionsScreen &options() const { return options_; }
	OptionsScreen &options() { return options_; }

	// --- readers ---
	const MenuCommands &commands() const { return commands_; }
	MenuFlow &flow() { return flow_; }
	const MenuFlow &flow() const { return flow_; }
	ModList &mods() { return mods_; }
	const std::string &selected_mission() const { return flow_.selected_mission(); }
	// The command bound to the control `id` on the screen holding it (None for none).
	MenuCommand command_of(int id) const;

private:
	std::shared_ptr<const mnu::Document> menu_(const std::string &file);
	void on_event_(const MenuEvent &event);
	void handle_(const MenuEvent &event);
	void wire_();
	void prepare_options_();
	void seed_lists_();
	void on_screen_changed_(const std::string &screen);
	void on_value_changed_(const MenuEvent &event);
	void on_activated_(const MenuEvent &event);
	void on_list_activated_(const MenuEvent &event);
	void on_menu_requested_(const std::string &file, const std::string &screen);
	bool pop_history_();
	void on_url_(const std::string &url, bool external);
	void run_command_(MenuCommand command, const std::string &control);
	void start_mission_(const std::string &control);
	void apply_expansion_(const std::string &control);
	void launch_update_(const std::string &control);
	void back_(const std::string &control);
	void reload_game_(const std::string &name);
	void release_();
	bool word_edit_(int id, int value);

	MenuRuntime runtime_;
	MenuShellHost *host_ = nullptr;
	MenuEventSink sink_;
	MenuCommands commands_;
	MenuFlow flow_;
	ModList mods_;
	OptionsScreen options_;
	// The Options dialog's preview baseline: the player's words as the dialog opened (or its last
	// in-game OK took them), which its Cancel and the front BACK put back.
	OptionsScreen::Words baseline_{};
	bool has_baseline_ = false;
	std::map<std::string, std::shared_ptr<const mnu::Document>> menus_; // by lower-case file name
	std::shared_ptr<const mnu::Document> shown_;
	std::string main_menu_ = "main.mnu";
	std::string ingame_menu_ = "game.mnu";
	std::string file_;
	bool entered_ = false; // enter_menu_mode ran before
	bool in_game_ = false;
	bool menu_mode_ = false;
	bool wired_by_shell_ = false; // the shell opened the document shown (its wiring answers)
	uint64_t open_serial_ = 0;
	std::vector<int> update_gated_; // the multiplayer entries the update gate disabled
	std::vector<MenuScreenController *> controllers_;
	MenuScreenController *wired_ = nullptr;
};

} // namespace opennova::menu
