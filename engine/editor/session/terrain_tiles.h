#pragma once

#include <string>

namespace opennova::editor {

class SessionCore;
struct EditorRequest;

// A batch of the tile tools (ADR 0046 S25 B2): an edit_record whose terrain_view names the view (a mission or a
// terrain) whose Tiles tools made it. Its edits go to the tile placement the view's tiles go to (session/
// terrain_family's tiles: a mission's <mission>.til, which the game reads in place of the terrain's own; a terrain's
// own under no mission): refused where that file refuses (an import's output) or is another than the request's path;
// made first where the mission has none (seeded with the terrain's own tiles, which the mission's file hides once it
// exists, the game reading one [orig: PolyTrn_LoadTerrainConfig @ 0x60E6D2..0x60E6E5]; one step of the file history);
// opened quietly; and the batch applied as a terrain stroke's is (DocumentSet::apply_terrain_edits), so the view's
// Undo, Save and close take it with the view. A batch of no edits opens the file alone (the Tiles tool's first look).
void serve_terrain_tiles_edit(SessionCore &core, const EditorRequest &request);

} // namespace opennova::editor
