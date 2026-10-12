#pragma once

#include <string>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/terrain_brush.h>
#include <editor/preview/viewport_model.h>

namespace opennova::editor {

class MissionGround;

// What a view hands the terrain tools while its brush is live (ADR 0046 S25, the wire's `edit_in_viewport` in the
// mission's 3D view, its map and the terrain viewport): the view, its kind, the brush's space over its picture and the
// ground as the game reads it there (null: none).
struct TerrainViewContext {
	const ViewportContext &context;
	const TerrainBrushSpace &space;
	const std::string &view;
	ViewportKind kind = ViewportKind::kCount;
	const MissionGround *ground = nullptr;
};

// The terrain tools' commands and drags in a view, planned as the canvas's brush half raises them (each a request):
// - drag {id 0, handle "brush", to}: one sample of a stroke, a terrain_stroke of the ground under the point (the
//   drag's gesture and end its own);
// - click {at} while the brush is live: one dab of a Sculpt or Paint tool (a stroke of one point), a Stamp's tile
//   laid there, the Tiles tool's select;
// - eyedropper {at}: the value under the point taken into the brush (Set's and Flatten's height, Surface's class,
//   Foliage's code: a set_workspace of terrain_brush);
// - the tile commands (tile_rotate {by}, tile_flip {value h|v}, tile_layer {by}, tile_delete, tile_group_rotate {by})
//   and the tile drop {reference "tile", name <cell>} and drag {id <.til row>, handle "tile"}: the tile tools'
//   (the planners below).
// `handled` false: no terrain tool's (the viewport's own planning goes on). True with a refusal in `error`.
bool terrain_view_command(const TerrainViewContext &view, const ViewportCommand &command, CanvasRequests &out,
                          std::string &error, bool &handled);
bool terrain_view_drag(const TerrainViewContext &view, const ViewportDrag &drag, CanvasRequests &out, std::string &error,
                       bool &handled);
bool terrain_view_drop(const TerrainViewContext &view, const ViewportDrop &drop, CanvasRequests &out, std::string &error,
                       bool &handled);

// The tile tools' planners (the maps' and the tiles' lane, preview/tile_tools; stand-ins refusing "not ready yet" in
// preview/terrain_tile_stubs.cpp until then): what a view's tile command, its tile drop (a Stamp) and its tile drag
// plan as requests over the .til its tiles go to (session/terrain_family's tiles), and a click's select of the tile
// under the point. False, nothing planned, with why. The commands are the original tools' (Rotate CW/CCW, Flip H/V,
// Tile Layer Up/Down, Delete, a group's rotation about its centroid).
bool plan_tile_command(const TerrainViewContext &view, const ViewportCommand &command, CanvasRequests &out,
                       std::string &error);
bool plan_tile_drop(const TerrainViewContext &view, const ViewportDrop &drop, CanvasRequests &out, std::string &error);
bool plan_tile_drag(const TerrainViewContext &view, const ViewportDrag &drag, CanvasRequests &out, std::string &error);
bool plan_tile_select(const TerrainViewContext &view, const ViewportCommand &click, CanvasRequests &out, std::string &error);

} // namespace opennova::editor
