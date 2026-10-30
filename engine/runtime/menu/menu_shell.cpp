#include <runtime/menu/menu_shell.h>

#include <algorithm>

#include <base/io/strutil.h>

namespace opennova::menu {

namespace {

using Kind = MenuEvent::Kind;

// The LAN multiplayer entry (an authored SCREEN row to mp.mnu), which the update gate disables beside
// the NovaWorld controls while an update waits.
const char *const kLanEntry = "LAN_MULTI_PLAYER";
// The updater's button, on the STARTUP and OPTIONS screens (menu_commands.h).
const char *const kUpdateControl = "UPDATE";

bool holds(const std::vector<int> &ids, int id) {
	return id >= 0 && std::find(ids.begin(), ids.end(), id) != ids.end();
}

// The file's own name, the shipped same-file jumps compare against (mp.mnu names itself).
std::string base_name(const std::string &file) {
	const size_t slash = file.find_last_of("/\\");
	return slash == std::string::npos ? file : file.substr(slash + 1);
}

} // namespace

MenuShell::MenuShell() {
	runtime_.set_sink([this](const MenuEvent &event) { on_event_(event); });
	// The single-player screen's controls and the Mods list's descriptions, the engine's name sets.
	flow_.set_mission_controls(menu_name_set(MenuNameSet::SinglePlayerLists), menu_name_set(MenuNameSet::Briefings),
			menu_name_set(MenuNameSet::SinglePlayerAccepts));
	mods_.set_descriptions(menu_name_set(MenuNameSet::ModDescriptions));
}

MenuShell::~MenuShell() {
	runtime_.set_sink(nullptr);
}

void MenuShell::set_menu_files(const std::string &main_menu, const std::string &ingame_menu) {
	main_menu_ = main_menu;
	ingame_menu_ = ingame_menu;
}

// --- the menu mode ------------------------------------------------------------------------------

bool MenuShell::enter_menu_mode(bool root_changed) {
	const bool reentry = entered_ && !root_changed && !menu_mode_;
	if (root_changed) forget_menus();
	entered_ = true;
	in_game_ = false;
	menu_mode_ = true;
	if (!reentry) {
		runtime_.screen_history().clear();
		return open_menu(main_menu_, std::string());
	}
	// The return from a mission shows again the screen it was started from, the history under it kept
	// (D-MNU-28) [orig: Menu_TeardownShellAndCloseBinkVideos @ 0x54e430 - the mission's leave marks the
	// history (UIScene_MarkScreenHistory called @ 0x54e514)].
	ScreenHistoryRow row;
	if (runtime_.return_to_menu_mode(&row) && open_menu(row.file, row.screen)) return true;
	return open_menu(main_menu_, std::string());
}

void MenuShell::leave_menu_mode() {
	if (!menu_mode_ || !entered_) return;
	menu_mode_ = false;
	runtime_.leave_menu_mode();
}

bool MenuShell::open_ingame() {
	in_game_ = true;
	// The in-game screens' rows from an earlier visit go, as the original's close of an in-game screen
	// drops them; the screen the mission was started from stays under its mark, which a back here never
	// pops past.
	runtime_.trim_screen_history();
	return open_menu(ingame_menu_, std::string());
}

void MenuShell::forget_menus() {
	menus_.clear();
}

std::shared_ptr<const mnu::Document> MenuShell::menu_(const std::string &file) {
	if (file.empty() || !host_) return nullptr;
	const std::string key = strutil::to_lower(file);
	const auto held = menus_.find(key);
	if (held != menus_.end()) return held->second;
	std::shared_ptr<const mnu::Document> document = host_->read_menu(file);
	// A file the game does not find is read again at its next open (the root may gain it).
	if (document) menus_[key] = document;
	return document;
}

void MenuShell::release_() {
	if (wired_) {
		MenuScreenController *const released = wired_;
		wired_ = nullptr;
		released->close(runtime_);
	}
}

bool MenuShell::open_menu(const std::string &file, const std::string &screen) {
	std::shared_ptr<const mnu::Document> document = menu_(file);
	if (!document) {
		if (host_) host_->notice(ShellNotice::MenuMissing, std::string(), file, std::string());
		return false;
	}
	file_ = file;
	flow_.clear_selected_mission();
	// The new document's widget state starts fresh: nothing in it is the update gate's yet, and nothing
	// is bound on it until the wiring below.
	update_gated_.clear();
	commands_ = MenuCommands();
	wired_by_shell_ = true;
	++open_serial_;
	shown_ = document;
	opening_ = true;
	const bool shown = runtime_.open_document(document.get(), base_name(file), screen);
	opening_ = false;
	if (!shown) {
		// The runtime may already hold the document: nothing a controller parked on it can stand.
		release_();
		if (host_) host_->notice(ShellNotice::MenuMissing, std::string(), file, std::string());
		return false;
	}
	prepare_options_();
	wire_();
	return true;
}

bool MenuShell::show_document(const mnu::Document *document, const std::string &file, const std::string &screen) {
	commands_ = MenuCommands();
	update_gated_.clear();
	wired_by_shell_ = false;
	++open_serial_;
	shown_.reset();
	file_ = file;
	opening_ = true;
	const bool shown = runtime_.open_document(document, file, screen);
	opening_ = false;
	// The previous document's controller lets go: the embedder claims this one for its own.
	release_();
	return shown;
}

// After each open the engine wires the document: the commands the code binds on it (MenuCommands::wire:
// the screens' own registrations, the delegate that owns it, the lists the game fills and the
// launch/quit controls), the update gate, then the delegate's populate, or the lists the shell fills.
// [orig: CUIScene_RegisterControlCallback @ 0x63c060 -> CUIScene_BindControlCallbacks @ 0x63af80, the
//  rows each screen's INIT registers]
void MenuShell::wire_() {
	commands_.wire(runtime_, in_game_);
	apply_update_gate();
	flow_.clear_rows();
	// A delegate owns a whole document by the controls it holds: the controller of that name populates
	// it and the shell's lists stay out. The one wired before lets go when it loses the document.
	MenuScreenController *claimant = nullptr;
	if (!commands_.owner().empty())
		for (MenuScreenController *controller : controllers_)
			if (controller && commands_.owner() == controller->owner()) {
				claimant = controller;
				break;
			}
	if (wired_ != claimant) release_();
	wired_ = claimant;
	if (claimant) {
		claimant->open(runtime_, file_, runtime_.current_screen());
		return;
	}
	seed_lists_();
}

void MenuShell::seed_lists_() {
	if (!host_) return;
	// The catalog is the host's; filtering, selection and ACCEPT live in the flow.
	if (!commands_.mission_lists().empty()) {
		const std::vector<MissionChoice> rows = host_->missions();
		for (const int id : commands_.mission_lists()) flow_.seed_missions(runtime_, id, rows);
	}
	// The Mods lists: the base game's row, then the game folder's expansions, the one running
	// highlighted and described (mod_list.h).
	if (!commands_.mod_lists().empty()) {
		mods_.set_records(host_->expansions());
		const std::string running = host_->expansion();
		for (const int id : commands_.mod_lists()) mods_.populate(runtime_, id, running);
	}
}

// --- the Options screens -------------------------------------------------------------------------

// Every options-owned control rebuilt over the profile's current record, after each open and on every
// show of the front OPTIONS screen (OptionsScreen::is_front_screen): the engine's policy, the record's
// controls words, then the player's options, whose words as they stand are the dialog's preview
// baseline. Presence-gated: off an options surface nothing moves.
void MenuShell::prepare_options_() {
	playersav::ProfileRecord *const record = host_ ? host_->profile_record() : nullptr;
	options_.prepare(runtime_, record);
	if (!options_.is_surface()) return;
	options_.apply_policy(runtime_);
	options_.seed_profile(runtime_, record);
	OptionsScreen::Words words{};
	if (host_ && host_->player_words(words)) {
		baseline_ = words;
		has_baseline_ = true;
		options_.seed_words(runtime_, words);
	}
}

// A control's edit, `value` the edit's own (-1 for a click), into the player's options
// (OptionsScreen::edit_word), applied and saved at once (D-MNU-19); false when the control shows none.
bool MenuShell::word_edit_(int id, int value) {
	if (!host_ || id < 0) return false;
	OptionsScreen::Words words{};
	if (!host_->player_words(words)) return false;
	if (!options_.edit_word(runtime_, id, value, words)) return false;
	host_->set_player_words(words);
	return true;
}

int MenuShell::consume_options_input(const RemapInput &input) {
	return options_.consume(runtime_, input);
}

void MenuShell::poll_options_joystick(const controls::JoystickBits &bits) {
	// The menu frame's capture pump: an armed row on the joystick's page polls the device's state every
	// frame, so a button already down when the row armed is captured too.
	if (options_.capturing_joystick()) (void)options_.poll_joystick(runtime_, bits);
}

// --- the update gate -------------------------------------------------------------------------------

// The update gate (ADR 0067 d3 as amended 2026-10-28, a divergence from retail, whose UPDATE is always
// there and whose multiplayer is never held back): the updater's control shows only while its check
// found an update waiting, and the multiplayer entries are disabled while one waits. An unknown answer
// gates nothing. Widget state is the open document's, so each open applies it again (its screen change
// and its wiring); a control the gate disabled is enabled back once the answer changes, and no other.
void MenuShell::apply_update_gate() {
	const bool waiting = host_ && host_->update_waiting();
	const int update = runtime_.widget_id(kUpdateControl);
	if (update >= 0 && command_of(update) == MenuCommand::Update) {
		runtime_.set_widget_shown(update, waiting);
		runtime_.set_widget_disabled(update, !waiting);
	}
	if (!waiting) {
		for (const int id : update_gated_) runtime_.set_widget_disabled(id, false);
		update_gated_.clear();
		return;
	}
	std::vector<std::string> names = menu_name_set(MenuNameSet::NovaWorld);
	names.push_back(kLanEntry);
	for (const std::string &name : names) {
		const int id = runtime_.widget_id(name);
		if (id >= 0 && !holds(update_gated_, id) && !runtime_.is_widget_disabled(id)) {
			runtime_.set_widget_disabled(id, true);
			update_gated_.push_back(id);
		}
	}
}

// --- screen controllers ----------------------------------------------------------------------------

void MenuShell::add_controller(MenuScreenController *controller) {
	if (controller && std::find(controllers_.begin(), controllers_.end(), controller) == controllers_.end())
		controllers_.push_back(controller);
}

void MenuShell::remove_controller(MenuScreenController *controller) {
	if (wired_ == controller) release_();
	controllers_.erase(std::remove(controllers_.begin(), controllers_.end(), controller), controllers_.end());
}

bool MenuShell::claim(MenuScreenController *controller) {
	if (!controller || !runtime_.document()) return false;
	if (wired_ != controller) release_();
	wired_ = controller;
	controller->open(runtime_, file_, runtime_.current_screen());
	return true;
}

void MenuShell::release() {
	release_();
}

MenuCommand MenuShell::command_of(int id) const {
	if (id < 0) return MenuCommand::None;
	return commands_.command_of(runtime_.widget_screen_of(id), runtime_.widget_name_of(id));
}

// --- the runtime's events --------------------------------------------------------------------------

void MenuShell::on_event_(const MenuEvent &event) {
	MenuScreenController *const holder = wired_;
	const uint64_t serial = open_serial_;
	if (wired_by_shell_) handle_(event);
	// The controller holding the document hears it after the shell, while it still holds it: an event
	// the shell answered with another open is not the new document's. The open's own events are its
	// controller's open() (its INIT).
	if (holder && !opening_ && holder == wired_ && serial == open_serial_) holder->on_event(runtime_, event);
	if (sink_) sink_(event);
}

void MenuShell::handle_(const MenuEvent &event) {
	switch (event.kind) {
	case Kind::ScreenChanged:
		on_screen_changed_(event.text);
		return;
	case Kind::MenuRequested:
		on_menu_requested_(event.text, event.text2);
		return;
	case Kind::PopRequested:
		pop_history_();
		return;
	case Kind::UrlRequested:
		on_url_(event.text, event.flag);
		return;
	case Kind::ServiceRequested:
		if (host_) host_->service(event);
		// An APPMSG row's message, which the game answers on any screen (app_message_command).
		if (event.text == "APPMSG" && app_message_command(event.text2) == MenuCommand::Update)
			launch_update_(event.id >= 0 ? runtime_.widget_name_of(event.id) : std::string());
		return;
	case Kind::FilterRequested:
		if (host_) host_->service(event);
		return;
	case Kind::ValueChanged:
		on_value_changed_(event);
		return;
	case Kind::WidgetActivated:
		on_activated_(event);
		return;
	case Kind::ListActivated:
		on_list_activated_(event);
		return;
	case Kind::TableCellClicked:
		// A single press on a table row: the controls table's ends a running capture and arms only when
		// no row is left selected (OptionsScreen::press, which answers its own table alone).
		if (!event.flag) options_.press(runtime_, event.id, event.value);
		return;
	case Kind::MusicVar:
	case Kind::Sound:
	case Kind::HoverChanged:
	case Kind::ShownChanged:
	case Kind::EditCommitted:
		return;
	}
}

void MenuShell::on_screen_changed_(const std::string &screen) {
	apply_update_gate();
	// A screen or document switch drops an armed table selection. Every show of the front OPTIONS
	// screen runs its init again (OptionsScreen::is_front_screen carries the witness): the controls seed
	// from the saved words and the player's options as they stand are the visit's baseline, so an
	// accepted edit is what a later BACK keeps and a discarded one is not shown again. A document's open
	// shows its screen first; the open's own preparation after it repeats this harmlessly.
	options_.end_remap(runtime_, true);
	if (OptionsScreen::is_front_screen(screen)) prepare_options_();
}

void MenuShell::on_value_changed_(const MenuEvent &event) {
	const int id = runtime_.widget_id(event.text);
	if (event.text2 == "list") {
		// A pick of a mission list selects its mission, of a Mods list describes its game.
		if (holds(commands_.mission_lists(), id)) flow_.select_mission(runtime_, id, event.value, event.text3);
		else if (holds(commands_.mod_lists(), id)) mods_.select(runtime_, id, event.value);
	}
	if (options_.is_surface()) word_edit_(id, event.value);
}

void MenuShell::on_activated_(const MenuEvent &event) {
	const std::string control = event.text;
	// The command the game binds on the control, by the screen that holds it.
	const MenuCommand command = command_of(event.id);
	// The Options surface's own controls first (the device radios, DEFAULTS, CLEAR_KEY, its ACCEPT and
	// BACK, the in-game OK and Cancel), as retail's ACCEPT writes the options ahead of its Mods leg
	// [orig: Options_HandleAcceptOrBack @ 0x55a710 - the front ACCEPT @ 0x55ab5f..0x55ace5, the list's
	//  leg @ 0x55acea..0x55ad5b].
	if (options_.is_surface()) {
		// A check box's click (XHAIR_SPREAD, the tip pair) writes its word.
		word_edit_(event.id, -1);
		const int effects = options_.activate(runtime_, host_ ? host_->profile_record() : nullptr, event.id);
		if (host_ && (effects & OptionsScreen::ApplyControls)) host_->controls_accepted();
		if (host_ && (effects & OptionsScreen::ReloadCrosshairAlpha)) host_->crosshair_reload();
		// The in-game OK keeps what the dialog holds as the next baseline; its Cancel puts the baseline
		// back; both show the in-game menu again. The front BACK keeps no widget edit: the options,
		// applied and saved per edit (D-MNU-19), return to the baseline, and the BACK's pop leaves the
		// widgets to the next open's seed.
		if (effects & OptionsScreen::CommitPreview) {
			OptionsScreen::Words words{};
			if (host_ && host_->player_words(words)) {
				baseline_ = words;
				has_baseline_ = true;
			}
			OptionsScreen::show_ingame_main(runtime_);
		}
		if (effects & OptionsScreen::RestorePreview) {
			if (host_ && has_baseline_) {
				host_->set_player_words(baseline_);
				OptionsScreen::Words words{};
				if (host_->player_words(words)) options_.seed_words(runtime_, words);
			}
			OptionsScreen::show_ingame_main(runtime_);
		}
		if ((effects & OptionsScreen::DiscardEdits) && host_ && has_baseline_) host_->set_player_words(baseline_);
		if (host_ && effects != OptionsScreen::None) host_->options_effects(effects, control);
	}
	run_command_(command, control);
}

void MenuShell::on_list_activated_(const MenuEvent &event) {
	// A double click: a mission list's launches its row (the Mods list's does nothing: the game takes
	// its select alone, ModList::select); the controls table's arms the remap.
	if (holds(commands_.mission_lists(), event.id)) {
		flow_.activate_mission(event.id, event.value);
		start_mission_(runtime_.widget_name_of(event.id));
		return;
	}
	options_.arm(runtime_, event.id, event.value);
}

void MenuShell::run_command_(MenuCommand command, const std::string &control) {
	if (!host_) return;
	switch (command) {
	case MenuCommand::None:
		return;
	case MenuCommand::Update:
		launch_update_(control);
		return;
	case MenuCommand::StartMission:
		start_mission_(control);
		return;
	case MenuCommand::ApplyExpansion:
		apply_expansion_(control);
		return;
	case MenuCommand::Exit:
		host_->leave(ShellExit::Exit, command, control, "the main menu's EXIT");
		return;
	case MenuCommand::ReturnToMenu:
		host_->leave(ShellExit::ReturnToMenu, command, control, std::string());
		return;
	case MenuCommand::Restart:
		host_->leave(ShellExit::Restart, command, control, std::string());
		return;
	case MenuCommand::NovaWorld:
		// Out of date, NovaWorld waits for the update (the update gate); a press the disabled control
		// could not take (a scripted activate) does nothing either.
		if (host_->update_waiting()) return;
		host_->leave(ShellExit::NovaWorld, command, control, std::string());
		return;
	case MenuCommand::Back:
		back_(control);
		return;
	default:
		// A delegate's own command (the LAN screens', PLAYER_INFO's, an in-mission screen's): its
		// controller runs it on the control's event.
		host_->delegate_command(command, control);
		return;
	}
}

// The retail ACCEPT launches only through a shown mission list's current entry (or the IA_LIST's
// selected value); with nothing selected it does nothing (docs/mnu/menu-re.md, SINGLE_PLAYER). No
// catalog fallback.
void MenuShell::start_mission_(const std::string &control) {
	const std::string mission = flow_.selected_mission();
	if (mission.empty()) {
		host_->notice(ShellNotice::NothingSelected, control, std::string(), std::string());
		return;
	}
	host_->leave(ShellExit::StartMission, MenuCommand::StartMission, control, mission);
}

// OK/ACCEPT on a Mods screen: the game switches to the highlighted row's game rather than launching a
// mission. The pick of the game running takes nothing; another is queued for the frame's tail
// (frame_tail), and a pick that switches the game saves the player profile first, under the game it
// leaves (the profile's save points, runtime/profile/player_profiles.h).
void MenuShell::apply_expansion_(const std::string &control) {
	if (commands_.mod_lists().empty()) return;
	const std::string pick = mods_.pick(runtime_, commands_.mod_lists().front());
	switch (flow_.request_expansion(pick, host_->expansion(), host_->packed_install())) {
	case MenuFlow::ExpansionPick::NeedsPackedRoot:
		host_->notice(ShellNotice::ExpansionNeedsPackedInstall, control, pick, std::string());
		return;
	case MenuFlow::ExpansionPick::Ignored:
		host_->notice(ShellNotice::ExpansionUnchanged, control, pick, std::string());
		return;
	case MenuFlow::ExpansionPick::Queued:
		host_->save_profile();
		host_->notice(ShellNotice::ExpansionQueued, control, pick, std::string());
		return;
	}
}

// The updater runs whether or not an update is waiting (it finds that out itself), and the game quits
// once it started; a launch that fails leaves the menu as it was (menu_commands.h app_message_command).
void MenuShell::launch_update_(const std::string &control) {
	if (!host_) return;
	if (host_->launch_update(control))
		host_->leave(ShellExit::Exit, MenuCommand::Update, control, "the updater started");
	else
		host_->notice(ShellNotice::UpdateNotStarted, control, std::string(), std::string());
}

// The back control (HIDDEN_BACK, the retail per-control callback): the history across files first, then
// the mission's resume (in one) or the quit to the desktop. In a mission the history's top is the mark on
// the screen the mission was started from, which pops nothing (D-MNU-28)
// [orig: UI_IngameBackResumeCommand @ 0x555490].
void MenuShell::back_(const std::string &control) {
	if (pop_history_()) return;
	if (in_game_) host_->leave(ShellExit::Resume, MenuCommand::Back, control, std::string());
	else host_->leave(ShellExit::Exit, MenuCommand::Back, control, "the main menu's BACK with no screen behind it");
}

// POP_SCREEN with the document's own history empty: the previous file's screen, the one history retail
// keeps across every loaded file; with none, nothing happens (retail pops only a non-empty history;
// docs/mnu/menu-re.md). True when a row popped.
bool MenuShell::pop_history_() {
	ScreenHistoryRow row;
	if (!runtime_.screen_history().pop(&row)) return false;
	open_menu(row.file.empty() ? main_menu_ : row.file, row.screen);
	return true;
}

// A forward jump to another file: the screen it leaves goes on the history. A file that does not load,
// or a target screen it does not hold, changes nothing: retail's select finds no such screen and keeps
// the current one, its history untouched (docs/mnu/menu-re.md, "Activation and the ACTION walk").
void MenuShell::on_menu_requested_(const std::string &file, const std::string &screen) {
	const std::shared_ptr<const mnu::Document> document = menu_(file);
	if (!document) {
		if (host_) host_->notice(ShellNotice::MenuMissing, std::string(), file, std::string());
		return;
	}
	if (!screen.empty() && !document->find_screen(screen)) {
		if (host_) host_->notice(ShellNotice::ScreenMissing, std::string(), file, screen);
		return;
	}
	const std::string from_file = file_;
	const std::string from_screen = runtime_.current_screen();
	if (open_menu(file, screen)) runtime_.screen_history().push(from_file, from_screen);
}

// A URL row: with the external browser (EXTERNAL_BROWSER, or external_browser=1 / commercial_browser=1)
// the menu's cookie jar saves first and the browser opens on it [orig: UIScene_OpenUrl @ 0x63e2f0 - the
// external open's save called @ 0x63e3cb]; without it, retail's in-game page fetch, which the game does
// not host (docs/mnu/menu-re.md).
void MenuShell::on_url_(const std::string &url, bool external) {
	if (url.empty() || !host_) return;
	if (!external) {
		host_->fetch_page(url);
		return;
	}
	host_->save_cookie_jar();
	host_->open_browser(url);
}

// --- the Mods pick's reload ------------------------------------------------------------------------

// The request waits while the Options modal runs (its remap capture: OpenNova pins the VIDEO rows,
// so no resolution query ever runs, D-MNU-21) [orig: Menu_UpdateFrame @ 0x5528a0 - the request flag
// and g_OptionsModalState tested @ 0x552916, the reload called @ 0x552918, the flag cleared
// @ 0x55291d].
void MenuShell::frame_tail() {
	if (!flow_.has_pending_expansion() || options_.capturing()) return;
	reload_game_(flow_.take_expansion_reload());
}

// The game switched to the pick ("" the base game) and the menu booted anew over it: the menu destroyed
// (its cookie jar saved, the kept NovaWorld gate response forgotten), every archive closed and the pick
// mounted, the shell's resources made anew (the menus and their text read again, the music and the
// backdrop movies the new game's), the screen history dropped and the main menu's first screen shown,
// whose MUSICVAR the open pushes. The pick lasts the run: nothing persists it, the next launch boots
// `/exp` or the base game (D-MNU-31). A failed mount mounts the previous game again.
// [orig: Game_ReloadExpansionAndMods @ 0x552710 - g_MenuDestroyOnTeardown set @ 0x552756 around
//  Menu_TeardownShellAndCloseBinkVideos @ 0x54e430 (the jar saved @ 0x54e4aa, the gate response
//  forgotten @ 0x54e4c8); the archives closed @ 0x552770 and opened again @ 0x552798;
//  Menu_InitShellResources @ 0x552500 called @ 0x55281a (main.mnu @ 0x552656, its start screen
//  @ 0x552668); the new screen's music var (sub_639610 @ 0x639610) into AudioVM_SetVariable(2)
//  @ 0x55287a]
void MenuShell::reload_game_(const std::string &name) {
	if (!host_) return;
	const std::string previous = host_->expansion();
	host_->save_cookie_jar();
	host_->forget_gate_response();
	if (!host_->mount_game(name, previous)) return;
	forget_menus();
	host_->reload_shell_resources();
	runtime_.screen_history().clear();
	open_menu(main_menu_, std::string());
	host_->game_reloaded();
}

} // namespace opennova::menu
