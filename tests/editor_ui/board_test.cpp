// The Event board in the mission's tab over a real session (ADR 0046 S24, The board). S0: the 3D view's Logic
// tool raises one set_workspace that shows the mission's Logic pane, which then shares the viewport's column
// with the canvas (both drawn, nothing past its room) and lists the events as cards; a card clicked selects its
// event; Ctrl+L hides the pane again, as the Windows menu's Logic tick does.
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

void test_logic_pane() {
	MissionRun run("opennova_editor_ui_board_pane");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	const SessionView &v = run.session.view();
	const ImGuiWindow *column = MissionRun::drawn_window("/viewport_column_");
	CHECK(column != nullptr && !MissionRun::drawn_window("/logic_pane_"), "the 3D view's column, no pane yet");
	if (!column) return;

	// The toolbar's Logic: one set_workspace showing the pane.
	run.ui.activate(item_id(column->ID, {"Logic"}));
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	const std::vector<opennova::io::JsonValue> shown = workspace_sets(raised, "document", "logic");
	CHECK(raised.size() == 1 && shown.size() == 1 && shown[0].is_bool() && shown[0].boolean, "Logic: one set_workspace, logic on");
	CHECK(v.workspace.document(run.path).logic, "the session holds the pane shown");
	run.ui.away();
	const std::string text = logged_frame(run.ui);
	CHECK(MissionRun::drawn_window("/logic_pane_") && MissionRun::drawn_window("/canvas_column_"),
	      "the column split: the canvas and the pane both drawn");
	CHECK(run.devices.held(run.path, ViewportKind::Mission)->draws > 0, "the canvas still drawn");
	CHECK(in_order(text, {"Events (2)", "Event 1: When ", "Event 2: When event 1 has fired"}), "the events as cards");
	CHECK(overflowing().empty(), "nothing runs past its room");
	run.take();

	// A card clicked selects its event.
	const ImGuiWindow *cards = MissionRun::drawn_window("/cards_");
	CHECK(cards != nullptr, "the cards drawn");
	if (cards) {
		run.ui.click(ImVec2(cards->InnerRect.Min.x + 20.0f, cards->InnerRect.Min.y + 4.0f));
		run.settle();
		const Node *first = mission->rows_of(MissionKind::Event)[0];
		const std::vector<EditorRequest> clicked = run.take();
		const EditorRequest *select = only(clicked, EditorRequestKind::SelectRecord);
		CHECK(select && select->address.row == first->id && !select->address.child, "a card clicked: one SelectRecord of its event");
		CHECK(v.documents.selection.primary.row == first->id, "the session holds it");
	}

	// Ctrl+L hides the pane.
	run.ui.away();
	raised = run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_L});
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	const std::vector<opennova::io::JsonValue> hidden = workspace_sets(raised, "document", "logic");
	CHECK(hidden.size() == 1 && hidden[0].is_bool() && !hidden[0].boolean && !v.workspace.document(run.path).logic,
	      "Ctrl+L: the pane hidden");
	run.ui.frames(2);
	CHECK(!MissionRun::drawn_window("/logic_pane_"), "no pane drawn");

	// The Windows menu's tick shows it again.
	raised = choose(run.ui, "Windows", {"Logic"});
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	CHECK(workspace_sets(raised, "document", "logic").size() == 1 && v.workspace.document(run.path).logic,
	      "the Windows menu's Logic: the pane shown");
}

} // namespace

void run_board_tests() { test_logic_pane(); }

} // namespace editor_ui_test
