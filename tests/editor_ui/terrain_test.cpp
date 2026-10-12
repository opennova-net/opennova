// The terrain tools in the views over a real session (ADR 0046 S25). S0: the mission's Terrain pane, shown by one
// set_workspace (Ctrl+E's, the toolbar's), takes the Logic pane's place beside the 3D view (the session hides the
// Logic pane as it shows), draws its tool tabs with nothing raised by drawing and nothing past its room, and a tool
// pressed there is one set_workspace of the brush. A tab pressed picks its family's first tool where the brush has
// another family's (Paint: Surface, its legend then shown); the tab of the brush's own family picks nothing.
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
	// A tab pressed: its family's first tool (the brush's default is Raise, a Sculpt tool).
	const ImGuiWindow *pane = MissionRun::drawn_window("/terrain_pane_");
	CHECK(pane != nullptr, "the Terrain pane's window");
	if (pane) {
		const ImGuiID tabs = item_id(pane->ID, {"terrain_tools"});
		CHECK(run.ui.click_item(item_id(tabs, {"Paint"})), "the Paint tab clicked");
		run.settle();
		raised = run.take();
		const std::vector<opennova::io::JsonValue> tools = workspace_sets(raised, "terrain_brush", "tool");
		CHECK(tools.size() == 1 && tools[0].string == "surface" && v.workspace.terrain_brush.tool == TerrainTool::Surface,
		      "Paint: the Surface tool picked");
		CHECK(run.ui.click_item(item_id(tabs, {"Paint"})), "the Paint tab again");
		run.settle();
		CHECK(workspace_sets(run.take(), "terrain_brush", "tool").empty(), "its own family's tab picks nothing");
	}
	// The Logic pane shown again hides the Terrain pane.
	window_requests::show_logic(run.ui.windows, run.path, true);
	run.settle();
	CHECK(v.workspace.document(run.path).logic && !v.workspace.document(run.path).terrain, "one pane at a time");
}

// S25 A4: the Sculpt tab's parts over the mission's terrain (Tmap): the heights file named as it opens with the first
// stroke; once open, its range, and the bake line (a region waiting while the stroke is open); Set's height slider.
void test_sculpt_parts() {
	MissionRun run("opennova_editor_ui_terrain_sculpt");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const SessionView &v = run.session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	for (const char *name : {"Tmap.trn", "Tmap.cpt"})
		CHECK(editor_test::write_bytes(v.project.root + "/terrain/" + name,
		                               test_io::read_file(repo + "/fixtures/terrain/tmap/" + name)),
		      "the terrain's file written");
	run.session.handle(request::rescan());
	run.session.run_operations();
	window_requests::show_terrain(run.ui.windows, run.path, true);
	run.settle();
	run.take();
	std::string text = logged_frame(run.ui);
	CHECK(text.find("Tmap.cpt opens as you sculpt.") != std::string::npos, "the heights file named before a stroke");
	// A raise, its stroke open: the heights' range and a region waiting for its bake.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Heights;
	ask.brush.tool = TerrainTool::Raise;
	ask.brush.radius = 8.0f;
	ask.brush.strength = 1.0f;
	ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
	ask.points = {{-200.0, 200.0}};
	ask.gesture = 5151;
	ask.end = false;
	run.session.handle(request::terrain_stroke(run.path, ask));
	CHECK(run.session.outcome().done(), "the stroke served");
	run.settle();
	text = logged_frame(run.ui);
	CHECK(text.find("Heights ") != std::string::npos && text.find(" tiles") != std::string::npos, "the heights' range");
	CHECK(text.find("1 region waits for the bake") != std::string::npos, "the bake line: a region waiting");
	// Set: its height slider.
	window_requests::set_workspace(run.ui.windows, "terrain_brush", "tool", opennova::io::JsonValue::make_string("set"));
	run.settle();
	text = logged_frame(run.ui);
	CHECK(v.workspace.terrain_brush.tool == TerrainTool::Set && text.find("Height") != std::string::npos,
	      "Set's height slider");
	run.ui.frames(2);
	CHECK(run.ui.windows.pending_requests() == 0, "drawing the parts raises nothing");
}

} // namespace

void run_terrain_tests() {
	test_terrain_pane();
	test_sculpt_parts();
}

} // namespace editor_ui_test
