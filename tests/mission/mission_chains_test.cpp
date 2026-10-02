// An event's runs split out of the file's tables and joined back (formats/mission/mission_chains.h):
// split then join is the identity on a canonical file (the minted dense mission, byte for byte); a
// file whose runs partition its tables in another order joins canonical and reparses with every
// event holding the records it held; a record shared by two runs, one in no run and a run past its
// table are refused, the event or the record named; an empty run's index is the running offset, at
// the front, in the middle and at the end.
#include <cstdint>
#include <string>
#include <vector>

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_chains.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

namespace bms = opennova::bms;
using namespace opennova::mission;

namespace {

bool load(bms::File &file) {
	const std::vector<uint8_t> bytes =
	        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_dense.bms");
	std::string error;
	return !bytes.empty() && bms::parse(bytes.data(), bytes.size(), file, error);
}

std::vector<uint8_t> written(const bms::File &file) {
	std::vector<uint8_t> bytes;
	std::string error;
	bms::write(file, bytes, error);
	return bytes;
}

// A file of `runs` events, the e-th holding runs[e].first triggers and runs[e].second actions, each
// record marked with its event and its place in the chain (param1), laid out canonically.
bms::File made(const std::vector<std::pair<int, int>> &runs) {
	bms::File file;
	make_default(file);
	std::vector<EventChain> chains;
	for (size_t e = 0; e < runs.size(); ++e) {
		EventChain chain;
		chain.event = {};
		chain.event.delay = int32_t(e);
		for (int i = 0; i < runs[e].first; ++i) {
			bms::Trigger trigger = {};
			trigger.main_type = bms::TriggerMainType::MissionVariable;
			trigger.sub_type = 1;
			trigger.param1 = int32_t(e * 100 + size_t(i));
			chain.triggers.push_back(trigger);
		}
		for (int i = 0; i < runs[e].second; ++i) {
			bms::Action action = {};
			action.action_type = bms::ActionType::MisvarChange;
			action.param1 = int32_t(e * 100 + size_t(i));
			chain.actions.push_back(action);
		}
		chains.push_back(chain);
	}
	join_event_chains(chains, file);
	sync_counts(file);
	return file;
}

// What each event holds: its triggers' and its actions' marks.
std::vector<std::vector<int32_t>> held(const bms::File &file) {
	std::vector<std::vector<int32_t>> out;
	for (const bms::Event &event : file.events) {
		std::vector<int32_t> marks;
		for (int i = 0; i < event.trigger_count; ++i) marks.push_back(file.triggers[size_t(event.trigger_index + i)].param1);
		marks.push_back(-1);
		for (int i = 0; i < event.action_count; ++i) marks.push_back(file.actions[size_t(event.action_index + i)].param1);
		out.push_back(marks);
	}
	return out;
}

int test_identity() {
	bms::File file;
	TEST_EXPECT(load(file));
	TEST_EXPECT(file.events.size() >= 3 && !file.triggers.empty() && !file.actions.empty());
	const std::vector<uint8_t> before = written(file);
	std::vector<EventChain> chains;
	RunReport report;
	TEST_EXPECT(split_event_chains(file, chains, report));
	TEST_EXPECT(report.canonical() && report.splits() && report.event == -1 && report.record == -1);
	TEST_EXPECT(chains.size() == file.events.size());
	size_t triggers = 0, actions = 0;
	for (const EventChain &chain : chains) {
		triggers += chain.triggers.size();
		actions += chain.actions.size();
	}
	TEST_EXPECT(triggers == file.triggers.size() && actions == file.actions.size());
	bms::File joined = file;
	join_event_chains(chains, joined);
	TEST_EXPECT(bms::equal(file, joined) && written(joined) == before);
	return 0;
}

int test_empty_runs() {
	// Empty runs at the front, in the middle and at the end: each carries the running offset.
	const bms::File file = made({{0, 0}, {2, 1}, {0, 3}, {1, 0}, {0, 0}});
	TEST_EXPECT(file.events.size() == 5 && file.triggers.size() == 3 && file.actions.size() == 4);
	const int32_t trigger_at[] = {0, 0, 2, 2, 3}, action_at[] = {0, 0, 1, 4, 4};
	for (size_t e = 0; e < 5; ++e) {
		TEST_EXPECT(file.events[e].trigger_index == trigger_at[e]);
		TEST_EXPECT(file.events[e].action_index == action_at[e]);
	}
	TEST_EXPECT(file.events_count == 5 && file.trigger_count == 3 && file.action_count == 4 && file.header.num_events == 5);
	std::vector<EventChain> chains;
	RunReport report;
	TEST_EXPECT(split_event_chains(file, chains, report) && report.canonical());
	// add_event's empty runs carry it too, and a trigger put into an event in the middle moves the
	// later runs up, the empty ones with them.
	bms::File edited = file;
	MissionEventRecord fresh;
	TEST_EXPECT(add_event(edited, fresh) == 5);
	TEST_EXPECT(edited.events[5].trigger_index == 3 && edited.events[5].action_index == 4);
	std::string error;
	MissionTriggerRecord trigger;
	trigger.param1 = 777;
	TEST_EXPECT(insert_event_trigger(edited, 2, 0, trigger, error));
	const int32_t after[] = {0, 0, 2, 3, 4, 4};
	for (size_t e = 0; e < 6; ++e) TEST_EXPECT(edited.events[e].trigger_index == after[e]);
	TEST_EXPECT(edited.triggers[2].param1 == 777);
	TEST_EXPECT(split_event_chains(edited, chains, report) && report.canonical());
	return 0;
}

int test_reordered() {
	bms::File file = made({{2, 1}, {1, 2}, {3, 0}});
	const std::vector<std::vector<int32_t>> before = held(file);
	// The first two events' trigger runs swapped in the table: the runs still partition it.
	bms::File reordered = file;
	reordered.triggers = {file.triggers[2], file.triggers[0], file.triggers[1], file.triggers[3], file.triggers[4],
	                      file.triggers[5]};
	reordered.events[0].trigger_index = 1;
	reordered.events[1].trigger_index = 0;
	TEST_EXPECT(held(reordered) == before);
	std::vector<EventChain> chains;
	RunReport report;
	TEST_EXPECT(split_event_chains(reordered, chains, report));
	TEST_EXPECT(report.triggers == RunLayout::Reordered && report.actions == RunLayout::Canonical && !report.canonical());
	bms::File joined = reordered;
	join_event_chains(chains, joined);
	TEST_EXPECT(bms::equal(joined, file) && held(joined) == before);
	const std::vector<uint8_t> bytes = written(joined);
	bms::File back;
	std::string error;
	TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), back, error) && held(back) == before);
	// An empty run whose index is not the running offset is reordered too, and nothing else.
	bms::File stray = made({{1, 1}, {0, 0}, {1, 1}});
	stray.events[1].trigger_index = 0;
	TEST_EXPECT(split_event_chains(stray, chains, report) && report.triggers == RunLayout::Reordered &&
	            report.actions == RunLayout::Canonical);
	// An empty run's index past its table is never read: reordered, not out of range.
	stray.events[1].action_index = 999;
	TEST_EXPECT(split_event_chains(stray, chains, report) && report.actions == RunLayout::Reordered);
	return 0;
}

int test_refused() {
	std::vector<EventChain> chains;
	RunReport report;
	// A trigger in two runs.
	bms::File shared = made({{2, 1}, {2, 1}});
	shared.events[1].trigger_index = 1;
	TEST_EXPECT(!split_event_chains(shared, chains, report) && chains.empty());
	TEST_EXPECT(report.triggers == RunLayout::Shared && report.event == 1 && report.record == 1 && !report.splits());
	// An action in no run.
	bms::File unowned = made({{1, 2}, {1, 1}});
	unowned.events[0].action_count = 1;
	TEST_EXPECT(!split_event_chains(unowned, chains, report));
	TEST_EXPECT(report.triggers == RunLayout::Canonical && report.actions == RunLayout::Unowned && report.record == 1 &&
	            report.event == -1);
	// A run past its table, and one before it.
	bms::File past = made({{1, 1}, {2, 1}});
	past.events[1].trigger_count = 3;
	TEST_EXPECT(!split_event_chains(past, chains, report) && report.triggers == RunLayout::OutOfRange && report.event == 1);
	past = made({{1, 1}, {2, 1}});
	past.events[0].action_index = -1;
	TEST_EXPECT(!split_event_chains(past, chains, report) && report.actions == RunLayout::OutOfRange && report.event == 0);
	// The chain edits refuse such a file and leave it as it was.
	std::string error;
	MissionTriggerRecord trigger;
	const bms::File kept = shared;
	TEST_EXPECT(!insert_event_trigger(shared, 0, 0, trigger, error) && !error.empty());
	TEST_EXPECT(!remove_event(shared, 0, error) && bms::equal(shared, kept));
	return 0;
}

int test_event_references() {
	// remove_event renumbers what names an event by its index: an Event trigger and a ResetEvent
	// action, each naming an event after the hole one less, each naming the hole -1.
	bms::File file = made({{1, 1}, {1, 1}, {1, 1}, {1, 1}});
	for (size_t e = 0; e < 4; ++e) {
		bms::Trigger &trigger = file.triggers[size_t(file.events[e].trigger_index)];
		trigger.main_type = bms::TriggerMainType::Event;
		trigger.sub_type = 0;
		trigger.param1 = int32_t(3 - e); // 3, 2, 1, 0
		bms::Action &action = file.actions[size_t(file.events[e].action_index)];
		action.action_type = bms::ActionType::ResetEvent;
		action.param1 = int32_t((e + 1) % 4); // 1, 2, 3, 0
	}
	std::string error;
	TEST_EXPECT(remove_event(file, 1, error) && file.events.size() == 3);
	// The events left were 0, 2 and 3.
	TEST_EXPECT(file.triggers[0].param1 == 2 && file.triggers[1].param1 == -1 && file.triggers[2].param1 == 0);
	TEST_EXPECT(file.actions[0].param1 == -1 && file.actions[1].param1 == 2 && file.actions[2].param1 == 0);
	RunReport report;
	std::vector<EventChain> chains;
	TEST_EXPECT(split_event_chains(file, chains, report) && report.canonical());
	return 0;
}

} // namespace

int main() {
	if (test_identity() != 0) return 1;
	if (test_empty_runs() != 0) return 1;
	if (test_reordered() != 0) return 1;
	if (test_refused() != 0) return 1;
	return test_event_references();
}
