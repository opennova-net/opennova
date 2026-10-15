// The live logic on the Event board over a real session (ADR 0046 S24, Live). S0: while the view holds a running
// game's report of the mission (ActivityView::live_logic), the board draws its cards with it (the live hook at
// each card's heading), raising nothing by drawing, nothing past its room. And a variable's name in the board's
// Variables fold (S24 Names, C3).
#include <algorithm>
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
	const std::vector<const Node *> rows = mission->rows_of(MissionKind::Event);
	for (size_t i = 0; i < rows.size(); ++i) {
		// The played event at this place: its row and its triggers' records.
		PlayedEvent played;
		played.row = rows[i]->id;
		const EventRow &chain = static_cast<const EventRow &>(*rows[i]);
		for (size_t t = 0; t < chain.native.triggers.size() && !chain.ids.lists.empty() && t < chain.ids.lists[0].size(); ++t)
			played.triggers.push_back(chain.ids.lists[0][t].id);
		live.played.push_back(std::move(played));
		LiveEvent event;
		event.index = i;
		event.active = i == 0;
		event.fired = i == 1;
		// Event 1 waits on its delay, 3 of its 64-tick steps left; event 2 fired.
		event.activate_countdown = i == 0 ? 192 : 0;
		event.truth = {1};
		event.chain = 1;
		event.last_checked_tick = 600;
		live.events.push_back(event);
	}
	live.variables.assign(512, 0);
	live.serial = 1;
	window_requests::show_logic(run.ui.windows, run.path, true);
	run.settle();
	run.take();
	run.ui.away();
	std::string text = logged_frame(run.ui);
	CHECK(MissionRun::drawn_window("/logic_pane_") && in_order(text, {"2 of 2 events", "Event 1", "Waiting: acts in 3.1 s (3 steps)",
	                                                                    "Fire", "When", "Event 2", "Fired", "Fire"}),
	      "the board drawn with the report: each card's state and its Fire");
	CHECK(text.find("stale") == std::string::npos, "not stale while the document is as played");
	run.ui.frames(3);
	CHECK(run.ui.drain().empty(), "drawing it raises nothing");
	CHECK(overflowing().empty(), "nothing runs past its room");
	// Fire on event 2's card: one play_debug of its index.
	std::vector<const ImGuiWindow *> cards;
	for (const ImGuiWindow *window : GImGui->Windows)
		if (window->Active && !window->Hidden && std::strstr(window->Name, "/card_") && std::strstr(window->Name, "/logic_pane_"))
			cards.push_back(window);
	std::sort(cards.begin(), cards.end(), [](const ImGuiWindow *a, const ImGuiWindow *b) { return a->Pos.y < b->Pos.y; });
	CHECK(cards.size() == 2, "two cards");
	if (cards.size() != 2) return;
	run.ui.activate(item_id(cards[1]->ID, {"live", "Fire"}));
	const std::vector<EditorRequest> raised = run.ui.drain();
	const EditorRequest *fire = only(raised, EditorRequestKind::PlayDebug);
	CHECK(fire && fire->event == 1 && fire->variable == -1, "Fire: one play_debug of the event's index");
	// A fire asked waits for the event's next check; an edit since it was played says the report is stale.
	live.events[1].fire_pending = true;
	live.stale = true;
	++live.serial;
	run.settle();
	run.take();
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(in_order(text, {"Event 2", "Fired", "stale", "fire asked"}), "the fire asked, the report stale");
	// The game silent a while: its last report, said so.
	live.quiet_ms = 3500;
	++live.serial;
	run.settle();
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(in_order(text, {"Event 1", "no report for 3 s", "Waiting"}), "the game's silence said on the card");
	live.quiet_ms = 0;
	// An event not at its played place (added, moved or removed since Play started): none of the game's state.
	std::swap(live.played[0], live.played[1]);
	++live.serial;
	run.settle();
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(in_order(text, {"Event 1", "not as played", "Event 2", "not as played"}) && text.find("Waiting") == std::string::npos &&
	              text.find("Fire") == std::string::npos,
	      "a card of another row than the played one at its place shows none of the game's state, no Fire");
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
// mission's file-wide value (the .mis's `//var` line); a name the .mis cannot carry is refused in words, nothing
// raised.
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
	CHECK(in_order(text, {"alarm_raised (0 uses)", "Name", "255 more variables can take a name (256 at most)."}),
	      "the named variable listed, its name field, how many more take a name");
	const ImGuiID field = item_id(pushed(pane->ID, 12), {"###variable", "variable_name", "##name"});
	run.ui.activate(field);
	run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_A});
	ImGui::GetIO().AddInputCharactersUTF8("alarm");
	run.ui.frames(1);
	std::vector<EditorRequest> raised = run.ui.chord({ImGuiKey_Enter});
	const EditorRequest *named = only(raised, EditorRequestKind::EditRecord);
	CHECK(named && named->edits.size() == 1 && named->edits[0].field == kMisVariableField &&
	              named->edits[0].position == 12 && named->edits[0].value == Value(std::string("alarm")),
	      "Enter: one edit_record of V12's name");
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	run.take();
	CHECK(mission_variable_name(*mission, 12) == "alarm", "V12 renamed");
	// A ';' the .mis cannot carry: refused in words, nothing raised, the name as it was; the variable stays open
	// under its new name.
	run.ui.activate(field);
	run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_A});
	ImGui::GetIO().AddInputCharactersUTF8("alarm;raised");
	run.ui.frames(1);
	raised = run.ui.chord({ImGuiKey_Enter});
	CHECK(raised.empty(), "a name the .mis cannot carry raises nothing");
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(in_order(text, {"alarm (0 uses)", "A name takes no control character"}), "the refusal said beside the field");
	CHECK(mission_variable_name(*mission, 12) == "alarm", "the name as it was");
	CHECK(overflowing().empty(), "nothing runs past its room");
	// While the game reports the mission: V12's value there, and Set asks it to take the one typed.
	SessionView &v = const_cast<SessionView &>(run.session.view());
	v.activity.live_logic.mission = run.path;
	v.activity.live_logic.variables.assign(512, 0);
	v.activity.live_logic.variables[12] = 7;
	v.activity.live_logic.serial = 1;
	run.settle();
	run.take();
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(in_order(text, {"alarm (0 uses)", "In the game: 7", "Set"}), "the variable's value in the game");
	run.ui.activate(item_id(pushed(pane->ID, 12), {"###variable", "variable_live", "Set"}));
	raised = run.ui.drain();
	const EditorRequest *set = only(raised, EditorRequestKind::PlayDebug);
	CHECK(set && set->variable == 12 && set->value == 7 && set->event == -1, "Set: one play_debug of V12's value");
	v.activity.live_logic = LiveLogic();
}

} // namespace

void run_live_tests() {
	test_board_with_a_report();
	test_variable_name_field();
}

} // namespace editor_ui_test
