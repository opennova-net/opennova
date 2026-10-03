#pragma once

// A mission's logic in the Inspector and the outline (ADR 0046 S15, Events and scripts): what the
// mission's rows of ui/document_views hook in. The data is the portable core's
// (documents/mission_logic, mission_uses, mission_sentence); this draws it.

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The top of the Inspector over a mission's record. An event: its sentence, then its form in words
// (repeats, once at the mission's start, once at its end, the wait before its actions and the repeat's
// wait in steps and seconds, each with its range), its triggers and its actions in words with their
// tools (Up, Down, Move to another event, Remove; a click selects one) and Add trigger / Add action by
// name, each saying why it is off where the event holds the most it holds. A trigger or an action: its
// event's sentence (a click selects the event), its type by name (a picker of the types by group, a
// line each), a trigger's negation and its join to the next in words, and its own words; the
// parameters its type reads stay the generic form's. False, nothing taken, for any other record.
bool draw_mission_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                            InspectorTaken &taken);
// The bottom of the Inspector over an entity, an area trigger, a waypoint path, a group or an event:
// the events that name it (Used by), each its event and the trigger's or action's words, a click
// selecting that record, its event's sentence in its tooltip. Sets taken.own_uses where it drew.
void draw_mission_uses(Workspace &workspace, const Document &document, const NodeAddress &record, InspectorTaken &taken);

// The Inspector's breadcrumb names an event, a trigger and an action by name ("Event 9 / Action 1"):
// the top part shows their words whole.
bool mission_logic_by_name(const Document &document, const NodeAddress &record);

// The outline's "+" of a list whose records are added by type (an event's triggers and actions):
// whether `kind` is one, and the popup's body that offers the types and raises the add.
bool mission_adds_by_menu(NodeKind kind);
void draw_mission_add_menu(Workspace &workspace, const Document &document, const NodeAddress &owner, NodeKind kind);

} // namespace opennova::editor
