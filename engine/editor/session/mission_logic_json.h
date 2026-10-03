#pragma once

// The wire form of a mission's logic (ADR 0046 S15, Events and scripts): what the `mission_logic`
// query answers (an event's form, a trigger's or an action's form, the types Add trigger and Add
// action offer, the edits an add, a retype, a move, a negation or a join plans in the batch form an
// edit_record takes back) and the `mission_uses` query (the events naming a record, in words).

#include <base/io/json.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_uses.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

class Document;

// {type, sub, title, group, tip}.
io::JsonValue logic_type_to_json(const LogicType &type);
// A page of the types of triggers (`actions` false) or actions offered, in their groups' order, under
// `types`.
io::JsonValue logic_types_to_json(bool actions, const JsonPage &page);
// {record, event, action, index, count, type: {...} | null, type_words, negated, join, last, params:
// [{slot, field, kind, label, value, words, unit, picks}], unread: [...], words, sentence}.
io::JsonValue logic_form_to_json(const LogicForm &form);
// {event, index, when, then, delay, repeat, sentence, repeats, at_start, at_end, delay_steps,
// repeat_steps, delay_seconds, repeat_seconds, most_steps, triggers, actions, most, trigger_refusal,
// action_refusal}.
io::JsonValue logic_event_form_to_json(const LogicEventForm &form);
// {what, inert, count, uses: [{event, event_index, record, action, slot, field, words, sentence}]}.
io::JsonValue mission_uses_to_json(const MissionUses &uses, const JsonPage &page);
// A mission's ParamKind's token ("entity", "zone", "distance_m").
const char *param_kind_token(mission::ParamKind kind);
// A join's token: "and", "or", "or_else"; false for another.
const char *logic_join_token(LogicJoin join);
bool logic_join_from_token(const std::string &token, LogicJoin &out);

} // namespace opennova::editor
