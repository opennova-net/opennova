#pragma once

// A mission's Logic pane, the Event board (ADR 0046 S24, The board): its events as cards beside the 3D view,
// each reading as its sentence; a click on a card selects its event (the session's selection, which the views
// and the Inspector follow); a RevealRecord brings the record's card into sight. The pane is the mission row's
// (ui/document_views' make_pane), drawn while the document view's `logic` is on (ui/main_viewport_view).

#include <memory>

#include <editor/ui/document_views.h>

namespace opennova::editor {

// A new board, the mission row's pane.
std::unique_ptr<DocumentPane> make_mission_board_pane();

} // namespace opennova::editor
