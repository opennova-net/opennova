// The live logic on the Event board over a real session (ADR 0046 S24, Live). S0: while the view holds a running
// game's report of the mission (ActivityView::live_logic), the board draws its cards with it (the live hook at
// each card's heading), raising nothing by drawing, nothing past its room. And a variable's name in the board's
// Variables fold (S24 Names, C3).
#include <cstring>
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_live.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_variable_names.h>
#include <editor/documents/mission_variables.h>
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

// The logic pane's own window (none of the cards in it), drawn last frame.
const ImGuiWindow *pane_window() {
	for (const ImGuiWindow *window : GImGui->Windows) {
		if (!window->Active || window->Hidden) continue;
		const char *at = std::strstr(window->Name, "/logic_pane_");
		if (at && !std::strchr(at + 1, '/')) return window;
	}
	return nullptr;
}

// A variable's name in the board's Variables fold (S24 C3): a variable the mission names is listed though no event
// uses it, with its name field and how many more take a name; a name typed and Enter is one edit_record of the
// layer table's records; a name too long for its record is refused in words, nothing raised.
void test_variable_name_field() {
	MissionRun run("opennova_editor_ui_live_names");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	std::vector<Edit> edits;
	std::string error;
	CHECK(mission_variable_name_edits(*mission, 12, "alarm_raised", edits, error), "V12's name planned");
	run.session.handle(request::edit_record(run.path, edits));
	run.session.run_operations();
	window_requests::show_logic(run.ui.windows, run.path, true);
	run.settle();
	run.take();
	const ImGuiWindow *pane = pane_window();
	CHECK(pane != nullptr, "the logic pane drawn");
	if (!pane) return;
	const DocumentMissionNames names(*mission);
	const std::vector<MissionVariable> listed = mission_variables(*mission, names);
	const std::string heading = "Variables (" + std::to_string(listed.size()) + ")###variables";
	run.ui.activate(item_id(pane->ID, {heading.c_str()}));
	run.ui.activate(item_id(pushed(pane->ID, 12), {"alarm_raised (0 uses)###variable"}));
	run.ui.away();
	std::string text = logged_frame(run.ui);
	CHECK(in_order(text, {"alarm_raised (0 uses)", "Name", "30 more variables can take a name (31 at most)."}),
	      "the named variable listed, its name field, how many more take a name");
	const ImGuiID field = item_id(pushed(pane->ID, 12), {"###variable", "variable_name", "##name"});
	run.ui.activate(field);
	run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_A});
	ImGui::GetIO().AddInputCharactersUTF8("alarm");
	run.ui.frames(1);
	std::vector<EditorRequest> raised = run.ui.chord({ImGuiKey_Enter});
	const EditorRequest *named = only(raised, EditorRequestKind::EditRecord);
	CHECK(named && named->edits.size() == 1 && named->edits[0].field == "name" &&
	              named->edits[0].value == Value(std::string("V12 alarm")),
	      "Enter: one edit_record of the record that holds V12's name");
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	run.take();
	CHECK(mission_variable_name(*mission, 12) == "alarm", "V12 renamed");
	// Too long for V12's record (16 characters): refused in words, nothing raised, the name as it was; the
	// variable stays open under its new name.
	run.ui.activate(field);
	run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_A});
	ImGui::GetIO().AddInputCharactersUTF8("seventeen chars..");
	run.ui.frames(1);
	raised = run.ui.chord({ImGuiKey_Enter});
	CHECK(raised.empty(), "a name too long raises nothing");
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(in_order(text, {"alarm (0 uses)", "V12's name holds 16 characters"}), "the refusal said beside the field");
	CHECK(mission_variable_name(*mission, 12) == "alarm", "the name as it was");
	CHECK(overflowing().empty(), "nothing runs past its room");
}

} // namespace

void run_live_tests() {
	test_board_with_a_report();
	test_variable_name_field();
}

} // namespace editor_ui_test
