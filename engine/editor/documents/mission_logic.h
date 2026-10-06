#pragma once

// Adding and editing a mission's logic without knowing the format (ADR 0046 S15, Events and scripts):
// what a trigger's or an action's form shows (its type by name, its negation and its join, the
// parameters its type reads, each its kind, its plain label, its value and that value named), what an
// event's form shows (its flags, its delay and its repeat in words, with their range), and the edits
// that add a trigger or an action of a type with the defaults its kinds take, give a record another
// type keeping what still applies, and move one to another event, each one batch (one undo step) the
// windows raise and the editor MCP passes back (the `mission_logic` query answers them in the batch
// form). What refuses an add (the 20 records an event holds) says so before the edit. The meanings are
// mission_params' and mission_sentence's.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_sentence.h>
#include <editor/model/edit.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

class MissionDocument;

// What a new record's parameter of a kind holds: the value the shipped records of the kind hold most
// often where the kind is a quantity or a choice (a count, a distance, seconds, a speed, on or off, a
// team, a sub-goal: measured over the install's 115 missions, the `mission_logic` ctest's retail leg
// pinning each), 0 for an index or a record named (none named yet: the form asks for a pick).
int64_t logic_param_default(mission::ParamKind kind);
// The unit a kind's number is in ("m", "s", "km/h"; "" for none).
const char *logic_param_unit(mission::ParamKind kind);
// Whether a kind's value is picked by name (an entity, an area, a group, a waypoint path, an event).
bool logic_param_picks(mission::ParamKind kind);

// A parameter as its record's form shows it: its slot (0 for param1) and field, its kind, its label in
// plain words, its value, the value named ("Hostage #10034", "zone 3, which no area has", "on") and
// its unit.
struct LogicParam {
	int slot = 0;
	std::string field;
	mission::ParamKind kind = mission::ParamKind::Raw;
	std::string label;
	int64_t value = 0;
	std::string words;
	const char *unit = "";
	bool picks = false;
};

// A trigger's or an action's form: the record and its event, its place in its event's list, its type
// (null for a type no row names, `type_words` saying which), a trigger's negation and its join to the
// next (`last`: none follows, so the join is read by nothing), the parameters its type reads, those it
// does not read that hold a value other than the -1 every shipped record holds there
// (mission::kUnreadParam; shown apart, never lost), its words and its event's sentence.
struct LogicForm {
	bool action = false;
	NodeAddress record;
	NodeAddress event;
	size_t index = 0, count = 0;
	const LogicType *type = nullptr;
	std::string type_words;
	bool negated = false;
	LogicJoin join = LogicJoin::And;
	bool last = false;
	std::vector<LogicParam> params;
	std::vector<LogicParam> unread;
	std::string words;
	std::string sentence;
};
// False for a record that is no trigger or action of the document.
bool logic_form(const MissionDocument &document, const NodeAddress &record, const MissionNames &names, LogicForm &out);

// An event's form: its sentence and its parts, its flags in words (repeats, at the mission's start, at
// its end), its delay and its repeat in steps of 64 ticks and in seconds, the most a step count holds
// (1023: ten bits on disk), its triggers' and actions' counts and the most an event holds of each, and
// why it takes no more of either ("" where it does).
struct LogicEventForm {
	NodeAddress event;
	size_t index = 0;
	EventWords words;
	bool repeats = false, at_start = false, at_end = false;
	int64_t delay = 0, repeat = 0;
	int64_t most_steps = 1023;
	size_t triggers = 0, actions = 0, most = 20;
	std::string trigger_refusal, action_refusal;
};
bool logic_event_form(const MissionDocument &document, NodeId event, const MissionNames &names, LogicEventForm &out);

// Why an event takes no more triggers (`actions` false) or actions now ("" where it does): it holds the
// most an event's list holds (mission_chains' kMaxEventChainEntries; the shipped missions hold 16
// triggers and 20 actions at most).
std::string logic_add_refusal(const MissionDocument &document, NodeId event, bool actions);

// The edits that add a record of `type` to `event`'s list at `position` (SIZE_MAX: the end): an Add
// with its type (the new record holds -1 in each parameter, mission::kUnreadParam, as every shipped
// record holds a parameter its type does not read), then its sub-type and each parameter its type reads
// set to its kind's default, every edit after the Add naming what it made (batch_made). False, with why, where the event takes no more
// or is no event of the document.
bool logic_add_edits(const MissionDocument &document, NodeId event, const LogicType &type, size_t position,
                     std::vector<Edit> &out, std::string &error);
// The edits that give a trigger or an action another type: its type and sub-type set, each parameter
// whose kind the new type reads in the same slot kept, every other one the new type reads set to its
// kind's default, and one the new type does not read set to -1, as every shipped record holds one
// (mission::kUnreadParam; the file then holds no stale number). A trigger keeps its negation and its join. None where it is that type already.
bool logic_retype_edits(const MissionDocument &document, const NodeAddress &record, const LogicType &type,
                        std::vector<Edit> &out, std::string &error);
// The edits that move a trigger or an action to `to_event`'s list at `position` (SIZE_MAX: the end):
// within its own event a Move; to another event one batch that adds it there with every field it holds
// and removes it from its own (one undo step). False, with why, where the other event takes no more.
bool logic_move_edits(const MissionDocument &document, const NodeAddress &record, NodeId to_event, size_t position,
                      std::vector<Edit> &out, std::string &error);
// The Set of a trigger's condition flags that negates it or not, and the one that joins it to the next
// by `join`, each keeping the flag word's other bits.
bool logic_negate_edit(const MissionDocument &document, const NodeAddress &trigger, bool negated, Edit &out);
bool logic_join_edit(const MissionDocument &document, const NodeAddress &trigger, LogicJoin join, Edit &out);

} // namespace opennova::editor
