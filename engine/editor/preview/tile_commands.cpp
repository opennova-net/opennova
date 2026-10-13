// The tile tools in a view (ADR 0046 S25 B2; preview/terrain_view_commands.h's planners): a view's tile command, its
// stamp, its tile drag and its click's select planned as requests over the .til the view's tiles go to
// (session/terrain_family's tiles), each batch an edit_record of the view's terrain_view (session/terrain_tiles.h).
#include <editor/preview/terrain_view_commands.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/io/json.h>
#include <editor/documents/til_document.h>
#include <editor/model/document_base.h>
#include <editor/preview/tile_tools.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// The view's document, refused where the session takes no edit now.
const DocumentBase *view_document(const TerrainViewContext &view, std::string &error) {
	const DocumentBase *document = view.context.input.document;
	if (!document) {
		error = "No mission or terrain is open in this view.";
		return nullptr;
	}
	if (!view.context.editable()) {
		error = view.context.not_editable();
		return nullptr;
	}
	return document;
}

// The .til the view's tiles go to: its path, and its document where it is open (null where not, or not made yet).
bool tiles_of(const TerrainViewContext &view, const DocumentBase &document, std::string &path, const TilDocument *&tiles,
              std::string &error, bool *missing = nullptr) {
	tiles = nullptr;
	if (missing) *missing = false;
	TerrainFamily family;
	if (!resolve_terrain_family(view.context.input.view, document, family)) {
		error = family.error;
		return false;
	}
	if (!family.tiles.refusal.empty()) {
		error = family.tiles.refusal;
		return false;
	}
	if (family.tiles.path.empty()) {
		error = basename_of(document.path()) + " has no tile placement: its terrain names none.";
		return false;
	}
	path = family.tiles.path;
	if (missing) *missing = family.tiles.missing;
	for (const auto &open : view.context.input.view.documents.open)
		if (open->path() == path) tiles = dynamic_cast<const TilDocument *>(open.get());
	return true;
}

// A batch of the tile tools: to the view's tile placement, the selection as it was.
EditorRequest tile_edit(const TerrainViewContext &view, const std::string &tiles, std::vector<Edit> edits) {
	return request::tile_edit(view.view, tiles, std::move(edits));
}

// The view's tile selection set to `ids` (rows of the .til, by identity).
EditorRequest select_tiles(const TerrainViewContext &view, const std::vector<NodeId> &ids) {
	io::JsonValue tiles = io::JsonValue::make_array();
	for (const NodeId id : ids) tiles.push(io::json_number(double(id)));
	io::JsonValue members = io::JsonValue::make_object();
	members.set("path", io::json_string(view.view));
	members.set("tiles", std::move(tiles));
	io::JsonValue change = io::JsonValue::make_object();
	change.set("document", std::move(members));
	return request::set_workspace(io::json_write(change));
}

const std::vector<NodeId> &kept_selection(const TerrainViewContext &view) {
	return view.context.input.view.workspace.document(view.view).tiles;
}

// A command's whole quarter count `by` (its first), none 1.
bool quarters_of(const ViewportCommand &command, int &out, std::string &error) {
	const double by = command.by.empty() ? 1.0 : command.by[0];
	if (!(std::fabs(by) <= 3.0) || by != std::floor(by) || by == 0.0) {
		error = command.name + "'s \"by\" is the quarter turns, -3 to 3 (clockwise positive), not 0.";
		return false;
	}
	out = int(by);
	return true;
}

std::string tiles_words(size_t count) { return count == 1 ? std::string("1 tile") : std::to_string(count) + " tiles"; }

} // namespace

bool plan_tile_command(const TerrainViewContext &view, const ViewportCommand &command, CanvasRequests &out,
                       std::string &error) {
	const DocumentBase *document = view_document(view, error);
	if (!document) return false;
	std::string path;
	const TilDocument *tiles = nullptr;
	if (!tiles_of(view, *document, path, tiles, error)) return false;
	const std::vector<NodeId> &rows = command.ids.empty() ? kept_selection(view) : command.ids;
	if (!tiles || rows.empty()) {
		error = "Select tiles first (the Tiles tool's click).";
		return false;
	}
	std::vector<Edit> edits;
	std::string words;
	if (command.name == "tile_rotate" || command.name == "tile_group_rotate") {
		// Positive clockwise on the plane as the views show it, north up (the planners' quarters run the other way).
		int by = 0;
		if (!quarters_of(command, by, error)) return false;
		if (command.name == "tile_rotate") {
			edits = plan_tile_turn(*tiles, rows, -by);
		} else {
			// MED's group turn is one quarter a press; more are as many presses.
			if (std::abs(by) != 1) {
				error = "tile_group_rotate turns a quarter at a time: \"by\" 1 (clockwise) or -1.";
				return false;
			}
			edits = plan_tile_group_turn(*tiles, rows, -by);
		}
		words = std::string(by > 0 ? "Turned " : "Turned back ") + tiles_words(rows.size());
	} else if (command.name == "tile_flip") {
		if (command.value != "h" && command.value != "v") {
			error = "tile_flip's \"value\" is h (across, east to west) or v (north to south).";
			return false;
		}
		edits = plan_tile_flip(*tiles, rows, command.value == "h");
		words = "Flipped " + tiles_words(rows.size());
	} else if (command.name == "tile_layer") {
		const double by = command.by.empty() ? 1.0 : command.by[0];
		if (by == 0.0) {
			error = "tile_layer's \"by\" is 1 (over the others) or -1 (under them).";
			return false;
		}
		edits = plan_tile_layer(*tiles, rows, by > 0.0);
		if (edits.empty()) {
			error = by > 0.0 ? "The selected tiles are drawn over the others already." : "The selected tiles are drawn under the others already.";
			return false;
		}
		words = by > 0.0 ? "Laid the selected tiles over the others" : "Laid the selected tiles under the others";
	} else if (command.name == "tile_delete") {
		edits = plan_tile_delete(*tiles, rows);
		words = "Removed " + tiles_words(edits.size());
	} else if (command.name == "tile_nudge") {
		// MED's arrows: each press a whole unit, 16 with the modifier, a positive move rounding up [orig: JOTACmed
		// sub_43F0B0 @ 0x43F0DE..0x43F1A7] (tile_tools tile_nudged).
		if (command.by.size() != 2 || (command.by[0] == 0.0 && command.by[1] == 0.0)) {
			error = "tile_nudge's \"by\" is [east, north]: whole steps (16 units each with \"value\": \"16\").";
			return false;
		}
		const int dx = command.by[0] > 0.0 ? 1 : command.by[0] < 0.0 ? -1 : 0;
		const int dy = command.by[1] > 0.0 ? 1 : command.by[1] < 0.0 ? -1 : 0;
		edits = plan_tile_nudge(*tiles, rows, dx, dy, command.value == "16");
		words = "Nudged " + tiles_words(rows.size());
	} else if (command.name == "tile_duplicate") {
		const int32_t dx = command.by.size() == 2 ? int32_t(std::lround(command.by[0])) : 16;
		const int32_t dy = command.by.size() == 2 ? int32_t(std::lround(command.by[1])) : 0;
		edits = plan_tile_duplicate(*tiles, rows, dx, dy);
		words = "Duplicated " + tiles_words(rows.size());
	} else {
		error = "No tile command \"" + command.name + "\".";
		return false;
	}
	if (edits.empty()) {
		error = "None of the selected tiles is a tile of " + path + ".";
		return false;
	}
	const bool removed = command.name == "tile_delete";
	out.request(tile_edit(view, path, std::move(edits)));
	if (removed) out.request(select_tiles(view, {}));
	out.served(words, 0);
	return true;
}

bool plan_tile_drop(const TerrainViewContext &view, const ViewportDrop &drop, CanvasRequests &out, std::string &error) {
	const DocumentBase *document = view_document(view, error);
	if (!document) return false;
	std::string path;
	const TilDocument *tiles = nullptr;
	if (!tiles_of(view, *document, path, tiles, error)) return false;
	char *end = nullptr;
	const long cell = std::strtol(drop.name.c_str(), &end, 10);
	if (drop.name.empty() || *end || cell < 0 || cell > 255) {
		error = "A tile's drop names its cell of the tile set, 0 to 255, not \"" + drop.name + "\".";
		return false;
	}
	if (drop.box) {
		error = "A tile is stamped at a point, not over a box.";
		return false;
	}
	TerrainPoint at;
	double height = 0.0;
	if (!view.space.pointer_ground(drop.x, drop.y, at, height)) {
		error = "No ground lies under that point of the picture.";
		return false;
	}
	const TerrainBrush &brush = view.context.input.view.workspace.terrain_brush;
	std::vector<Edit> edits = plan_tile_stamp(int(cell), tile_stamp_flags(brush.rotation, brush.flip_h, brush.flip_v), at.x,
	                                          at.y, drop.snap > 0.0f);
	if (edits.empty()) {
		error = "A tile's corner there lies past the range a .til record holds (32767 units either way).";
		return false;
	}
	out.request(tile_edit(view, path, std::move(edits)));
	out.served("Stamped tile " + std::to_string(cell) + " in " + basename_of(path), 0);
	return true;
}

bool plan_tile_drag(const TerrainViewContext &view, const ViewportDrag &drag, CanvasRequests &out, std::string &error) {
	const DocumentBase *document = view_document(view, error);
	if (!document) return false;
	std::string path;
	const TilDocument *tiles = nullptr;
	if (!tiles_of(view, *document, path, tiles, error)) return false;
	if (!(drag.snap >= 0.0f)) {
		error = "The snap is 0 or more.";
		return false;
	}
	const TilRow *held = nullptr;
	if (tiles)
		for (const auto &row : tiles->rows())
			if (row && row->kind == node_kind(TilKind::Tile) && row->id == drag.id) held = static_cast<const TilRow *>(row.get());
	if (!held) {
		error = "Tile " + std::to_string(drag.id) + " is no tile of " + path + (tiles ? "." : " (not open).");
		return false;
	}
	// The tile's corner goes to the ground under the pointer, as Nile's cursor carries a tile by its corner (`by`: from
	// where the picture shows the corner).
	const TilOverlayEntry &entry = held->entry;
	const TerrainPoint corner{double(entry.x_fixed) / 65536.0, double(entry.z_fixed) / 65536.0};
	float x = drag.x, y = drag.y;
	if (drag.by) {
		float hx = 0.0f, hy = 0.0f;
		if (!view.space.to_picture(corner, view.space.planar() ? 0.0 : view.space.height(corner), hx, hy)) {
			error = "The tile is not on the picture: drag it to a point, \"to\": [x, y].";
			return false;
		}
		x += hx;
		y += hy;
	}
	TerrainPoint at;
	double height = 0.0;
	if (!view.space.pointer_ground(x, y, at, height)) {
		error = "No ground lies under that point of the picture.";
		return false;
	}
	// Its corner to whole units as Nile's stamp rounds one (snapped: the grid square the point lies in).
	int32_t x_fixed = 0, z_fixed = 0;
	if (!tile_corner_at(at.x, at.y, drag.snap > 0.0f, x_fixed, z_fixed)) {
		error = "A tile's corner there lies past the range a .til record holds (32767 units either way).";
		return false;
	}
	// The selection goes with it where it is among the selected; else it goes alone.
	const std::vector<NodeId> &kept = kept_selection(view);
	const std::vector<NodeId> moved =
	        std::find(kept.begin(), kept.end(), drag.id) != kept.end() ? kept : std::vector<NodeId>{drag.id};
	const uint64_t gesture = drag.gesture ? drag.gesture : next_edit_gesture();
	std::vector<Edit> edits = plan_tile_move(*tiles, moved, int64_t(x_fixed) - entry.x_fixed,
	                                         int64_t(z_fixed) - entry.z_fixed, gesture);
	const bool wrote = !edits.empty();
	if (wrote) out.request(tile_edit(view, path, std::move(edits)));
	if (drag.end && (wrote || drag.gesture)) out.request(request::end_edit(std::string(), gesture));
	return true;
}

bool plan_tile_select(const TerrainViewContext &view, const ViewportCommand &click, CanvasRequests &out, std::string &error) {
	const DocumentBase *document = view.context.input.document;
	if (!document) {
		error = "No mission or terrain is open in this view.";
		return false;
	}
	std::string path;
	const TilDocument *tiles = nullptr;
	bool missing = false;
	if (!tiles_of(view, *document, path, tiles, error, &missing)) return false;
	if (!tiles && missing) {
		// Not made yet: nothing to select in it, and nothing made (the first stamp makes it, seeded with what the game
		// draws there now).
		out.served(basename_of(path) + " is not made yet: stamp a tile to make it (with the tiles the game draws there "
		                               "now), then select",
		           0);
		return true;
	}
	if (!tiles) {
		// Not open: opened (a batch of none), its tiles selected by the next click.
		out.request(tile_edit(view, path, {}));
		out.served("Opened " + basename_of(path) + " for the Tiles tool: click a tile to select it", 0);
		return true;
	}
	if (!click.has_at) {
		error = "A click takes the point of the picture it is at, \"at\": [x, y].";
		return false;
	}
	TerrainPoint at;
	double height = 0.0;
	const bool on_ground = view.space.pointer_ground(click.at_x, click.at_y, at, height);
	const size_t hit = on_ground ? tile_at(tiles->file(), at.x, at.y) : size_t(-1);
	const NodeId id = hit < tiles->rows().size() ? tiles->rows()[hit]->id : 0;
	std::vector<NodeId> ids = kept_selection(view);
	switch (click.mode) {
	case SelectMode::Replace:
		ids.clear();
		if (id) ids.push_back(id);
		break;
	case SelectMode::Add:
		if (id && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
		break;
	case SelectMode::Toggle:
		if (id) {
			const auto found = std::find(ids.begin(), ids.end(), id);
			if (found == ids.end()) ids.push_back(id);
			else ids.erase(found);
		}
		break;
	}
	out.request(select_tiles(view, ids));
	if (id) {
		const TilOverlayEntry &entry = static_cast<const TilRow *>(tiles->rows()[hit].get())->entry;
		out.served("Tile " + std::to_string(hit + 1) + " of " + basename_of(path) + ": cell " + std::to_string(entry.tile_index) +
		                   (ids.size() > 1 ? ", " + tiles_words(ids.size()) + " selected" : std::string()),
		           0);
	}
	return true;
}

} // namespace opennova::editor
