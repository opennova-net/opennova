#pragma once

#include <memory>

#include <editor/ui/document_views.h>

namespace opennova::editor {

// The terrain heights' Document tab (S25; the heights type's row of ui/document_views): their file page. What the file
// is (a .cpt in place, or an imported terrain's heightmap and the .cpt its import makes), what the heights span, the
// heights past TrnGen's 128-unit draw-back and the depth section's steep stretches (each what the game makes of it),
// the regions whose bake is due, and where they are sculpted: a mission's or the terrain's view, Terrain (Ctrl+E).
std::unique_ptr<DocumentView> make_terrain_heights_view();

} // namespace opennova::editor
