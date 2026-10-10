#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The music bank's part of the Inspector (round S23 lane A): a stream's place, which the music script plays it by, a
// Play of it as the game streams it (a play_sound request: the Shell's one preview player streams the bank's file),
// a Stop, and what the last play said; the bank's streams in words.
bool draw_music_bank_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                               InspectorTaken &taken);

} // namespace opennova::editor
