// The live logic on the Event board over a real session (ADR 0046 S24, Live). S0: while the view holds a running
// game's report of the mission (ActivityView::live_logic), the board draws its cards with it (the live hook at
// each card's heading), raising nothing by drawing, nothing past its room.
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_live.h>
#include <editor/documents/mission_table.h>
#include <editor/ui/editor_requests.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

void test_board_with_a_report() {
	MissionRun run("opennova_editor_ui_live_board");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	// The game's report, as the session holds it while Play runs the runtime.
	SessionView &v = const_cast<SessionView &>(run.session.view());
	LiveLogic &live = v.activity.live_logic;
	live.mission = run.path;
	live.played_revision = mission->revision();
	live.tick = 620;
	for (size_t i = 0; i < mission->rows_of(MissionKind::Event).size(); ++i) {
		LiveEvent event;
		event.index = i;
		event.active = i == 0;
		event.truth = {1};
		event.last_checked_tick = 600;
		live.events.push_back(event);
	}
	live.variables.assign(8, 0);
	live.serial = 1;
	window_requests::show_logic(run.ui.windows, run.path, true);
	run.settle();
	run.take();
	run.ui.away();
	const std::string text = logged_frame(run.ui);
	CHECK(MissionRun::drawn_window("/logic_pane_") && in_order(text, {"2 of 2 events", "Event 1", "When"}),
	      "the board drawn with the report");
	run.ui.frames(3);
	CHECK(run.ui.drain().empty(), "drawing it raises nothing");
	CHECK(overflowing().empty(), "nothing runs past its room");
	v.activity.live_logic = LiveLogic();
}

} // namespace

void run_live_tests() { test_board_with_a_report(); }

} // namespace editor_ui_test
