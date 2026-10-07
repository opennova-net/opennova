#pragma once

#include <editor/ui/workspace.h>

namespace opennova::editor {

class DocumentBase;

// The HUD layout type's part of the Inspector (the deep-integration plan's DI-37): the element picked in
// its HUD preview, its words and each of its fields (preview/hud_layout_edit: its place and size in the
// 1024 x 768 design space, its anchor and hidden value, the fonts it writes in, its colour's channels, its
// detail level's flags), each a number in the game's range, a word or a name, its range and witness in its
// tooltip; a change is a set command of the viewport (one undo step), written to the element's line.
// Nothing picked: how to pick one.
void draw_hud_layout_inspector(Workspace &workspace, const DocumentBase &document);

} // namespace opennova::editor
