#include <runtime/inmatch/pre_game_controller.h>

#include <base/io/strutil.h>
#include <runtime/menu/menu_runtime.h>

namespace opennova::inmatch {

namespace {

const char *const kMessages = "MESSAGES";
const char *const kGamePassword = "GAME_PASSWORD";
const char *const kSpectate = "SPECTATE";
const char *const kTeamPassword = "TEAM_PASSWORD";

// The join screen runs on the joining process, which is no authority (pre_game_entry_panel's and
// pre_game_accept's `authority`).
constexpr bool kJoiningAuthority = false;

std::string widget_text(const menu::MenuRuntime &menu, const char *name) {
	const int id = menu.widget_id(name);
	return id >= 0 ? menu.get_widget_text(id) : std::string();
}

bool widget_checked(const menu::MenuRuntime &menu, const char *name) {
	const int id = menu.widget_id(name);
	return id >= 0 && menu.is_widget_checked(id);
}

} // namespace

void PreGameController::open(menu::MenuRuntime &menu, const std::string &, const std::string &) {
	// [orig: PreMenu_Init @0x5693E0 — g_PreGamePanelState = 0 @0x5694bb]
	following_ = false;
	starting_shown_ = false;
	show_progress(menu, std::string());
}

void PreGameController::close(menu::MenuRuntime &) {
	following_ = false;
	starting_shown_ = false;
}

void PreGameController::on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) {
	if (event.kind != menu::MenuEvent::Kind::WidgetActivated)
		return;
	// [orig: PreMenu_OnAbortCancel @0x568CA0 on BACK (@0x569498) and ABORT_CANCEL (@0x5694b6);
	//  UI_PreGameMenuStateMachine @0x568A40 on ACCEPT (@0x56947a)]
	if (strutil::iequals(event.text, "ABORT_CANCEL") || strutil::iequals(event.text, "BACK")) {
		if (host_)
			host_->cancelled();
	} else if (strutil::iequals(event.text, "ACCEPT")) {
		accept(menu);
	}
}

void PreGameController::show_progress(menu::MenuRuntime &menu, const std::string &text) {
	set_panel_(menu, PreGamePanel::Progress);
	set_message_(menu, text);
}

void PreGameController::show_failure(menu::MenuRuntime &menu, const std::string &text) {
	following_ = false;
	set_panel_(menu, PreGamePanel::Error);
	set_message_(menu, text);
}

bool PreGameController::show_choice(menu::MenuRuntime &menu, uint32_t server_flags) {
	// [orig: UI_ShowPreGameMenuByState @0x568d10 — the arm @0x568dfc..0x568e44]
	const PreGamePanel entry = pre_game_entry_panel(server_flags, kJoiningAuthority);
	if (entry == PreGamePanel::Progress)
		return false;
	following_ = false;
	choice_flags_ = server_flags;
	set_panel_(menu, entry);
	return true;
}

void PreGameController::accept(menu::MenuRuntime &menu) {
	// [orig: UI_PreGameMenuStateMachine @0x568A40 — the walk, the abort @0x568b57, the commit
	//  @0x568b7e]
	const PreGameAccept step = pre_game_accept(
			panel_, choice_flags_, widget_checked(menu, kSpectate), kJoiningAuthority);
	if (step.abort) {
		if (host_)
			host_->cancelled();
		return;
	}
	if (!step.commit) {
		// ACCEPT on Progress does nothing.
		if (step.next != PreGamePanel::Progress)
			set_panel_(menu, step.next);
		return;
	}
	const std::string game_password = widget_text(menu, kGamePassword);
	const bool spectate = widget_checked(menu, kSpectate);
	const std::string team_password = widget_text(menu, kTeamPassword);
	set_panel_(menu, PreGamePanel::Progress);
	if (host_)
		host_->committed(game_password, spectate, team_password);
}

PreGameController::FollowStep PreGameController::follow_frame(
		menu::MenuRuntime &menu, bool has_status, JoinScreenStage stage, const std::string &text) {
	if (!following_ || panel_ != PreGamePanel::Progress)
		return FollowStep::Idle;
	// The start's line (PRE_CONNECTED, MultiPlayer_JoinSessionStateMachine @0x56a90b) shows for the
	// frame it was stamped on; the next hands over to the loading screen.
	if (starting_shown_) {
		following_ = false;
		starting_shown_ = false;
		return FollowStep::HandOff;
	}
	if (!has_status)
		return FollowStep::Idle;
	set_message_(menu, text);
	starting_shown_ = stage == JoinScreenStage::Starting;
	return FollowStep::Shown;
}

// [orig: UI_ShowPreGameMenuByState @0x568d10 — each window shown or hidden by the panel's set]
void PreGameController::set_panel_(menu::MenuRuntime &menu, PreGamePanel panel) {
	panel_ = panel;
	const uint32_t shown = pre_game_windows(panel);
	for (int bit = 0; bit < kPreGameWindowCount; ++bit) {
		const int id = menu.widget_id(pre_game_window_name(bit));
		if (id >= 0)
			menu.set_widget_shown(id, (shown & (1u << bit)) != 0);
	}
}

// The status line replaces MESSAGES' text; it never appends.
void PreGameController::set_message_(menu::MenuRuntime &menu, const std::string &text) {
	const int id = menu.widget_id(kMessages);
	if (id >= 0)
		menu.set_widget_text(id, text);
}

} // namespace opennova::inmatch
