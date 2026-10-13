#include <editor/preview/terrain_view_commands.h>

#include <algorithm>
#include <cstdio>

#include <base/io/json.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/texture_document.h>
#include <editor/documents/texture_raster.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/terrain_map_target.h>
#include <editor/project/project_files.h>
#include <editor/session/terrain_family.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_tools_parts.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

bool is_tile_command(const std::string &name) {
	return name == "tile_rotate" || name == "tile_flip" || name == "tile_layer" || name == "tile_delete" ||
	       name == "tile_group_rotate" || name == "tile_nudge" || name == "tile_duplicate";
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

// The texel of a colour or blend map under a mission point as R, G, B, A: the open document's texels where one holds
// the map (a paint shows at once), else the file's; through the game's routing (preview/terrain_map_target). False,
// with why, where the map does not resolve or no texel lies there.
bool map_texel_at(const SessionView &view, const DocumentBase &document, TerrainPlane plane, TerrainPoint at, uint8_t rgba[4],
                  std::string &why) {
	TerrainFamily family;
	if (!resolve_terrain_family(view, document, family) || !family.trn) {
		why = family.error.empty() ? "the terrain does not resolve" : family.error;
		return false;
	}
	const TerrainFamilyFile &file = family.plane(plane);
	if (file.path.empty()) {
		why = file.refusal.empty() ? std::string("the terrain names no ") + (terrain_plane_words(plane) + 4) : file.refusal;
		return false;
	}
	RasterState read;
	const RasterState *state = nullptr;
	for (const auto &open : view.documents.open)
		if (open && open->path() == file.path)
			if (const auto *texture = dynamic_cast<const TextureDocument *>(open.get())) state = &texture->state();
	if (!state) {
		std::vector<uint8_t> bytes;
		if (!view.findings.assets || !view.findings.assets->read(basename_of(file.path), bytes)) {
			why = file.path + " does not read";
			return false;
		}
		read = read_texture_state(file.path, bytes);
		state = &read;
	}
	if (!texture_paintable(*state)) {
		why = file.path + " is no map the editor reads texels of";
		return false;
	}
	const RasterPlane &texels = state->planes.front();
	const TerrainMapTarget target = terrain_map_target(*family.trn, plane, texels.width(), texels.height());
	int tx = 0, ty = 0;
	std::vector<uint8_t> one;
	if (!terrain_map_texel(target, at.x, at.y, tx, ty) || !texture_rgba_rect(*state, tx, ty, 1, 1, one)) {
		why = "no texel of " + basename_of(file.path) + " lies there";
		return false;
	}
	std::copy_n(one.data(), 4, rgba);
	return true;
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
		} else if ((brush.tool == TerrainTool::Colour || brush.tool == TerrainTool::Detail) && view.context.input.document) {
			// The colour map's colour there; the blend map's heaviest of its three layers (its red, green, blue).
			uint8_t rgba[4] = {0, 0, 0, 0};
			std::string why;
			const TerrainPlane plane = brush.tool == TerrainTool::Colour ? TerrainPlane::Colour : TerrainPlane::Blend;
			if (!map_texel_at(view.context.input.view, *view.context.input.document, plane, at, rgba, why)) {
				error = "The eyedropper reads nothing there: " + why + ".";
				return false;
			}
			if (plane == TerrainPlane::Colour) {
				char hex[8];
				std::snprintf(hex, sizeof hex, "#%02X%02X%02X", rgba[0], rgba[1], rgba[2]);
				members.set("colour", io::json_string(hex));
			} else {
				members.set("layer", io::json_number(rgba[1] > rgba[0] && rgba[1] >= rgba[2] ? 1 : rgba[2] > rgba[0] ? 2 : 0));
			}
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
