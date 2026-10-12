// The terrain tools in the views over a real session (ADR 0046 S25). S0: the mission's Terrain pane, shown by one
// set_workspace (Ctrl+E's, the toolbar's), takes the Logic pane's place beside the 3D view (the session hides the
// Logic pane as it shows), draws its tool tabs with nothing raised by drawing and nothing past its room, and a tool
// pressed there is one set_workspace of the brush. A tab pressed picks its family's first tool where the brush has
// another family's (Paint: Surface, its legend then shown); the tab of the brush's own family picks nothing.
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/til_document.h>
#include <editor/model/document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_viewport.h>
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

// The mission's project on its terrain (Tmap: its .trn, heights and maps), the mission open and drawn.
bool open_on_terrain(MissionRun &run) {
	if (!run.open()) return false;
	const std::string root = run.session.view().project.root;
	const std::string fixtures = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/terrain/tmap/";
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		if (!editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixtures + name))) return false;
	run.session.handle(request::rescan());
	run.session.run_operations();
	run.settle();
	run.take();
	return true;
}

// The brush's drag over the wire while the windows draw between its samples (S25): one gesture, each sample going on
// with the one before, in the mission's 3D view.
void test_wire_brush_drag() {
	MissionRun run("opennova_editor_ui_terrain_wire_drag");
	CHECK(open_on_terrain(run), "the mission open on its terrain");
	run.session.handle(request::set_workspace(R"({"document": {"terrain": true}, "terrain_brush": {"tool": "raise", "radius": 20}})"));
	run.settle();
	run.take();
	const ViewportModel *model = run.session.viewports().find(run.path, ViewportKind::Mission);
	CHECK(model != nullptr, "the 3D view");
	if (!model) return;
	const float x = float(model->size().width) * 0.5f, y = float(model->size().height) * 0.5f;
	double token = 0.0;
	for (int i = 0; i < 4; ++i) {
		opennova::io::JsonValue sample;
		std::string error;
		const std::string text = R"({"kind": "edit_in_viewport", "path": ")" + run.path +
		                         R"(", "drag": {"id": 0, "handle": "brush", "to": [)" + std::to_string(x + 4.0f * float(i)) +
		                         ", " + std::to_string(y) + R"(], "gesture": )" + std::to_string(uint64_t(token)) +
		                         R"(, "end": )" + (i == 3 ? "true" : "false") + R"(, "kind": "mission"}})";
		CHECK(opennova::io::json_parse(text, sample, error), "the sample reads");
		const opennova::io::JsonValue answer = run.session.handle_json(sample);
		const opennova::io::JsonValue *outcome = answer.get("outcome");
		const opennova::io::JsonValue *done = outcome ? outcome->get("done") : nullptr;
		const opennova::io::JsonValue *gesture = outcome ? outcome->get("gesture") : nullptr;
		if (!done || !done->boolean) std::printf("  sample %d: %s\n", i, opennova::io::json_write(answer).c_str());
		CHECK(done && done->boolean && gesture && (i == 0 || gesture->number == token), "a sample of the one gesture");
		if (i == 0 && gesture) token = gesture->number;
		run.settle();
		run.take();
	}
}


// A tile set of `w` x `h` grey texels, an uncompressed true-colour TGA (the palette's cells as the game cuts them).
std::vector<uint8_t> grey_tga(int w, int h) {
	std::vector<uint8_t> out(18, 0);
	out[2] = 2;
	out[12] = uint8_t(w & 0xFF);
	out[13] = uint8_t(w >> 8);
	out[14] = uint8_t(h & 0xFF);
	out[15] = uint8_t(h >> 8);
	out[16] = 32;
	out[17] = 0x28;
	out.resize(out.size() + size_t(w) * size_t(h) * 4, 0x80);
	return out;
}

// A drag of the mouse across the 3D view's picture, as a person makes one: pressed at `from`, moved there in `steps`,
// let go (points of the picture, its corner 0, 0).
void picture_drag(MissionRun &run, const DrawnDevice &device, ImVec2 from, ImVec2 to, int steps) {
	run.ui.mouse(device.origin.x + from.x, device.origin.y + from.y);
	run.ui.button(true);
	for (int i = 1; i <= steps; ++i) {
		const float t = float(i) / float(steps);
		run.ui.mouse(device.origin.x + from.x + (to.x - from.x) * t, device.origin.y + from.y + (to.y - from.y) * t);
	}
	run.ui.button(false);
	run.settle();
}

// An item of a window scrolled into sight with the mouse wheel over the window, as a person reaches a row past its
// fold (no click): the window turned to its top, then down to the item. False where it never comes whole into sight.
bool wheel_to(Ui &ui, const char *window_part, ImGuiID id) {
	const auto in_sight = [&]() {
		const ImGuiWindow *window = MissionRun::drawn_window(window_part);
		ImRect rect;
		return window && ui.locate(id, rect) && window->InnerClipRect.Contains(rect.GetCenter()) &&
		       rect.Max.y <= window->InnerClipRect.Max.y + 6.0f && rect.Min.y >= window->InnerClipRect.Min.y - 6.0f;
	};
	const auto wheel = [&](float by) {
		const ImGuiWindow *window = MissionRun::drawn_window(window_part);
		if (!window) return false;
		ui.mouse(window->InnerClipRect.GetCenter().x, window->InnerClipRect.GetCenter().y);
		ImGui::GetIO().AddMouseWheelEvent(0.0f, by);
		ui.frames(2);
		return true;
	};
	if (in_sight()) return true;
	for (int i = 0; i < 30; ++i)
		if (!wheel(1.0f)) return false;
	for (int i = 0; i < 60; ++i) {
		if (in_sight()) return true;
		if (!wheel(-1.0f)) return false;
	}
	return false;
}

// The Terrain pane's tab body drawn last frame (its own scroll under the tab bar).
const ImGuiWindow *tab_body() { return MissionRun::drawn_window("/terrain_tab_body_"); }

// The requests of a kind among `requests`.
std::vector<const EditorRequest *> of_kind(const std::vector<EditorRequest> &requests, EditorRequestKind kind) {
	std::vector<const EditorRequest *> out;
	for (const EditorRequest &request : requests)
		if (request.kind == kind) out.push_back(&request);
	return out;
}

// Whether `requests` hold a stroke on `plane`, every sample under the one gesture, and its end by that gesture.
bool one_stroke(const std::vector<EditorRequest> &requests, TerrainPlane plane) {
	const std::vector<const EditorRequest *> strokes = of_kind(requests, EditorRequestKind::TerrainStroke);
	if (strokes.empty()) return false;
	const uint64_t gesture = strokes.front()->stroke.gesture;
	for (const EditorRequest *stroke : strokes)
		if (stroke->stroke.gesture != gesture || stroke->stroke.plane != plane || stroke->stroke.points.empty()) return false;
	const std::vector<const EditorRequest *> ends = of_kind(requests, EditorRequestKind::EndEdit);
	return gesture != 0 && ends.size() == 1 && ends[0]->gesture == gesture;
}

// The open document at `path` (null: not open).
const DocumentBase *open_at(const SessionView &v, const std::string &path) {
	for (const auto &open : v.documents.open)
		if (open->path() == path) return open.get();
	return nullptr;
}

// The plan's acceptance ledger (ADR 0046 S25), with the mouse and the keys over the null-backend Ui: the toolbar's
// Terrain (Raise, the default), a drag of it, Paint and its legend's Stone (12), a drag of it, Tiles and a cell of
// the palette, a click laying the tile, Select and R turning it; four undos taking every file back to its bytes, four
// redos and a save writing them; Play. 10 clicks (2 drags), no typed entry. Lane A's step (the tank's z following the
// sculpted ground, its .bms the fourth file saved) is asserted where A5 lands.
void test_ledger() {
	MissionRun run("opennova_editor_ui_terrain_ledger");
	CHECK(open_on_terrain(run), "the mission open on its terrain");
	const SessionView &v = run.session.view();
	const std::string root = v.project.root;
	CHECK(editor_test::write_bytes(root + "/terrain/mnml_t.tga", grey_tga(256, 128)), "the terrain's tile set written");
	run.session.handle(request::rescan());
	run.session.run_operations();
	ViewportCommand top;
	top.name = "top";
	top.kind = ViewportKind::Mission;
	run.session.handle(request::edit_in_viewport(run.path, top));
	run.settle();
	run.take();
	DrawnDevice *device = run.devices.held(run.path, ViewportKind::Mission);
	CHECK(device != nullptr && device->width > 0, "the 3D view's picture drawn");
	if (!device || device->width <= 0) return;
	device->ground = [](double, double) { return 0.0; };
	run.ui.pump = [&run] {
		run.pump();
		run.session.poll();
	};
	const float w = float(device->width), h = float(device->height);
	const std::string cpt = "terrain/Tmap.cpt", pcx = "terrain/Tmap_m.pcx";
	const std::string cpt_loaded = test_io::read_file_text(root + "/" + cpt);
	const std::string pcx_loaded = test_io::read_file_text(root + "/" + pcx);
	const std::string bms_loaded = test_io::read_file_text(root + "/" + run.path);
	int clicks = 0;

	// 1. The toolbar's Terrain: the pane, Raise the brush's tool.
	const ImGuiWindow *column = MissionRun::drawn_window("/viewport_column_");
	CHECK(column != nullptr, "the 3D view's column");
	if (!column) return;
	CHECK(run.ui.click_item(item_id(column->ID, {"Terrain"})), "1. the toolbar's Terrain clicked");
	++clicks;
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	CHECK(workspace_sets(raised, "document", "terrain").size() == 1 && v.workspace.document(run.path).terrain &&
	              v.workspace.terrain_brush.tool == TerrainTool::Raise,
	      "1. one set_workspace: the Terrain pane, Raise");

	// 2. A drag across the middle: one gesture of the heights' strokes, the heights opened beside the mission.
	picture_drag(run, *device, ImVec2(w * 0.42f, h * 0.5f), ImVec2(w * 0.58f, h * 0.5f), 8);
	++clicks;
	raised = run.take();
	CHECK(one_stroke(raised, TerrainPlane::Heights), "2. the drag: the heights' strokes under one gesture, ended");
	const DocumentBase *heights = open_at(v, cpt);
	CHECK(heights && heights->dirty() && v.documents.active == run.path, "2. Tmap.cpt opened beside the mission, sculpted");

	// 3. Paint, then Stone (12) in its legend.
	const ImGuiWindow *pane = MissionRun::drawn_window("/terrain_pane_");
	CHECK(pane != nullptr, "the Terrain pane drawn");
	if (!pane) return;
	const ImGuiID tabs = item_id(pane->ID, {"terrain_tools"});
	CHECK(run.ui.click_item(item_id(tabs, {"Paint"})), "3. the Paint tab clicked");
	++clicks;
	run.settle();
	const std::string stone = "12  " + mission_surface_words(12);
	const ImGuiWindow *body = tab_body();
	CHECK(body != nullptr, "3. Paint's body drawn");
	if (!body) return;
	const ImGuiID stone_id = item_id(pushed(body->ID, 12), {stone.c_str()});
	CHECK(wheel_to(run.ui, "/terrain_tab_body_", stone_id), "3. Stone in sight, the legend scrolled to it");
	CHECK(run.ui.click_item(stone_id), "3. Stone clicked in the legend");
	++clicks;
	run.settle();
	run.take();
	CHECK(v.workspace.terrain_brush.tool == TerrainTool::Surface && v.workspace.terrain_brush.surface == 12,
	      "3. the brush paints the surface with class 12");

	// 4. A drag over the hill: the surface map's strokes, its file opened beside the mission.
	picture_drag(run, *device, ImVec2(w * 0.45f, h * 0.45f), ImVec2(w * 0.55f, h * 0.55f), 6);
	++clicks;
	raised = run.take();
	CHECK(one_stroke(raised, TerrainPlane::Surface), "4. the drag: the surface's strokes under one gesture, ended");
	const DocumentBase *surface = open_at(v, pcx);
	CHECK(surface && surface->dirty(), "4. Tmap_m.pcx opened beside the mission, painted");

	// 5. Tiles (Stamp), then a cell of the palette (once the tile set reads).
	const ImGuiID tiles_tab = item_id(tabs, {"Tiles"});
	CHECK(run.ui.click_item(tiles_tab), "5. the Tiles tab clicked");
	++clicks;
	run.settle();
	CHECK(v.workspace.terrain_brush.tool == TerrainTool::Stamp, "5. Tiles: the Stamp tool");
	body = tab_body();
	CHECK(body != nullptr, "5. Tiles' body drawn");
	if (!body) return;
	const ImGuiID cell = item_id(pushed(body->ID, 1), {"cell"});
	ImRect cell_rect;
	for (int round = 0; round < 60 && !run.ui.locate(cell, cell_rect); ++round) run.settle(1);
	CHECK(wheel_to(run.ui, "/terrain_tab_body_", cell), "5. the cell in sight");
	CHECK(run.ui.click_item(cell), "5. the palette's cell 1 clicked");
	++clicks;
	run.settle();
	run.take();
	CHECK(v.workspace.terrain_brush.cell == 1 && v.workspace.terrain_brush.tool == TerrainTool::Stamp, "5. the stamp's cell 1");

	// 6. A click beside the hill lays the tile: the mission's .til made with it.
	run.ui.click(ImVec2(device->origin.x + w * 0.5f, device->origin.y + h * 0.3f));
	++clicks;
	run.settle();
	run.take();
	const std::string til = run.path.substr(0, run.path.rfind('.')) + ".til";
	const DocumentBase *tiles = open_at(v, til);
	const Document *placement = tiles ? records_of(*tiles) : nullptr;
	CHECK(placement && placement->rows().size() == 1 && tiles->dirty(), "6. a tile laid in the mission's .til");
	if (!placement || placement->rows().size() != 1) return;

	// 7. Select, then R: the selected tile turned. The plan's ledger has the stamp leave its tile selected; where it is
	// not, the tile is clicked in the view (a click past the ledger's ten).
	body = tab_body();
	const ImGuiID select = body ? item_id(body->ID, {"Select"}) : 0;
	CHECK(body && wheel_to(run.ui, "/terrain_tab_body_", select), "7. Select in sight");
	CHECK(run.ui.click_item(select), "7. Select clicked");
	++clicks;
	run.settle();
	run.take();
	CHECK(v.workspace.terrain_brush.tool == TerrainTool::Tiles, "7. the Select tool");
	bool tile_clicked = false;
	if (v.workspace.document(run.path).tiles.empty()) {
		const auto *laid = dynamic_cast<const TilDocument *>(open_at(v, til));
		const auto *model = static_cast<const MissionViewport *>(run.session.viewports().find(run.path, ViewportKind::Mission));
		float x = 0.0f, y = 0.0f;
		if (laid && model && !laid->file().entries.empty()) {
			const double middle[3] = {laid->file().entries[0].x_fixed / 65536.0 + 8.0,
			                          -laid->file().entries[0].z_fixed / 65536.0 + 8.0, 0.0};
			model->camera().project(mission_to_preview(middle), device->width, device->height, x, y);
		}
		run.ui.click(ImVec2(device->origin.x + x, device->origin.y + y));
		++clicks;
		tile_clicked = true;
		run.settle();
		run.take();
	}
	CHECK(v.workspace.document(run.path).tiles.size() == 1, "7. the laid tile selected");
	// A click in the pane leaves the keys to the view (the pane is its palette): on its body, away from its controls.
	body = tab_body();
	if (body) {
		run.ui.mouse(body->InnerClipRect.GetCenter().x, body->InnerClipRect.Max.y - 3.0f);
		CHECK(GImGui->HoveredId == 0, "7. the pane's body clicked where no control is");
		run.ui.button(true);
		run.ui.button(false);
		run.ui.frames(2);
	}
	raised = run.ui.chord({ImGuiKey_R});
	run.settle();
	std::vector<EditorRequest> turned = run.take();
	turned.insert(turned.begin(), raised.begin(), raised.end());
	bool rotate = false;
	for (const EditorRequest &request : turned)
		if (request.kind == EditorRequestKind::EditInViewport && request.command.name == "tile_rotate" &&
		    request.command.by == std::vector<double>{1.0})
			rotate = true;
	CHECK(rotate, "7. R: tile_rotate by 1, the keys the view's after a click in the pane");

	// 8. Ctrl+Z four times: every file back to the bytes it was loaded with, the .til to no tile.
	for (int i = 0; i < 4; ++i) run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_Z});
	run.settle();
	run.take();
	heights = open_at(v, cpt);
	surface = open_at(v, pcx);
	tiles = open_at(v, til);
	placement = tiles ? records_of(*tiles) : nullptr;
	CHECK(heights && heights->serialize().text == cpt_loaded, "8. Tmap.cpt's bytes as loaded");
	CHECK(surface && surface->serialize().text == pcx_loaded, "8. Tmap_m.pcx's bytes as loaded");
	CHECK(placement && placement->rows().empty(), "8. the .til with no tile");
	const DocumentBase *mission = open_at(v, run.path);
	CHECK(mission && mission->serialize().text == bms_loaded, "8. the mission's bytes as loaded");

	// 9. Ctrl+Y four times, Ctrl+S: the files written, one status line.
	for (int i = 0; i < 4; ++i) run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_Y});
	run.settle();
	run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_S});
	run.settle();
	run.take();
	CHECK(test_io::read_file_text(root + "/" + cpt) != cpt_loaded && test_io::read_file_text(root + "/" + pcx) != pcx_loaded,
	      "9. Tmap.cpt and Tmap_m.pcx written with their edits");
	CHECK(test_io::read_file(root + "/" + til).size() == 16 + 12, "9. the .til written with its tile");
	heights = open_at(v, cpt);
	surface = open_at(v, pcx);
	tiles = open_at(v, til);
	CHECK(heights && !heights->dirty() && surface && !surface->dirty() && tiles && !tiles->dirty(), "9. all saved");
	CHECK(!v.activity.status.empty() && v.activity.status.find('\n') == std::string::npos, "9. one status line");

	// 10. Play mission, on the toolbar over the picture (the column split beside the pane).
	const ImGuiWindow *canvas_column = MissionRun::drawn_window("/canvas_column_");
	CHECK(canvas_column && (run.ui.click_item(item_id(canvas_column->ID, {"Play mission"})) ||
	                        run.ui.click_item(item_id(canvas_column->ID, {"###Play mission"}))),
	      "10. Play mission clicked");
	++clicks;
	run.settle();
	CHECK(!of_kind(run.take(), EditorRequestKind::Play).empty(), "10. Play raised");
	std::printf("  ledger: %d clicks%s\n", clicks, tile_clicked ? " (the stamped tile clicked to select it)" : "");
	CHECK(clicks == (tile_clicked ? 11 : 10), "the ledger: 10 clicks (2 drags), no typed entry");
	run.ui.pump = nullptr;
}

} // namespace

void run_terrain_tests() {
	test_terrain_pane();
	test_sculpt_parts();
	test_wire_brush_drag();
	test_ledger();
}

} // namespace editor_ui_test
