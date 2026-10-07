#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The menu's part of the Inspector (DI-34): over a window, or a record a window holds, what the game plays and
// when, as its pump plays the window's SOUND rows (preview/menu_sounds.h, menu::menu_window_sound): on hover
// (MOUSEIN), on click (SELECTED), on leaving (MOUSEOUT), each the set of its row's own bank on a Play, where
// several rows of a state are authored the last one, in one row heading the Inspector; a window that plays none
// heads nothing. Every Play is a play_sound request of the set from that bank (session/sound_play.h).
bool draw_menu_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                         InspectorTaken &taken);

} // namespace opennova::editor
