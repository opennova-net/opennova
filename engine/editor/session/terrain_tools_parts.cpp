#include <editor/session/terrain_tools_parts.h>

#include <algorithm>
#include <iterator>

#include <editor/model/document.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

// The .til rows `ids` names, each a row of the open document at `tiles`: false with why where one is not.
bool tiles_held(const SessionView &view, const std::string &tiles, const std::vector<NodeId> &ids, std::string &error) {
	if (ids.empty()) return true;
	if (tiles.empty()) {
		error = "This document has no tile layout its tiles go to yet.";
		return false;
	}
	const Document *records = nullptr;
	for (const auto &document : view.documents.open)
		if (document->path() == tiles) records = records_of(*document);
	if (!records) {
		error = tiles + " is not open: its tiles are selected once it is.";
		return false;
	}
	for (const NodeId id : ids)
		if (!records->address_of(id).row) {
			error = tiles + " has no tile " + std::to_string(id) + ".";
			return false;
		}
	return true;
}

} // namespace

bool terrain_pane_kind(AssetKind kind) { return kind == AssetKind::Mission || kind == AssetKind::Terrain; }

bool terrain_brush_live(const SessionView &view, const DocumentBase &document) {
	return terrain_pane_kind(document.kind()) && view.workspace.terrain_brush.tool != TerrainTool::None &&
	       view.workspace.document(document.path()).terrain;
}

bool set_terrain_brush(SessionView &view, const JsonValue &part, std::string &error) {
	TerrainBrush brush = view.workspace.terrain_brush;
	uint32_t named = 0;
	if (!read_terrain_brush(part, brush, named, false, error)) {
		error = "workspace.terrain_" + error;
		return false;
	}
	if (brush == view.workspace.terrain_brush) return false;
	view.workspace.terrain_brush = brush;
	return true;
}

JsonValue terrain_brush_json(const WorkspaceView &workspace) { return terrain_brush_to_json(workspace.terrain_brush); }

bool set_document_terrain(const SessionView &view, const JsonValue &part, const DocumentBase &document,
		WorkspaceView::DocumentView &shown, std::string &error) {
	const JsonValue *terrain = part.get("terrain");
	const JsonValue *tiles = part.get("tiles");
	if (!terrain && !tiles) {
		// The Logic pane shown hides the Terrain pane (one pane beside the 3D view).
		if (const JsonValue *logic = part.get("logic"); logic && logic->boolean) shown.terrain = false;
		return true;
	}
	const std::string &at = document.path();
	if (!terrain_pane_kind(document.kind())) {
		error = at + " has no Terrain pane: a mission's and a terrain's show beside their 3D view.";
		return false;
	}
	const JsonValue *logic = part.get("logic");
	if (terrain && terrain->boolean && logic && logic->boolean) {
		error = "document.terrain and document.logic share the place beside the 3D view: show one of them.";
		return false;
	}
	if (terrain) {
		shown.terrain = terrain->boolean;
		if (shown.terrain) shown.logic = false;
	}
	if (tiles) {
		std::vector<NodeId> ids;
		for (const JsonValue &id : tiles->array) ids.push_back(NodeId(id.number));
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		TerrainFamily family;
		resolve_terrain_family(view, document, family);
		if (!tiles_held(view, family.tiles.path, ids, error)) return false;
		shown.tiles = std::move(ids);
	}
	return true;
}

bool document_terrain_moved(const WorkspaceView::DocumentView &held, const WorkspaceView::DocumentView &shown) {
	return held.terrain != shown.terrain || held.tiles != shown.tiles;
}

void document_terrain_json(const DocumentBase &document, const WorkspaceView::DocumentView &shown, JsonValue &out) {
	if (!terrain_pane_kind(document.kind())) return;
	out.set("terrain", JsonValue::make_bool(shown.terrain));
	JsonValue tiles = JsonValue::make_array();
	for (const NodeId id : shown.tiles) tiles.push(io::json_number(double(id)));
	out.set("tiles", std::move(tiles));
}

} // namespace opennova::editor
