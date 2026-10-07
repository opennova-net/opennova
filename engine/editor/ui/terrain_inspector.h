#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The terrain type's part of the Inspector (the deep-integration plan's DI-30), over the portable
// session/terrain_uses: for a terrain an import makes, the import that makes it (its terrain set and the
// images it names, each a Go to, its options and a Reimport), the edits it does not take saying so; the
// missions that run on it, each a Go to, with the environment and the tile set it loads beside it; a grid
// row's and a cell's place in the world.
bool draw_terrain_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                            InspectorTaken &taken);

} // namespace opennova::editor
