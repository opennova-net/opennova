#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The environment type's part of the Inspector (the deep-integration plan's DI-19a), over the
// portable session/environment_uses: the missions that run on it, each a Go to, with the terrain it
// pairs with, what its header sets over this environment (each a Go to on the header's field), the
// clock it starts on, where its water plane comes from, and each terrain key of this file its terrain
// takes over its .trn's and overcast.def's (a Go to on the key, the value it sets over and whose); a
// keyframe's place in the day; on a terrain key, that key's line of each mission alone.
bool draw_environment_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                InspectorTaken &taken);

} // namespace opennova::editor
