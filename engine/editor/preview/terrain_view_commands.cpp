#include <editor/preview/terrain_view_commands.h>

#include <base/io/json.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_tools_parts.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

bool is_tile_command(const std::string &name) {
	return name == "tile_rotate" || name == "tile_flip" || name == "tile_layer" || name == "tile_delete" ||
	       name == "tile_group_rotate";
}

bool brush_live(const TerrainViewContext &view) {
	const DocumentBase *document = view.context.input.document;
	return document && terrain_brush_live(view.context.input.view, *document);
}

// One sample of a stroke of the workspace's brush at the ground point `at`.
void stroke_at(const TerrainViewContext &view, TerrainPoint at, uint64_t gesture, bool end, CanvasRequests &out) {
	TerrainStrokeAsk ask;
	terrain_tool_plane(view.context.input.view.workspace.terrain_brush.tool, ask.plane);
	ask.points.emplace_back(at.x, at.y);
	ask.gesture = gesture;
	ask.end = end;
	out.request(request::terrain_stroke(view.view, ask));
}

// The eyedropper: the value under the point taken into the brush.
bool eyedropper(const TerrainViewContext &view, const ViewportCommand &command, CanvasRequests &out, std::string &error) {
	if (!command.has_at) {
		error = "An eyedropper takes the point of the picture it is at, \"at\": [x, y].";
		return false;
	}
	TerrainPoint at;
	double height = 0.0;
	if (!view.space.pointer_ground(command.at_x, command.at_y, at, height)) {
		error = "No ground lies under that point of the picture.";
		return false;
	}
	const TerrainBrush &brush = view.context.input.view.workspace.terrain_brush;
	const MissionGroundFacts facts = view.ground ? view.ground->terrain_at(at.x, at.y, height) : MissionGroundFacts();
	io::JsonValue members = io::JsonValue::make_object();
	switch (terrain_tool_family(brush.tool)) {
	case TerrainToolFamily::Sculpt:
		members.set("height", io::json_number(height < 0.0 ? 0.0 : height > kTerrainHeightMost ? kTerrainHeightMost : height));
		break;
	case TerrainToolFamily::Paint:
		if (brush.tool == TerrainTool::Surface && facts.surface >= 0 && facts.surface <= 255) {
			members.set("surface", io::json_number(facts.surface));
		} else if (brush.tool == TerrainTool::Foliage && facts.foliage_code >= 0) {
			members.set("foliage", io::json_number(facts.foliage_code));
		} else {
			error = std::string("The eyedropper takes no ") + terrain_tool_label(brush.tool) + " value there yet.";
			return false;
		}
		break;
	case TerrainToolFamily::Tiles:
	case TerrainToolFamily::None:
		error = "Pick a Sculpt or Paint tool for the eyedropper.";
		return false;
	}
	io::JsonValue change = io::JsonValue::make_object();
	change.set("terrain_brush", std::move(members));
	out.request(request::set_workspace(io::json_write(change)));
	return true;
}

} // namespace

bool terrain_view_command(const TerrainViewContext &view, const ViewportCommand &command, CanvasRequests &out,
                          std::string &error, bool &handled) {
	handled = false;
	if (is_tile_command(command.name)) {
		handled = true;
		return plan_tile_command(view, command, out, error);
	}
	if (command.name == "eyedropper") {
		handled = true;
		return eyedropper(view, command, out, error);
	}
	if (command.name != "click" || !brush_live(view)) return false;
	// A click while the brush is live: one dab, a stamp or a tile's select.
	handled = true;
	if (!command.has_at) {
		error = "A click takes the point of the picture it is at, \"at\": [x, y].";
		return false;
	}
	const TerrainBrush &brush = view.context.input.view.workspace.terrain_brush;
	TerrainPlane plane = TerrainPlane::Heights;
	if (terrain_tool_plane(brush.tool, plane)) {
		TerrainPoint at;
		double height = 0.0;
		if (!view.space.pointer_ground(command.at_x, command.at_y, at, height)) {
			error = "No ground lies under that point of the picture.";
			return false;
		}
		stroke_at(view, at, 0, true, out);
		return true;
	}
	if (brush.tool == TerrainTool::Stamp) {
		ViewportDrop drop;
		drop.reference = "tile";
		drop.name = std::to_string(brush.cell);
		drop.x = command.at_x;
		drop.y = command.at_y;
		drop.kind = view.kind;
		return plan_tile_drop(view, drop, out, error);
	}
	return plan_tile_select(view, command, out, error);
}

bool terrain_view_drag(const TerrainViewContext &view, const ViewportDrag &drag, CanvasRequests &out, std::string &error,
                       bool &handled) {
	handled = false;
	if (drag.handle == "tile") {
		handled = true;
		return plan_tile_drag(view, drag, out, error);
	}
	if (drag.handle != "brush") return false;
	handled = true;
	TerrainPlane plane = TerrainPlane::Heights;
	if (!terrain_tool_plane(view.context.input.view.workspace.terrain_brush.tool, plane)) {
		error = "The brush's tool strokes no plane: pick a Sculpt or Paint tool (set_workspace's terrain_brush).";
		return false;
	}
	if (drag.by) {
		error = "A brush drag goes to a point of the picture, \"to\": [x, y].";
		return false;
	}
	TerrainPoint at;
	double height = 0.0;
	if (!view.space.pointer_ground(drag.x, drag.y, at, height)) {
		error = "No ground lies under that point of the picture.";
		return false;
	}
	stroke_at(view, at, drag.gesture, drag.end, out);
	return true;
}

bool terrain_view_drop(const TerrainViewContext &view, const ViewportDrop &drop, CanvasRequests &out, std::string &error,
                       bool &handled) {
	handled = drop.reference == "tile";
	return handled && plan_tile_drop(view, drop, out, error);
}

} // namespace opennova::editor
