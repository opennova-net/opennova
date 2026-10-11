#pragma once

// New events by what they wait on (ADR 0046 S24, The board): a recipe is a title, one of the existing trigger
// types (or none) with preset parameters, and the event flags it starts with; it invents no logic. Each is a
// composition the shipped missions use: a recipe's `events` and `missions` are what the install's 115
// missions hold of it (one trigger of the type, not negated, the same flags; measured and pinned by the
// editor_mission_logic ctest's retail leg), the measurement its citation. The words follow the game's
// reading of the type (documents/mission_sentence.h): an area trigger holds while the entity stands inside
// the area's box, tested each time the chain is checked [docs/mission/bms-event-runtime-re.md 7.4, sub 10], and a
// repeating event fires again while it holds [1.2], so the recipe says "is inside", never "enters".
//
// STAGED, NOT WIRED: the Event board's New event (ui/mission_board_view, its `board` query op) and the views'
// New event menu (lane B's mark menu) are its live owners, landing next in S24.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_logic.h>
#include <editor/model/edit.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

class MissionDocument;

// A parameter a recipe fixes: its slot and value (the player's SSN for "the player").
struct RecipePreset {
	int slot = 0;
	int64_t value = 0;
};

struct MissionRecipe {
	const char *token = "";
	const char *title = "";
	const char *tip = "";
	// What the recipe is made over (Unused: nothing): an area's zone id, an entity's SSN, a group's index or an
	// event's index, given to the trigger's parameter at `subject_slot`.
	mission::ParamKind subject = mission::ParamKind::Unused;
	int subject_slot = -1;
	// The trigger it waits on (none: an event with no trigger, checked as its flags say) and what it fixes.
	bool has_trigger = false;
	int32_t type = 0;
	int32_t sub = 0;
	std::vector<RecipePreset> presets;
	// The event's flags (bms::EventFlags: PreMission for one checked at the mission's start).
	uint32_t flags = 0;
	// What the shipped missions hold of it: the events, and the missions holding them (the first few named).
	size_t events = 0;
	size_t missions = 0;
	const char *seen_in = "";
};

// Every recipe, the most used first.
const std::vector<MissionRecipe> &mission_recipes();
// A recipe by its token; null for none.
const MissionRecipe *mission_recipe(const std::string &token);
// The recipes made over a record of `kind` (Unused: those made over nothing), in their order.
std::vector<const MissionRecipe *> mission_recipes_for(mission::ParamKind kind);
// The trigger type a recipe waits on (null for one with no trigger).
const LogicType *recipe_trigger_type(const MissionRecipe &recipe);
// Whether a shipped event is the recipe's composition: one trigger of its type and sub-type, not negated,
// its own presets held (the player's SSN), and the recipe's flags (ResetAfter aside); or no trigger and the
// flags, for one with none.
bool recipe_matches(const MissionRecipe &recipe, const bms::Event &event, const std::vector<bms::Trigger> &triggers);

// The edits that make a new event of `recipe` at the end of the events (an event's index is its identity:
// one added among them would renumber every later one), one batch, one undo step: the event, its flags,
// and its trigger with the presets, the subject's value (`subject`, a record of the kind the recipe is made
// over: mission_record_value) and every other parameter its type reads at its kind's default. False, with
// why, where the subject is not a record of the recipe's kind.
bool recipe_event_edits(const MissionDocument &document, const MissionRecipe &recipe, const NodeAddress &subject,
                        std::vector<Edit> &out, std::string &error);

} // namespace opennova::editor
