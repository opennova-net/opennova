#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The environment type's part of the Inspector (the deep-integration plan's DI-19a), over the
// portable session/environment_uses: the missions that run on it, each a Go to, with the terrain it
// pairs with, what its header sets over this environment (each a Go to on the header's field), the
// clock it starts on and where its water plane comes from; a keyframe's place in the day.
bool draw_environment_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                InspectorTaken &taken);

} // namespace opennova::editor
