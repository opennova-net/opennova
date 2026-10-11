#pragma once

// One card of the Event board (ADR 0046 S24, The board): an event drawn as it reads, its title and its flags
// as toggles, its wait and repeat as chips that take seconds and show the steps the game counts, its triggers
// ("When", each joined to the one before) and its actions ("Then") as lines whose words flow with the values as
// chips; the type's own words open the type picker (a retype), a "+" before a line adds one there, "+ trigger"
// and "+ action" add one last; its notes, never gating. Every change is a request (edit_record with
// keep_selection: the card stays selected); the model is documents/mission_board's, the words the sentences'.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <editor/documents/mission_board.h>
#include <editor/graph/display_names.h>
#include <editor/model/document.h>
#include <editor/session/view/workspace_view.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

class MissionDocument;

// What a trigger or an action line carries while it is dragged (S24 A6): its document (the instance) and the
// record. Dropped before a line of a card, or on a card's "+ trigger" or "+ action", it moves there
// (logic_move_edits: one batch, one undo step).
inline constexpr const char *kLogicLineDragPayload = "opennova.logic_line";
struct LogicLineDrag {
	uint64_t document = 0;
	NodeAddress record;
};
static_assert(std::is_trivially_copyable<LogicLineDrag>::value, "Dear ImGui copies a payload by its bytes");

// What a board keeps for its cards between frames: the chip a line's add opens (the line added asked for it,
// shown on the frame its line first draws), the type picker's filter and what it adds where, and the new
// text being typed for a Show text.
struct CardState {
	// A chip to open: the record (an action or a trigger the board just asked to add, by its event and its place
	// in the list once it is there) and its slot; and whether it opens on its New text.
	NodeId open_event = 0;
	bool open_action = false;
	size_t open_index = SIZE_MAX;
	int open_slot = -1;
	bool open_new_text = false;
	// The type picker: its filter, and the event, the list and the place an add goes (SIZE_MAX the end); the
	// line it retypes (none: an add).
	char filter[64] = {};
	NodeId add_event = 0;
	bool add_actions = false;
	size_t add_position = SIZE_MAX;
	NodeAddress retype;
	// The card whose type picker the board's keys asked for (Ctrl+Shift+T, Ctrl+Shift+A), and its list.
	NodeId picker_event = 0;
	bool picker_actions = false;
	// The words of a Show text's New text.
	char new_text[256] = {};
	// A wait typed and not yet set: its field's record and the seconds.
	NodeId waiting_event = 0;
	const char *waiting_field = "";
	double waiting_seconds = 0.0;
	// The card whose Move event... was asked for (its popup opened as it draws).
	NodeId move_event = 0;
	// The picker the chips pick names with (one per board).
	ReferencePicker picker;
};

// What a card draws over: the session's workspace and the mission, the words of its values, whether its
// document takes edits (the busy gate, an import's file), the card selected (its event or a line of it) and the
// pick that waits on a click.
struct CardContext {
	Workspace &workspace;
	const MissionDocument &mission;
	const NameSource *names = nullptr;
	bool editable = true;
	NodeAddress selected;
	WorkspaceView::Pick pick;
	CardState &state;
};

// What the card was asked this frame: its header clicked (its event selected), and the type picker opened
// for an add (the board draws it, `state` saying where).
struct CardAsked {
	bool selected = false;
	bool picker = false;
};

// One card, the full width of the room.
CardAsked draw_mission_card(CardContext &context, const BoardCard &card);
// A script line naming a mission's variable or event by its number (S24 C4: a script's V# and event(n), the graph's
// MissionVariable and MissionEventLatch edges): its file, its place (line:column) and its line.
struct ScriptUse {
	std::string file;
	std::string locator;
	size_t line = 0;
};
// The mission's script's variables (its V#), by number, each with its lines.
std::map<int64_t, std::vector<ScriptUse>> script_variable_uses(const SessionView &view, const MissionDocument &mission);
// The script lines that name the event at `index` by its number (event(n)).
std::vector<ScriptUse> script_event_uses(const SessionView &view, const MissionDocument &mission, size_t index);
// The number a new variable takes: the lowest neither an event nor the mission's script uses.
int64_t new_variable_index(const SessionView &view, const MissionDocument &mission);

// One line of a card alone (S24 A7: the Inspector over a trigger or an action), the card's width: its lead, its
// runs and chips, its retype picker.
void draw_mission_line(CardContext &context, const BoardCard &card, const BoardLine &line);
// The type picker for the add or the retype `state` names, drawn in an open popup's body: Common (the shipped
// missions' most used), for a trigger the recipes' triggers with their presets, then every type by its group,
// filtered by what is typed. True once it raised the add or the retype (the popup then closed).
bool draw_type_picker(CardContext &context, bool actions);

// The edits a type picker's choice makes: an add of `type` (with `presets`, a recipe's) to the event at a place,
// or a retype of a line, raised as one edit_record that keeps the selection. True when raised.
bool raise_logic_add(CardContext &context, NodeId event, const LogicType &type, size_t position,
                     const std::vector<LogicPreset> &presets = {});

} // namespace opennova::editor
