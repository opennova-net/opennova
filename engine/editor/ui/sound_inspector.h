#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The sound types' parts of the Inspector (the sound lane, DI-02): a bank's set, layer or member with a
// Play that plays the set as the game picks it, and what the last play of it played; a wave with a Play of
// its file; a profile's slot with a Play of the set it names, and a footstep slot's Play on each surface
// the game's test tells apart (ground, snow, an object, water). Every Play is a play_sound request
// (session/sound_play.h), so the Shell's one preview player plays it and the MCP sees it.
bool draw_sound_bank_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                               InspectorTaken &taken);
bool draw_sound_profile_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                  InspectorTaken &taken);
// A dialog bank's dialog or line (DI-32): the number a mission plays it by, a Play of the dialog as the game plays
// it (its lines one after another), a line's Play alone, and what the last play of it said.
bool draw_dialog_bank_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                InspectorTaken &taken);

} // namespace opennova::editor
