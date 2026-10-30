#include <runtime/menu/death_screen.h>

#include <base/io/strutil.h>
#include <runtime/menu/menu_runtime.h>

namespace opennova::menu {

namespace {

constexpr const char *kSpawnList = "SPAWNPOINTS_LIST";
// The window holding the MAP, the list and every static.
constexpr const char *kShroud = "DEATH_SHROUD";
// The team-service pair, shown together; only SWAP_TEAMS has a DEATH callback.
constexpr const char *kSwapTeams = "SWAP_TEAMS";
constexpr const char *kTeamList = "BUTTON_TEAMLIST";
// The LEAVE MISSION button (its Esc hotkey shows CONFIRM_EXIT) and the confirmation's NO and YES.
constexpr const char *kHiddenBack = "HIDDEN_BACK";
constexpr const char *kConfirmNo = "CONFIRM_NO";
constexpr const char *kConfirmYes = "CONFIRM_YES";

} // namespace

void DeathScreen::set_text(hud::GameTextLookup gametext, hud::GameTextLookup menutxt,
		hud::GameTextLookup gameui) {
	gametext_ = std::move(gametext);
	menutxt_ = std::move(menutxt);
	gameui_ = std::move(gameui);
}

// A Menu token: menutxt's Menu section, then Game.bin's, else the readable fallback.
std::string DeathScreen::menu_text_(const char *key, const char *fallback) const {
	const std::string gameui = hud::game_text(gameui_, "Menu", key, fallback);
	return hud::game_text(menutxt_, "Menu", key, gameui.c_str());
}

bool DeathScreen::can_open() const {
	return host_ != nullptr && host_->held();
}

bool DeathScreen::initial_overlay_() const {
	return host_ != nullptr && !host_->pick_pending() && !host_->local_player_dead();
}

void DeathScreen::set_shroud_shown_(MenuRuntime &menu, bool shown) {
	const int id = menu.widget_id(kShroud);
	if (id >= 0) menu.set_widget_shown(id, shown);
}

// The swap/team pair's show rule is the engine's (world::deploy_team_buttons_shown)
// [orig: DeathScreen_UpdateUI @0x553489 / @0x5534AA].
void DeathScreen::apply_team_buttons_(MenuRuntime &menu, bool shown) {
	for (const char *name : { kSwapTeams, kTeamList }) {
		// A -1 id means this screen does not author the control.
		const int id = menu.widget_id(name);
		if (id >= 0) menu.set_widget_shown(id, shown);
	}
}

// The show: the team-service pair by the session's rule, the shroud hidden (the per-frame reveal
// shows it and runs the content refresh), the input pass-through word raised, the tick stamped
// [orig: DeathScreen_UpdateUI @0x553150 — CWnd_SetShown(DEATH_SHROUD, 0) @0x55325f].
void DeathScreen::open(MenuRuntime &menu, const std::string &file, const std::string &screen) {
	(void)file;
	(void)screen;
	if (host_ == nullptr) return;
	apply_team_buttons_(menu, host_->status().team_buttons_shown);
	set_shroud_shown_(menu, false);
	pass_through_ = true;
	last_tick_ = host_->logic_tick();
}

DeathScreen::Frame DeathScreen::frame(MenuRuntime &menu, bool scene_drawn) {
	if (host_ == nullptr) return Frame::Close;
	if (host_->session_lost()) return Frame::Teardown;
	// Only every trigger falling closes the screen, and on a drawn scene frame: a frame a listen
	// host's status page stood in for closes nothing.
	if (!host_->held() && scene_drawn) return Frame::Close;
	// The shroud reveal and, inside it, the content refresh on each 16-tick boundary
	// [orig: DeathScreen_UpdateShroudReveal @0x554730 — CWnd_SetShown(DEATH_SHROUD, 1) @0x554771,
	//  (g_CurrentTick & 0xF) == 0 -> UI_UpdateDeathScreenContent @0x55477d].
	const int64_t tick = host_->logic_tick();
	if (host_->shroud_revealed()) {
		set_shroud_shown_(menu, true);
		if (world::deploy_refresh_due(last_tick_, tick)) refresh_(menu);
	}
	last_tick_ = tick;
	return Frame::Keep;
}

bool DeathScreen::pick(int param) {
	if (host_ == nullptr) return false;
	// Input case 12 resets the dialogs and queues the request even for an alive player; the screen
	// closes on the send only under the initial overlay [orig: Input_HandleActionBinding case 12].
	const bool initial = initial_overlay_();
	return host_->send_pick(param) && initial;
}

void DeathScreen::reset() {
	rows_.clear();
	last_tick_ = 0;
}

// The list refill and the statics [orig: UI_UpdateDeathScreenContent @0x5536a0]: the rows the
// engine builder compiles (world::build_deploy_rows — the Default row, the secured team zones, the
// whole-list text sort, then each zone's wave occupants and blank separator at node -1, each row's
// text in retail's team colour tag; @0x553aef..0x553de3), the list showing them with their tags
// stripped (D-HUD-19), and the pick kept across the refill by its PARAMETER, not its row: a zone's
// security or ownership change can reshuffle the rows. The refill's own select emits nothing:
// picks ride the player's clicks alone.
void DeathScreen::refresh_(MenuRuntime &menu) {
	const world::DeployScreenStatus status = host_->status();
	apply_statics_(menu, status);
	if (status.instructions.permanent_death) return;
	const int list = menu.widget_id(kSpawnList);
	if (list < 0) return;
	int keep = -1;
	const int selected = menu.selected_row(list);
	if (selected >= 0 && selected < static_cast<int>(rows_.size())) keep = rows_[selected].param;
	// [orig: GameText_GetString(WPNames, "STRWPNAME%03d") @0x5536a0]
	const auto zone_name = [this](const std::string &key) {
		if (key.empty()) return std::string("Spawn Point");
		return hud::game_text(gametext_, hud::kGameTextWPNames, key.c_str(), "Spawn Point");
	};
	const std::vector<world::DeployListRow> compiled = host_->list_rows(
			menu_text_("DEFAULT_SPAWN_KEY", "D"), menu_text_("HOME", "Home Base"), zone_name);
	const std::vector<std::string> items = world::deploy_list_items(compiled);
	rows_.clear();
	rows_.reserve(compiled.size());
	for (size_t i = 0; i < compiled.size(); ++i) rows_.push_back({ items[i], compiled[i].value });
	menu.set_widget_items(list, items);
	if (keep < 0) return;
	for (size_t row = 0; row < rows_.size(); ++row)
		if (rows_[row].param == keep) {
			menu.select_row(list, static_cast<int>(row), false);
			return;
		}
}

// The witnessed STATIC show and text rules, refreshed with the list
// [orig: UI_UpdateDeathScreenContent @0x5536a0]: the two instruction widgets (a retained first
// text when the build keeps it); under permanent death the round status pair alone; else
// STATIC_RESPAWN_MSG1 (@0x5538e7..0x553a7b) hidden but for the penalty timer or the wave zone
// listing the local player, STATIC_LIST_TITLE shown with the list (@0x553ab4),
// STATIC_PSPRESPAWN_MSG1 (@0x553e10) while the spawn-target hold runs, and STATIC_MEDIC_MSG1 +
// STATIC_CALLMEDIC_MSG (@0x553e74..0x553f60) while the local revive window runs and the player
// is not in a seat. The texts are the feed's (world::deploy_statics_text, deploy_status_text,
// build_deploy_instructions).
void DeathScreen::apply_statics_(MenuRuntime &menu, const world::DeployScreenStatus &status) {
	apply_team_buttons_(menu, status.team_buttons_shown);
	const world::DeployInstructions &instructions = status.instructions;
	const int first = menu.widget_id("STATIC_INSTRUCTIONS_MSG");
	const int second = menu.widget_id("STATIC_INSTRUCTIONS2_MSG");
	if (first >= 0 && second >= 0) {
		menu.set_widget_shown(first, instructions.show_first);
		menu.set_widget_shown(second, instructions.show_second);
		if (instructions.replace_first) menu.set_widget_text(first, instructions.first_text);
		menu.set_widget_text(second, instructions.second_text);
	}
	if (instructions.permanent_death) {
		const int round = menu.widget_id("STATIC_RESPAWN_MSG1");
		const int remaining = menu.widget_id("STATIC_PSPRESPAWN_MSG1");
		if (round >= 0 && remaining >= 0) {
			menu.set_widget_shown(round, instructions.show_round_status);
			menu.set_widget_shown(remaining, instructions.show_round_status);
			if (instructions.show_round_status) {
				menu.set_widget_text(round, instructions.round_text);
				menu.set_widget_text(remaining, instructions.remaining_players_text);
			}
		}
		return;
	}
	const int title = menu.widget_id("STATIC_LIST_TITLE");
	if (title >= 0) menu.set_widget_shown(title, true);
	const int respawn = menu.widget_id("STATIC_RESPAWN_MSG1");
	if (respawn >= 0) {
		const bool queued = status.line.kind != world::DeployStatusLine::Kind::None;
		menu.set_widget_shown(respawn, queued);
		if (queued) menu.set_widget_text(respawn, status.respawn_text);
	}
	const int psp = menu.widget_id("STATIC_PSPRESPAWN_MSG1");
	if (psp >= 0) {
		menu.set_widget_shown(psp, status.statics.psp_respawn);
		if (status.statics.psp_respawn) menu.set_widget_text(psp, status.statics_text.psp_respawn);
	}
	const int medic = menu.widget_id("STATIC_MEDIC_MSG1");
	const int call = menu.widget_id("STATIC_CALLMEDIC_MSG");
	if (medic >= 0 && call >= 0) {
		menu.set_widget_shown(medic, status.statics.medic);
		menu.set_widget_shown(call, status.statics.medic);
		if (status.statics.medic) {
			menu.set_widget_text(medic, status.statics_text.medic_timer);
			menu.set_widget_text(call, status.statics_text.call_medic);
		}
	}
}

void DeathScreen::on_event(MenuRuntime &menu, const MenuEvent &event) {
	(void)menu;
	if (host_ == nullptr) return;
	if (event.kind == MenuEvent::Kind::ValueChanged) {
		// Every click on a row queues a pick, the row already highlighted among them: the select
		// callback is a COMMAND fired from the list event with no "did the selection change" test,
		// guarded only on node != -1 (an occupant or the blank separator) [orig:
		// DeathScreen_OnSpawnListSelect @0x553630 -> Input_QueueEvent(12, node) @0x55364d; node 0,
		// the Default Spawn, is the parameter-0 pick].
		if (event.text2 != "list" || !strutil::iequals(event.text, kSpawnList)) return;
		if (event.value < 0 || event.value >= static_cast<int>(rows_.size())) return;
		const int param = rows_[event.value].param;
		if (param == -1) return;
		if (pick(param)) host_->close_screen();
		return;
	}
	if (event.kind != MenuEvent::Kind::WidgetActivated) return;
	// HIDDEN_BACK and CONFIRM_NO lower and raise the pass-through word [orig:
	// DeathScreen_OnHiddenBack @0x553660 -> UI_SetMenuInputPassThrough(0) @0x55366c;
	// DeathScreen_OnConfirmNo @0x553680 -> UI_SetMenuInputPassThrough(1) @0x55368c].
	if (strutil::iequals(event.text, kHiddenBack)) {
		pass_through_ = false;
		return;
	}
	if (strutil::iequals(event.text, kConfirmNo)) {
		pass_through_ = true;
		return;
	}
	// CONFIRM_YES closes the in-game screens, then the shell runs input action 3 [orig:
	// DeathScreen_OnConfirmYes @0x553530 — Game_CloseInGameScreens @0x55353a,
	// Input_HandleActionBinding(3) @0x553549].
	if (strutil::iequals(event.text, kConfirmYes)) {
		host_->close_screen();
		host_->exit_confirmed();
		return;
	}
	// SWAP_TEAMS asks the host for the other side (C2S 0x4D; the engine's
	// ClientRuntime::queue_team_change_request carries the witness) [orig: DeathScreen_OnSwapTeams
	// @0x5535b0].
	if (strutil::iequals(event.text, kSwapTeams)) host_->send_team_change();
}

} // namespace opennova::menu
