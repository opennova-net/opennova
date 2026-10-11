#pragma once

// The right-click menu over a mark of a mission's 3D view or its map (ADR 0046 S24, In the views): what both offer
// over the record the menu was opened on, after the view's own items (the 3D view's Place here, Frame, Duplicate,
// Delete and the entity's jumps; the map's Frame, Duplicate, Delete): Select all like this, Go to in outline, Find
// usages and Show events using this (the 3D view's, moved here), then the logic made from where the record stands:
// New event (the board's recipes made over the record, or over an entity's group: one batch, the Logic pane shown
// on the new card), Add to group (the selected entities' group set: an existing group by its name, or a new one),
// Select group members, and Use in the event the selection held as the menu opened (a trigger or an action of a
// type that takes the record, added to it with the record in its slot). And the outline's line menu's own items for
// a placed record (ui/mission_row_menu.h): Frame, Select group members.

#include <editor/model/value.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

class MissionDocument;

// What the menu was opened over: the mark's record (row 0: nothing under the pointer), the event, trigger or action
// the selection held as it opened (the one Use in adds to; row 0: none), whether the document had a selection.
struct MissionMarkMenu {
	NodeAddress record;
	NodeAddress event;
	bool selected = false;
};
// The event a selection's primary is or belongs to (an event, a trigger or an action of it); row 0 for any other.
NodeAddress mission_menu_event(const NodeAddress &primary);

void draw_mission_mark_items(Workspace &workspace, const MissionDocument &mission, const MissionMarkMenu &menu,
		bool editable);

} // namespace opennova::editor
