#pragma once

// The Event board on the wire (ADR 0046 S24, The board): the mission_logic query's `board` op, the cards as the
// Logic pane shows them (documents/mission_board.h), with the types the picker offers first (Common) and the
// recipes a new event is made from (documents/mission_recipes.h).

#include <string>

#include <base/io/json.h>
#include <editor/documents/mission_board.h>
#include <editor/documents/mission_recipes.h>
#include <editor/session/editor_queries.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

// {event, index, title, sentence, when, then, delay, repeat, repeats, at_start, at_end, delay_steps,
// repeat_steps, delay_seconds, repeat_seconds, folded, trigger_refusal, action_refusal, triggers, actions:
// [{record, index, action, type: {type, sub, title, group} | null, type_words, negated, join, last, words,
// runs: [{text, slot}], chips: [{slot, field, label, words, value, kind, unit, picks, unset, missing}]}],
// notes: [{record, code, words}], live?: {active, fired, activate_countdown, repeat_countdown, truth,
// last_checked_tick}}.
io::JsonValue board_card_to_json(const BoardCard &card);
// {token, title, tip, subject, trigger: {type, sub, title} | null, start, events, missions, seen_in}.
io::JsonValue mission_recipe_to_json(const MissionRecipe &recipe);
// The `board` op: the open mission `path` names (the active one left out); its filter, folded events and Needs
// attention the Logic pane's (the workspace's document view) unless the args name them; a page of its cards.
io::JsonValue answer_mission_board(const QueryContext &context, const QueryArgs &args, std::string &error);

} // namespace opennova::editor
