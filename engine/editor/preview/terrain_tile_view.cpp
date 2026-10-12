// The views' tile commands, drops, drags and clicks (ADR 0046 S25, terrain_view_commands.h), planned over the tile
// tools' planners (preview/tile_tools.h) on the .til the view's tiles go to (session/terrain_family's tiles): each a
// batch of edits on that document (an edit_record that leaves the view's selection and its active document as they
// are), or the view's tile selection (a set_workspace of the document view's `tiles`).
#include <editor/preview/terrain_view_commands.h>

#include <algorithm>
#include <cmath>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/til_document.h>
#include <editor/model/document_base.h>
#include <editor/preview/tile_tools.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// The .til the view's tiles go to, its document where it is open, and the tiles the view has selected.
struct TileTarget {
	std::string path;
	const TilDocument *document = nullptr;
	std::vector<NodeId> selected;
};

bool tile_target(const TerrainViewContext &view, TileTarget &out, std::string &error) {
	const SessionView &session = view.context.input.view;
	const DocumentBase *document = view.context.input.document;
	if (!document) {
		error = "Open the mission or the terrain first.";
		return false;
	}
	TerrainFamily family;
	if (!resolve_terrain_family(session, *document, family)) {
		error = family.error;
		return false;
	}
	if (!family.tiles.refusal.empty()) {
		error = family.tiles.refusal;
		return false;
	}
	if (family.tiles.path.empty() || family.tiles.missing) {
		error = "The project has no " + (family.tiles.name.empty() ? std::string("tile layout") : family.tiles.name) +
		        " for these tiles yet: make it first (New file, a tile placement).";
		return false;
	}
	out.path = family.tiles.path;
	for (const auto &open : session.documents.open)
		if (open->path() == out.path) out.document = dynamic_cast<const TilDocument *>(open.get());
	out.selected = session.workspace.document(view.view).tiles;
	return true;
}

// The batch on the .til, leaving the view's selection and active document as they are.
void raise_batch(const std::string &path, std::vector<Edit> edits, CanvasRequests &out) {
	EditorRequest batch = request::edit_record(path, std::move(edits));
	batch.keep_selection = true;
	out.request(std::move(batch));
}

// The tiles a command acts on: those it names, else the view's selection.
std::vector<NodeId> acted_on(const ViewportCommand &command, const TileTarget &target) {
	return command.ids.empty() ? target.selected : command.ids;
}

// A whole number of quarter turns or steps from a command's `by` (1 when left out).
int steps_of(const ViewportCommand &command) {
	return command.by.empty() ? 1 : int(std::lround(command.by.front()));
}

void set_tiles(const TerrainViewContext &view, const std::vector<NodeId> &ids, CanvasRequests &out) {
	io::JsonValue tiles = io::JsonValue::make_array();
	for (const NodeId id : ids) tiles.push(io::json_number(double(id)));
	io::JsonValue members = io::JsonValue::make_object();
	members.set("path", io::json_string(view.view));
	members.set("tiles", std::move(tiles));
	io::JsonValue change = io::JsonValue::make_object();
	change.set("document", std::move(members));
	out.request(request::set_workspace(io::json_write(change)));
}

} // namespace

bool plan_tile_command(const TerrainViewContext &view, const ViewportCommand &command, CanvasRequests &out,
                       std::string &error) {
	TileTarget target;
	if (!tile_target(view, target, error)) return false;
	const std::vector<NodeId> ids = acted_on(command, target);
	if (!target.document || ids.empty()) {
		error = "No tile is selected: click one with the Tiles tool first.";
		return false;
	}
	const TilDocument &document = *target.document;
	std::vector<Edit> edits;
	// The turns are the original tools' Rotate CW (by 1) and CCW (by -1), as the plane shows them from above; the
	// planners' quarter turns run counter-clockwise.
	if (command.name == "tile_rotate") {
		edits = plan_tile_turn(document, ids, -steps_of(command));
	} else if (command.name == "tile_group_rotate") {
		edits = plan_tile_group_turn(document, ids, -steps_of(command));
	} else if (command.name == "tile_flip") {
		if (command.value != "h" && command.value != "v") {
			error = "tile_flip takes value \"h\" (across) or \"v\" (up and down).";
			return false;
		}
		edits = plan_tile_flip(document, ids, command.value == "h");
	} else if (command.name == "tile_layer") {
		edits = plan_tile_layer(document, ids, steps_of(command) > 0);
	} else if (command.name == "tile_delete") {
		edits = plan_tile_delete(document, ids);
	} else {
		error = "No tile command \"" + command.name + "\" (tile_rotate, tile_flip, tile_layer, tile_delete, tile_group_rotate).";
		return false;
	}
	if (edits.empty()) {
		error = "That changes no tile.";
		return false;
	}
	raise_batch(target.path, std::move(edits), out);
	if (command.name == "tile_delete") set_tiles(view, {}, out);
	return true;
}

bool plan_tile_drop(const TerrainViewContext &view, const ViewportDrop &drop, CanvasRequests &out, std::string &error) {
	TileTarget target;
	if (!tile_target(view, target, error)) return false;
	const std::optional<int> cell = strutil::parse_int(drop.name);
	if (!cell || *cell < 0 || *cell > 255) {
		error = "A tile's drop names its cell, 0 to 255.";
		return false;
	}
	TerrainPoint at;
	double height = 0.0;
	if (!view.space.pointer_ground(drop.x, drop.y, at, height)) {
		error = "No ground lies under that point of the picture.";
		return false;
	}
	const TerrainBrush &brush = view.context.input.view.workspace.terrain_brush;
	std::vector<Edit> edits =
			plan_tile_stamp(*cell, tile_stamp_flags(brush.rotation, brush.flip_h, brush.flip_v), at.x, at.y, drop.snap >= 16.0f);
	raise_batch(target.path, std::move(edits), out);
	return true;
}

bool plan_tile_select(const TerrainViewContext &view, const ViewportCommand &click, CanvasRequests &out, std::string &error) {
	TileTarget target;
	if (!tile_target(view, target, error)) return false;
	if (!target.document) {
		error = "Open " + basename_of(target.path) + " to select its tiles (a stamp opens it).";
		return false;
	}
	TerrainPoint at;
	double height = 0.0;
	if (!click.has_at || !view.space.pointer_ground(click.at_x, click.at_y, at, height)) {
		error = "No ground lies under that point of the picture.";
		return false;
	}
	const TilFile file = target.document->file();
	const size_t index = tile_at(file, at.x, at.y);
	const std::vector<std::shared_ptr<const Node>> &rows = target.document->rows();
	std::vector<NodeId> tiles = click.mode == SelectMode::Replace ? std::vector<NodeId>() : target.selected;
	if (index < rows.size()) {
		const NodeId id = rows[index]->id;
		const auto held = std::find(tiles.begin(), tiles.end(), id);
		if (click.mode == SelectMode::Toggle && held != tiles.end()) tiles.erase(held);
		else if (held == tiles.end()) tiles.push_back(id);
	}
	set_tiles(view, tiles, out);
	return true;
}

bool plan_tile_drag(const TerrainViewContext &view, const ViewportDrag &drag, CanvasRequests &out, std::string &error) {
	TileTarget target;
	if (!tile_target(view, target, error)) return false;
	if (!target.document) {
		error = "Open " + basename_of(target.path) + " to move its tiles.";
		return false;
	}
	if (drag.by) {
		error = "A tile drag goes to a point of the picture, \"to\": [x, y]: the dragged tile's corner lands there.";
		return false;
	}
	const TilFile file = target.document->file();
	const std::vector<std::shared_ptr<const Node>> &rows = target.document->rows();
	size_t grabbed = rows.size();
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == drag.id) grabbed = i;
	if (grabbed >= rows.size() || grabbed >= file.entries.size()) {
		error = basename_of(target.path) + " has no tile " + std::to_string(drag.id) + ".";
		return false;
	}
	TerrainPoint at;
	double height = 0.0;
	if (!view.space.pointer_ground(drag.x, drag.y, at, height)) {
		error = "No ground lies under that point of the picture.";
		return false;
	}
	// The grabbed tile's corner to the point (whole units, or the 16 grid), the selection with it.
	int32_t x = 0, z = 0;
	tile_corner_at(at.x, at.y, drag.snap >= 16.0f, x, z);
	const int32_t dx = (x - file.entries[grabbed].x_fixed) >> 16, dy = -((z - file.entries[grabbed].z_fixed) >> 16);
	std::vector<NodeId> ids = target.selected;
	if (std::find(ids.begin(), ids.end(), drag.id) == ids.end()) ids = {drag.id};
	std::vector<Edit> edits = plan_tile_move(*target.document, ids, dx, dy, drag.gesture);
	if (!edits.empty()) raise_batch(target.path, std::move(edits), out);
	if (drag.end) out.request(request::end_edit(target.path, drag.gesture));
	return true;
}

} // namespace opennova::editor
