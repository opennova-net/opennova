// The terrain tools in the views over a real session (ADR 0046 S25). S0: the mission's Terrain pane, shown by one
// set_workspace (Ctrl+E's, the toolbar's), takes the Logic pane's place beside the 3D view (the session hides the
// Logic pane as it shows), draws its tool tabs with nothing raised by drawing and nothing past its room, and a tool
// pressed there is one set_workspace of the brush.
#include <string>
#include <vector>

#include <editor/ui/editor_requests.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

void test_terrain_pane() {
	MissionRun run("opennova_editor_ui_terrain_pane");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const SessionView &v = run.session.view();
	window_requests::show_logic(run.ui.windows, run.path, true);
	run.settle();
	CHECK(v.workspace.document(run.path).logic, "the Logic pane shown");
	window_requests::show_terrain(run.ui.windows, run.path, true);
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	CHECK(workspace_sets(raised, "document", "terrain").size() == 1, "Terrain: one set_workspace");
	CHECK(v.workspace.document(run.path).terrain && !v.workspace.document(run.path).logic,
	      "the Terrain pane shown, the Logic pane hidden");
	CHECK(MissionRun::drawn_window("/terrain_pane_") != nullptr && MissionRun::drawn_window("/logic_pane_") == nullptr,
	      "the Terrain pane drawn in the Logic pane's place");
	run.ui.frames(2);
	CHECK(run.ui.windows.pending_requests() == 0, "drawing the pane raises nothing");
	// The Logic pane shown again hides the Terrain pane.
	window_requests::show_logic(run.ui.windows, run.path, true);
	run.settle();
	CHECK(v.workspace.document(run.path).logic && !v.workspace.document(run.path).terrain, "one pane at a time");
}

} // namespace

void run_terrain_tests() { test_terrain_pane(); }

} // namespace editor_ui_test
