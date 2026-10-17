#pragma once

// A mission's events with the triggers and actions each owns. The file holds three tables (events,
// triggers, actions), an event naming a run of each of the other two by its first record's index and a
// count [orig: EventTrigger_LoadAllData @0x453eb0 fixes each index up into a pointer]. Every shipped
// mission lays the two tables out as its events' runs in event order, no record shared between two
// events and none outside every run, an empty run carrying the running offset [corpus: 115 of 115
// shipped missions, 742 empty trigger runs and 520 empty action runs]. So an event's triggers and
// actions are records the event holds, and its two first indexes and two counts are what the writer
// derives.
//
// What the original editor writes for an edited mission is not witnessed (Med_WriteBmsFile @0x44f920,
// D-MIS-3); the layout join_event_chains writes is the corpus's. An empty run's index is never
// dereferenced [orig: EventTrigger_EvaluateChain @0x454050: zero triggers is TRUE], so a file whose
// runs partition its tables in another order (Reordered) loses nothing but its byte layout when it is
// joined.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <formats/mission/bms.h>

namespace opennova::mission {

// The records of an event's run the game reads, from the run's count byte, which every walk reads signed.
// The trigger walk evaluates the first record whenever the byte is not zero and the next ones while their
// index is below the signed count, so 128..255 evaluates the first alone [orig: EventTrigger_EvaluateChain
// @0x454050, the first @0x45405f, the rest @0x45408f/@0x4540d3]; the action walks dispatch while the index
// is below the signed count, so 128..255 dispatches none [orig: EventTrigger_UpdateEntry @0x454c30, @0x454c95
// and @0x454d04].
inline size_t triggers_read(uint8_t count) { return count < 128 ? count : 1; }
inline size_t actions_read(uint8_t count) { return count < 128 ? count : 0; }

// An event and the records it owns, the run the game reads of each table (triggers_read, actions_read). The
// event's trigger_index and action_index are not read from here, nor its two counts but a count byte of
// 128..255 (join_event_chains): join_event_chains derives them.
struct EventChain {
	bms::Event event;
	std::vector<bms::Trigger> triggers;
	std::vector<bms::Action> actions;
};

// How a file lays a table out against its events' runs, worst first where two hold:
//   OutOfRange  a run of one record or more reaches past its table;
//   Shared      a record lies in two events' runs;
//   Unowned     a record lies in no event's run;
//   Reordered   the runs partition the table, but not as the events stand (a run out of event order,
//               or an empty run whose index is not the running offset);
//   Canonical   each run where its event stands, the layout join_event_chains writes.
enum class RunLayout { Canonical, Reordered, Shared, Unowned, OutOfRange };

// What split_event_chains found, a table at a time. `event` is the first event whose run is out of
// range or holds a shared record (the later of the two events sharing it), -1 otherwise; `record` the
// first shared or unowned record's index in its table, -1 otherwise; the triggers' where both tables
// are at fault.
struct RunReport {
	RunLayout triggers = RunLayout::Canonical;
	RunLayout actions = RunLayout::Canonical;
	int event = -1;
	int record = -1;
	// Whether the chains hold every record once: the file can be split.
	bool splits() const {
		return (triggers == RunLayout::Canonical || triggers == RunLayout::Reordered) &&
		       (actions == RunLayout::Canonical || actions == RunLayout::Reordered);
	}
	bool canonical() const { return triggers == RunLayout::Canonical && actions == RunLayout::Canonical; }
};

// The file's events with their runs. False, with `out` empty, where a table is Shared, Unowned or
// OutOfRange (report says which and where): the chains could not hold what the file holds.
bool split_event_chains(const bms::File &file, std::vector<EventChain> &out, RunReport &report);

// The three tables from the chains: each run where its event stands, its first index the running
// offset (an empty run's too), its count the list's size; the file's three counts and the header's
// event count with them. [corpus: 115 of 115 shipped missions] A count byte of 128..255 is written as
// the chain's event holds it while the chain holds what the game reads of it (the first trigger, no
// action): the size would read otherwise (at a trigger count of 1 the zone resolver walks the trigger,
// rewriting a zone trigger's zone, which at 128..255 it never does [orig: EventTrigger_ResolveZoneTriggerRefs
// @0x453000, the signed test @0x453029]).
void join_event_chains(const std::vector<EventChain> &chains, bms::File &file);

// The most triggers, and the most actions, an event's chain is given by an edit. The counts are bytes;
// the shipped missions hold 16 triggers and 20 actions at most [corpus], and the original editor's
// ceiling is not witnessed (D-MIS-3): an edit past what a shipped mission holds is refused.
inline constexpr size_t kMaxEventChainEntries = 20;

} // namespace opennova::mission
