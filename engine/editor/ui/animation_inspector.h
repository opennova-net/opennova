#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The animation types' parts of the Inspector (ADR 0046 S17, the animation lane), over the portable
// words of preview/animation_uses: what a map's row is for (its slot's meaning and the map's rules in
// words, the slots it leaves out) and who plays the map (the items with their models, the weapons);
// a clip's length in frames and seconds and the maps and slots that play it, each a Go to.
bool draw_animation_map_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                  InspectorTaken &taken);
bool draw_clip_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                         InspectorTaken &taken);

} // namespace opennova::editor
