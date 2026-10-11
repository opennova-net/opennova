// A mission's logic in its views over a real session (ADR 0046 S24, In the views). S0: Pick in view raises the
// set_workspace whose pick the session holds (a trigger's parameter slot), and ends it; an event's outline line's
// right-click menu offers the logic's items after the jumps (the mission's row menu), Show on board one
// set_workspace that shows the Logic pane.
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/ui/editor_requests.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

std::string right_click(Ui &ui, ImVec2 at) {
	ui.mouse(at.x, at.y);
	ui.button(true, ImGuiMouseButton_Right);
	ui.button(false, ImGuiMouseButton_Right);
	return logged_frame(ui);
}

void test_pick_and_row_menu() {
	MissionRun run("opennova_editor_ui_space_pick");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	const SessionView &v = run.session.view();
	const Node *event = mission->rows_of(MissionKind::Event)[0];
	const std::vector<Document::Collection> lists = mission->collections_of({event->id, event->kind, 0});
	CHECK(!lists.empty() && !lists[0].ids.empty(), "the first event's trigger");
	if (lists.empty() || lists[0].ids.empty()) return;
	const NodeAddress trigger{event->id, node_kind(MissionKind::Trigger), lists[0].ids[0]};

	// Pick in view: one set_workspace, the pick held; ended by a record of row 0.
	window_requests::pick_in_view(run.ui.windows, run.path, trigger, 1);
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	CHECK(raised.size() == 1 && workspace_sets(raised, "document", "pick").size() == 1, "Pick in view: one set_workspace");
	const WorkspaceView::Pick &pick = v.workspace.document(run.path).pick;
	CHECK(pick.active() && pick.record == trigger && pick.slot == 1, "the session holds the pick");
	window_requests::pick_in_view(run.ui.windows, run.path, NodeAddress{}, -1);
	run.settle();
	run.take();
	CHECK(!v.workspace.document(run.path).pick.active(), "the pick ended");

	// The outline: the first event's line, its menu's logic items after the jumps.
	const ImGuiWindow *outline = MissionRun::drawn_window("/outline_column_");
	CHECK(outline != nullptr, "the outline column");
	if (!outline) return;
	std::string text;
	bool opened = false;
	for (float y = outline->InnerRect.Min.y + 2.0f; y < outline->InnerRect.Max.y && !opened; y += 6.0f) {
		run.ui.mouse(outline->InnerRect.Min.x + 40.0f, y);
		if (GImGui->HoveredId == 0) continue;
		text = right_click(run.ui, ImVec2(outline->InnerRect.Min.x + 40.0f, y));
		opened = text.find("Show on board") != std::string::npos && GImGui->OpenPopupStack.Size == 1;
		if (!opened) {
			ImGui::ClosePopupsExceptModals();
			run.ui.frames(1);
		}
	}
	CHECK(opened && in_order(text, {"Go to definition", "Find usages", "Show on board"}),
	      "an event's line: its jumps, then the logic's Show on board");
	if (!opened) return;
	run.settle();
	run.take();
	const ImGuiWindow *popup = GImGui->OpenPopupStack.Size ? GImGui->OpenPopupStack.back().Window : nullptr;
	CHECK(popup != nullptr, "the menu open");
	if (!popup) return;
	run.ui.activate(item_id(popup->ID, {"Show on board"}));
	run.settle();
	raised = run.take();
	CHECK(workspace_sets(raised, "document", "logic").size() == 1 && v.workspace.document(run.path).logic,
	      "Show on board: the Logic pane shown");
}

} // namespace

void run_space_tests() { test_pick_and_row_menu(); }

} // namespace editor_ui_test
