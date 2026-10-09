#pragma once

// A mission's logic in words (ADR 0046 S15, Events and scripts): an event as the sentence a modder
// reads, "When <trigger> and <trigger>, then <action>; <action>.", each trigger and action worded from
// its type and its typed parameters (formats/mission/mission_params.h), the names of what they name
// from a MissionNames (the document's own words, or the project's display names over it). What a
// type means is the game's, from the witness record (docs/mission/bms-event-runtime-re.md 1.2 to 1.5,
// 7.4, 7.5, 8.2): the evaluator's test for a trigger [orig: EventTrigger_EvaluateCondition @0x453620],
// the dispatcher's case for an action [orig: EventAction_Dispatch @0x4542e0], the chain's flat
// left-to-right fold of the joins and the negations [orig: EventTrigger_EvaluateChain @0x454050], the
// event's flags, delay and repeat [orig: EventTrigger_UpdateEntry @0x454c30]. A type no row names
// says so ("a trigger of unknown type 9"), a value naming nothing says so ("SSN 10034 (no entity has
// it)"): never a guess.
//
// The same rows say what each type is called by itself, the group it is offered in and what it does in
// a line (logic_types): what "Add trigger" and "Add action" offer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/mission/bms.h>
#include <formats/mission/mission_chains.h>

namespace opennova::editor {

class MissionDocument;

// The words for what a parameter names. The base words a value by itself ("group 4", "SSN 10034",
// "zone 3", "path 5", "event 3"); a document's names (DocumentMissionNames) and the project's display
// names override them with what the value names there.
class MissionNames {
public:
	virtual ~MissionNames() = default;
	// An entity by its SSN: the player for 10000 [bms-event-runtime-re.md 7.3].
	virtual std::string entity(int64_t ssn) const;
	// An area trigger by its zone id.
	virtual std::string zone(int64_t id) const;
	// A group by its index (0: none).
	virtual std::string group(int64_t group) const;
	// A waypoint path by its number: 0 none, 1..122 a path, 123..127 a command.
	virtual std::string path(int64_t number) const;
	// An event by its index in the event table (0 the first).
	virtual std::string event(int64_t index) const;
	// A text key's string in the shown language, "" where none is known (the base knows none).
	virtual std::string text(const std::string &section, const std::string &key) const;
	// Whether a zone id resolves at the mission's start: an area has it and its box is not flat (the
	// resolver neuters a trigger, and drops an area action, naming one that does not [section 7.3]). The
	// base knows no areas: every id resolves.
	virtual bool zone_resolves(int64_t id) const;
};

// The words a mission document gives what its records name: an entity by the record holding its SSN
// (the first in the lookups' pool order, as the game finds it: MissionDocument::entity_holder), titled
// as the document titles it (its kind and its SSN where the title is the bare number), an area by its
// record, an event by its place; a value no record holds says so. It reads no string table: text() is
// the base's. Made for one state of the document.
class DocumentMissionNames : public MissionNames {
public:
	explicit DocumentMissionNames(const MissionDocument &document) : document_(document) {}
	std::string entity(int64_t ssn) const override;
	std::string zone(int64_t id) const override;
	std::string event(int64_t index) const override;
	bool zone_resolves(int64_t id) const override;

private:
	const MissionDocument &document_;
	mutable bool counted_ = false;
	mutable size_t events_ = 0;
};

// How a trigger's result joins the next trigger's in the chain: the PREVIOUS trigger's flags say
// (bit 1 or, else bit 2 exclusive or, else and) [orig: EventTrigger_EvaluateChain @0x4540b6..0x4540c5].
enum class LogicJoin : uint8_t { And, Or, Xor };
LogicJoin trigger_join(const bms::Trigger &trigger);
// "and", "or", "or else" (exclusive: one of the two, not both).
const char *logic_join_words(LogicJoin join);

// One trigger in words, its negation in them ("Hostage #10034 is not destroyed"). Whether an SSN slot keys
// a relation record the game never writes for it is the engine's (mission::trigger_ssn_unrecorded).
std::string trigger_words(const bms::Trigger &trigger, const MissionNames &names);
// A waypoint list's command (123..127) by what the game does with it [docs/world/world-wac-ai-re.md 3.2,
// 9.1]; null for any other number. Its name in the original editor (dfx2med), for a tooltip, is the
// engine's (mission::path_command_editor_name).
const char *path_command_words(int64_t number);
// One action in words ("show text 4: 'Mission failed'"). `header` gives a sub-goal's text keys, the
// slots' ids (null: no text quoted).
std::string action_words(const bms::Action &action, const MissionNames &names, const bms::Header *header);

// An event in words: when it is checked and what it waits on (`when`: "When A and B", "Right away", "At
// the mission's start, if A"), what it does (`then`: its actions joined by "; ", "nothing" for none),
// how long after its triggers hold it does it (`delay`: "" or "after 5.1 s"; past 512 steps about one
// pass, the countdown wrapping; "never" for a start or end event, checked once), whether it is checked
// again (`repeat`: "" or "Checked again about 11.3 s after its triggers held."; none for a start or end
// event), and the whole of it as one sentence [bms-event-runtime-re.md 1.2, 1.6].
struct EventWords {
	std::string when;
	std::string then;
	std::string delay;
	std::string repeat;
	std::string sentence;
};
EventWords event_words(const mission::EventChain &chain, const MissionNames &names, const bms::Header *header);
inline std::string event_sentence(const mission::EventChain &chain, const MissionNames &names,
                                  const bms::Header *header) {
	return event_words(chain, names, header).sentence;
}

// A delay or a repeat's authored units in seconds: each unit is bms::kEventStepTicks (64) ticks of the
// 62.5 Hz clock, the quarter pass's period [orig: EventTrigger_UpdateEntry @0x454c30 counts the reload
// << 6 down by 64 a pass; docs/mission/bms-event-runtime-re.md 1.6]. Past bms::kEventStepsUnwrapped
// steps the countdown wraps (bms::event_steps_wrap).
double logic_units_seconds(int64_t units);
// What a count of steps comes to in the game: "614.4 s", or past 512 steps "about 1.0 s (past 512 steps
// the game's countdown wraps)".
std::string logic_steps_words(int64_t steps);

// A type of trigger or of action as "Add trigger" and "Add action" offer it: its type and sub-type (a
// trigger's main type and sub-type, an action's type and sub-type), what it is called by itself ("Entity
// is destroyed"), the group it is offered in ("Life and death") and what it does in a line.
struct LogicType {
	bool action = false;
	int32_t type = 0;
	int32_t sub = 0;
	const char *title = "";
	const char *group = "";
	const char *tip = "";
};
// Every trigger type the evaluator has a case for, and every action type the dispatcher has one for
// (and each AI command of the four AI actions), in their groups' order.
const std::vector<LogicType> &logic_types(bool actions);
// The row of a trigger's or an action's type and sub-type (null: none: an unknown type).
const LogicType *logic_type(bool action, int32_t type, int32_t sub);
// An action type's own title whatever its sub-type ("Change group AI"); null for a type the dispatcher has
// no case for.
const char *logic_action_title(int32_t type);
// A team by the colour the round's end names it ("the red team (team 2)"), one name everywhere.
std::string team_words(int64_t team);

} // namespace opennova::editor
