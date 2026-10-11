#pragma once

// A mission record's own items in its outline line's right-click menu (ADR 0046 S24; the outline's row_menu,
// drawn after the jumps): the logic's (ui/mission_logic_view: Show on board, Add trigger, Add action, Move to
// event, Negate) and the views' (lane B: New event, Add to group, Select group members), each lane's over the
// same record.

#include <editor/model/document.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

class MissionDocument;

// The outline's row_menu over a mission's record: the logic's items, then the views'.
void draw_mission_row_menu(Workspace &workspace, const Document &document, const NodeAddress &record);
// The logic's items over a record (ui/mission_logic_view.cpp), and the views' (ui/mission_row_menu.cpp until
// lane B's own file takes them); each draws nothing for a record it has nothing for.
void mission_logic_row_items(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record);
void mission_space_row_items(Workspace &workspace, const MissionDocument &mission, const NodeAddress &record);

} // namespace opennova::editor
