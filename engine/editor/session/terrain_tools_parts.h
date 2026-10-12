#pragma once

#include <cstddef>
#include <string>

#include <base/io/json.h>
#include <editor/session/view/workspace_view.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

class DocumentBase;
struct SessionView;

// The terrain tools' parts of the workspace (ADR 0046 S25, the shared seams; session/workspace_parts.h's table):
// the session-wide brush (its own part, `terrain_brush`) and a document's Terrain pane and tile selection (members
// of the `document` part). workspace_parts.cpp reaches them through the hooks below, one line each.

// The terrain_brush part's members, in their order (the catalog's).
inline constexpr WorkspaceMember kTerrainBrushMembers[] = {
	{"tool", WorkspaceJson::String,
			"The tool (none: the views take their mission tools again; raise, lower, smooth, flatten, set: the heights; "
			"surface, foliage, colour, detail: the maps; stamp, tiles: the .til's tiles)."},
	{"radius", WorkspaceJson::Number, "Its radius, metres, 0 to 512 (0: one texel of the plane, the original editor's click)."},
	{"strength", WorkspaceJson::Number, "Its strength, 0 to 1 (a colour's opacity)."},
	{"falloff", WorkspaceJson::Number, "The share of its radius it fades over to its rim, 0 (hard) to 1."},
	{"spacing", WorkspaceJson::Number, "A dab every spacing x radius along the pointer's ground path, 0.05 to 2."},
	{"height", WorkspaceJson::Number, "Set's height, world units (TrnGen's raw / 256), 0 to 255.99."},
	{"surface", WorkspaceJson::Integer, "The surface class Surface paints, 0 to 255 (1 the game's default)."},
	{"foliage", WorkspaceJson::Integer, "The foliage code Foliage paints, 0 to 255 (0 grows nothing)."},
	{"colour", WorkspaceJson::String, "The colour Colour paints, \"#RRGGBB\"."},
	{"layer", WorkspaceJson::Integer, "The detail layer Detail paints, 0 to 2 (the blend map's red, green, blue)."},
	{"cell", WorkspaceJson::Integer, "The tile set's cell Stamp places, 0 to 255."},
	{"rotation", WorkspaceJson::Integer, "The stamped tile's quarter turns clockwise (north up), 0 to 3."},
	{"flip_h", WorkspaceJson::Boolean, "The stamped tile flipped across."},
	{"flip_v", WorkspaceJson::Boolean, "The stamped tile flipped up and down."},
	{"cursor", WorkspaceJson::Numbers,
			"Where the brush's ring shows with no pointer over a view, [x, y] in mission metres (an MCP client's "
			"screenshot); null for none."},
	{"fill", WorkspaceJson::Boolean,
			"The Paint tools fill: a click fills the texels joined to the one under it (left, right, up, down) that hold "
			"its value, wherever the terrain draws them."},
};
// A set_workspace's terrain_brush: the members it names set in the session's brush, each checked first (nothing set
// when one is refused, `error` why). True when the brush moved.
bool set_terrain_brush(SessionView &view, const io::JsonValue &part, std::string &error);
// The brush in the workspace section.
io::JsonValue terrain_brush_json(const WorkspaceView &workspace);

// A set_workspace's document part, its terrain members (terrain, tiles) set in `shown` (the document's views as the
// change leaves them): refused (false, `error` why) for a document that is neither a mission nor a terrain, tiles
// that are no rows of the .til its tiles go to, and a Terrain pane and a Logic pane both asked; the Terrain pane
// shown hides the Logic pane and the Logic pane shown hides it (one pane beside the 3D view).
bool set_document_terrain(const SessionView &view, const io::JsonValue &part, const DocumentBase &document,
		WorkspaceView::DocumentView &shown, std::string &error);
// Whether the terrain members of two states of a document's views differ.
bool document_terrain_moved(const WorkspaceView::DocumentView &held, const WorkspaceView::DocumentView &shown);
// The document's terrain members in the workspace section (a mission's or a terrain's: terrain, tiles).
void document_terrain_json(const DocumentBase &document, const WorkspaceView::DocumentView &shown, io::JsonValue &out);
// Whether a document of `kind` has a Terrain pane (a mission, a terrain).
bool terrain_pane_kind(AssetKind kind);
// Whether the views of `document` brush now (S25): its Terrain pane shows and the brush's tool is not None; the
// canvases then take a press as the brush's, never their mission tool's.
bool terrain_brush_live(const SessionView &view, const DocumentBase &document);

} // namespace opennova::editor
