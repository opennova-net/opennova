#include <runtime/inmatch/end_round_screen.h>

#include <base/io/strutil.h>
#include <runtime/menu/menu_runtime.h>

namespace opennova::inmatch {

namespace {

constexpr const char *kResultList = "RESULTLIST";
// The tab radios in the filter's tab order (0 all, 1 team 2, 2 team 1).
constexpr const char *kTabs[] = { "RADIO_TAB_OVERALL", "RADIO_TAB_REDTEAM", "RADIO_TAB_BLUETEAM" };
constexpr const char *kConfirmYes = "CONFIRM_YES";

} // namespace

// The STAT show callback [orig: StatScreen_ShowCallback @0x562840]: the RESULTLIST filled
// (StatScreen_PopulateStatResultsList @0x562240, the call @0x562840: the columns the fill defines,
// stat.mnu's RESULTLIST authoring none, their width split read off the table's design rect, every
// row with its team colour, the sort, the local slot's row selected; inmatch fill_stat_results),
// then by the game type's team bit (@0x56285f) OVERALL checked (@0x562873), or the three tab radios
// hidden (@0x56288b, @0x5628ad, @0x5628cf).
void EndRoundScreen::open(menu::MenuRuntime &menu, const std::string &file, const std::string &screen) {
	(void)file;
	(void)screen;
	tab_ = -1;
	if (host_ == nullptr) return;
	const int list = menu.widget_id(kResultList);
	if (list >= 0) {
		// The design rect the width split reads (CWnd_GetRect's): its width, the authored one until
		// the frame lays the table out.
		int width = 0;
		if (const menu::MenuFrameSeam *frame = menu.frame()) {
			const int index = menu.frame_index(list);
			if (index >= 0) width = static_cast<int>(frame->widget_rect(index).w);
		}
		if (width <= 0) width = kResultListDefaultWidth;
		fill_stat_results(menu, list, host_->columns(width), host_->rows(), host_->local_slot());
	}
	if (host_->team_mode()) {
		const int overall = menu.widget_id(kTabs[0]);
		if (overall >= 0) menu.set_widget_checked(overall, true);
		return;
	}
	for (const char *name : kTabs) {
		const int id = menu.widget_id(name);
		if (id >= 0) menu.set_widget_shown(id, false);
	}
}

void EndRoundScreen::on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) {
	if (host_ == nullptr || event.kind != menu::MenuEvent::Kind::WidgetActivated) return;
	// The CONFIRM_YES command closes the in-game screens, then exits the mission (input action 3)
	// [orig: UI_StatConfirmExitCommand @0x562210 — Game_CloseInGameScreens @0x56221a,
	// Input_HandleActionBinding(3) @0x562229; registered @0x562813]; HIDDEN_BACK and CONFIRM_NO carry their authored actions alone (the
	// confirmation's SHOW CONFIRM_EXIT + HIDE STATS, and back).
	if (strutil::iequals(event.text, kConfirmYes)) {
		host_->close_screen();
		host_->exit_to_menu();
		return;
	}
	// The tab radios onto the filter's tab index [orig: StatScreen_StatFilterTabHandler @0x562140,
	// registered with params 0/1/2 by HUD_CacheStatPanelValues @0x5627a8..0x5627f5]: the filled rows
	// shown or hidden by their team value, their order and selection kept (D-MNU-53).
	for (int tab = 0; tab < 3; ++tab) {
		if (!strutil::iequals(event.text, kTabs[tab])) continue;
		const int list = menu.widget_id(kResultList);
		if (list >= 0) {
			stat_screen_filter_rows(menu, list, tab);
			tab_ = tab;
		}
		return;
	}
}

} // namespace opennova::inmatch
