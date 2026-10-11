#pragma once

// A mission's queries (ADR 0046 S15, S24): `mission_logic` (its logic without its format, by op) and
// `mission_uses` (what names a record), their params and their handlers, kept apart from the query table's
// other rows (session/editor_queries.cpp, which names these in its rows). The mission_logic query answers
// by its op: one row per op in kMissionLogicOps (mission_queries.cpp), each its token on the wire and its
// answer; the op param's choices are that table's tokens, so a row appended there is an op the query takes.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <base/io/json.h>
#include <editor/documents/mission_sentence.h>
#include <editor/graph/display_names.h>
#include <editor/session/editor_queries.h>
#include <editor/session/query_support.h>

namespace opennova::editor {

class MissionDocument;

// One op of the mission_logic query: its token (the op param's) and what answers it, from the query's args
// as its row's params read them (each op reads the ones its doc names), or null with `error`.
struct MissionLogicOp {
	const char *token = "";
	QueryHandler answer = nullptr;
};

// The op param's index-th token, the ops' table's order (null past the last).
const char *mission_logic_op_choice(size_t index);
// The list param's index-th token: trigger, action.
const char *mission_logic_list_choice(size_t index);
// The join param's index-th token: and, or, or_else.
const char *mission_logic_join_choice(size_t index);

inline constexpr QueryChoices kMissionLogicChoices[] = {
	{ "op", mission_logic_op_choice },
	{ "list", mission_logic_list_choice },
	{ "join", mission_logic_join_choice },
};

// The params every op reads among (each op's doc names its own); an op's own params append here.
inline constexpr QueryParam kMissionLogicParams[] = {
	{ "path", QueryJson::String, false, nullptr,
			"An open mission by its project-relative path or its logical name; left out, the active document." },
	{ "op", QueryJson::String, true, nullptr,
			"What is asked: form (an event's, or a trigger's or an action's: its words and what it holds), types "
			"(what Add trigger or Add action offers, by name and group), add, retype, move, negate, join (the "
			"edits that change the logic so, in the batch form an edit_record takes back), board (the Event board's "
			"cards, S24)." },
	{ "id", QueryJson::Integer, false, nullptr,
			"The record: an event (form, add) or a trigger or an action (form, retype, move, negate, join), by "
			"its identity." },
	{ "list", QueryJson::String, false, nullptr, "types' and add's list: trigger or action." },
	{ "type", QueryJson::Integer, false, nullptr, "add's and retype's type (a trigger's main type, an action's type)." },
	{ "sub", QueryJson::Integer, false, "0", "add's and retype's sub-type (a trigger's, an AI action's command)." },
	{ "to", QueryJson::Integer, false, nullptr, "move's event, by its identity." },
	{ "position", QueryJson::Integer, false, nullptr, "add's and move's place in the list; left out, the end." },
	{ "negated", QueryJson::Boolean, false, nullptr, "negate's: whether the trigger is negated." },
	{ "join", QueryJson::String, false, nullptr, "join's: how the trigger joins the next (and, or, or_else)." },
	{ "filter", QueryJson::String, false, nullptr,
			"board's filter: the cards whose title or sentence holds it, without case; left out, the Logic pane's." },
	{ "folded", QueryJson::Integers, false, nullptr,
			"board's folded events, by their identities; left out, the Logic pane's." },
	{ "attention", QueryJson::Boolean, false, nullptr,
			"board's Needs attention: only the cards with notes; left out, the Logic pane's." },
	{ "offset", QueryJson::Integer, false, "0", kQueryOffsetDoc },
	{ "limit", QueryJson::Integer, false, "100", kQueryLimitDoc },
};

inline constexpr const char *kMissionLogicDoc =
		"A mission's logic without its format (ADR 0046 S15), by op. form: an event's (its sentence and its "
		"parts, when, then, delay, repeat; repeats, at_start, at_end; delay_steps and repeat_steps with their "
		"seconds and most_steps; its triggers' and actions' counts, the most an event holds and why it takes "
		"no more, trigger_refusal and action_refusal), or a trigger's or an action's (its type by title and "
		"group, a trigger's negation, its join to the next and whether it is the last, the params its type "
		"reads, each its slot, field, kind, label, value, the value in words and its unit, those it does not "
		"read that hold a value, its words and its event's sentence). types: what Add trigger or Add action "
		"offers (list), each type, sub, title, group and tip, in their groups' order. add, retype, move, "
		"negate, join: the edits that add a record of a type with its kinds' defaults, give one another type "
		"keeping what still applies, move it (to another event: added there and removed here, one batch), "
		"negate it or join it to the next, in the batch form an edit_record takes back, or the refusal that "
		"says why none (an event holding the most it holds). board (S24): the Event board, events (how many the "
		"mission holds), noted (how many cards have notes), filter and attention as applied, and a page of its "
		"cards in the events' order (count, offset, next_offset): each its event, index, title, sentence and its "
		"parts, flags, wait and repeat in steps and seconds, folded, the Add refusals, its triggers and actions as "
		"lines (record, index, type, type_words, a trigger's negated and join, last, words, runs {text, slot: 0 to 3 "
		"a parameter, 4 the type's own words, -1 a note}, chips {slot, field, label, words, value, kind, unit, "
		"picks, unset, missing}), its notes {record, code, words} (never gating: no action, no trigger, a pick "
		"unset or naming nothing, an area the game drops, a wait past 512 steps, a value its type does not read) "
		"and, while Play runs it, live; with common_triggers and common_actions (what the type picker offers "
		"first, the shipped missions' most used) and recipes (what a new event is made from, each measured over "
		"the shipped missions).";

inline constexpr QueryParam kMissionUsesParams[] = {
	{ "path", QueryJson::String, false, nullptr, "An open mission, as mission_logic takes it." },
	{ "id", QueryJson::Integer, true, nullptr,
			"The record by its identity: an entity, an area trigger, a waypoint path, an event, or a group (a "
			"record of the mission row's Groups)." },
	{ "offset", QueryJson::Integer, false, "0", kQueryOffsetDoc },
	{ "limit", QueryJson::Integer, false, "100", kQueryLimitDoc },
};

inline constexpr const char *kMissionUsesDoc =
		"What happens when (ADR 0046 S15): the events whose triggers or actions name a record of a mission "
		"(an entity by its SSN, an area by its zone id, a waypoint path, an event, a group), each its event "
		"(address and index), the record and the field naming it, the record's words and its event's "
		"sentence; what the record is to them, and why a second holder of an SSN or a zone id is named by "
		"none (inert).";

// The queries' handlers: mission_logic answers by its op's row, mission_uses the uses of a record.
io::JsonValue answer_mission_logic(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_mission_uses(const QueryContext &context, const QueryArgs &args, std::string &error);

// What an op's answer reads (S24's ops share them). The open mission `args` names (`path`, "" the active
// one), or null with `error`.
const MissionDocument *mission_of(const QueryContext &context, const QueryArgs &args, std::string &error);

// A mission's words with the graph's names where it stands (documents/mission_labels.h, S15 Names), for
// one answer.
class MissionWords {
public:
	MissionWords(const QueryContext &context, const MissionDocument &mission);
	MissionWords(const MissionWords &) = delete;
	MissionWords &operator=(const MissionWords &) = delete;
	const MissionNames &operator*() const { return *names_; }

private:
	std::optional<GraphNameSource> source_;
	std::unique_ptr<MissionNames> names_;
};

} // namespace opennova::editor
