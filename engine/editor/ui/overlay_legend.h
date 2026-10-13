#pragma once

#include <string>
#include <vector>

#include <editor/preview/mission_ground_overlay.h>
#include <editor/session/view/viewport_kind.h>
#include <formats/foliage/foliage.h>

namespace opennova::editor {

class ViewportCanvas;
class Workspace;

// A ground overlay's legend over a view's picture (DI-29; ADR 0046 S25, the painting feedback of 2026-10-13), the
// mission's 3D view's and the terrain viewport's: a panel in the picture's corner a third of it at most either way
// (ViewportCanvas::legend), a row for each class or grown definition the overlay holds (its swatch, its key and a
// short name, the game's words and its share in the row's tip) and, while the terrain brush paints the overlay's map
// in the view, the value it lays among them outlined (its own row where the picture holds none of it yet). Its toggle
// folds it to its title: the viewport's `legend` option (a set_viewport of the view `path` of `kind`), which `open` is.
// `defs` are the terrain's foliage definitions (the brush's foliage code to the definitions it grows).
void draw_overlay_legend(Workspace &workspace, ViewportCanvas &ui, const std::string &path, ViewportKind kind,
                         const MissionOverlayImage &overlay, const std::vector<FoliageDef> &defs, bool open);

} // namespace opennova::editor
