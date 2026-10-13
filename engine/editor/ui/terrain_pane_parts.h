#pragma once

#include <editor/session/terrain_family.h>
#include <editor/session/view/terrain_tools_view.h>

namespace opennova::editor {

class DocumentBase;
class Workspace;

// The Terrain pane's parts (ADR 0046 S25, the shared seams; ui/terrain_pane draws the pane), each lane's in its own
// file: what each draws of the session's terrain brush over the view document (a mission or a terrain) and the files
// its brush edits as the game resolves them for it. Each raises its changes as requests (set_workspace's
// terrain_brush, an edit_in_viewport command), never touching the session.
struct TerrainPaneContext {
	Workspace &workspace;
	const DocumentBase &document;
	const TerrainFamily &family;
	const TerrainBrush &brush;
};

// The brush's shape (radius, strength, falloff, spacing) and the lock line (the views': ui/terrain_pane.cpp).
void draw_brush_part(TerrainPaneContext &context);
// The sculpt tools' values (Set's height, the limits shown) and the heights file's bake (the heights':
// ui/terrain_sculpt_parts.cpp).
void draw_sculpt_part(TerrainPaneContext &context);
void draw_bake_part(TerrainPaneContext &context);
// The maps' palettes (the legend, the foliage codes, the colour, the detail layers) and the tile tools' palette and
// operations (the maps' and the tiles': ui/terrain_palettes.cpp).
void draw_paint_part(TerrainPaneContext &context);
void draw_tiles_part(TerrainPaneContext &context);

} // namespace opennova::editor
