#include <formats/mission/mission_chains.h>

// An event's runs split out of the file's tables and joined back (mission_chains.h). The loader fixes
// each run's first index up into a pointer and the walks read its count byte signed [orig:
// EventTrigger_LoadAllData @0x453eb0; triggers_read, actions_read]; an empty run's index is never
// dereferenced [orig: EventTrigger_EvaluateChain @0x454050, zero triggers reads TRUE], so only a run the
// game reads past its table, a record two runs share or one no run holds is refused here.

#include <cstddef>

namespace opennova::mission {

namespace {

// One table against the events' runs of it. `first` and `count` read an event's run: its first index and
// the records the game reads of it.
template <class First, class Count>
RunLayout layout_of(const std::vector<bms::Event> &events, size_t table, First first, Count count, int &event,
                    int &record) {
	std::vector<uint8_t> owned(table, 0);
	RunLayout layout = RunLayout::Canonical;
	size_t offset = 0;
	int shared_event = -1, shared_record = -1;
	for (size_t e = 0; e < events.size(); ++e) {
		const int64_t at = first(events[e]);
		const size_t held = count(events[e]);
		if (held > 0 && (at < 0 || size_t(at) > table || held > table - size_t(at))) {
			event = int(e);
			return RunLayout::OutOfRange;
		}
		if (at != int64_t(offset)) layout = RunLayout::Reordered;
		for (size_t i = 0; i < held; ++i) {
			uint8_t &mark = owned[size_t(at) + i];
			if (mark && shared_record < 0) {
				shared_event = int(e);
				shared_record = int(size_t(at) + i);
			}
			mark = 1;
		}
		offset += held;
	}
	if (shared_record >= 0) {
		event = shared_event;
		record = shared_record;
		return RunLayout::Shared;
	}
	for (size_t i = 0; i < table; ++i)
		if (!owned[i]) {
			record = int(i);
			return RunLayout::Unowned;
		}
	return layout;
}

bool splits(RunLayout layout) { return layout == RunLayout::Canonical || layout == RunLayout::Reordered; }

} // namespace

bool split_event_chains(const bms::File &file, std::vector<EventChain> &out, RunReport &report) {
	out.clear();
	report = RunReport();
	int event = -1, record = -1;
	report.triggers = layout_of(
	        file.events, file.triggers.size(), [](const bms::Event &e) { return int64_t(e.trigger_index); },
	        [](const bms::Event &e) { return triggers_read(e.trigger_count); }, event, record);
	int action_event = -1, action_record = -1;
	report.actions = layout_of(
	        file.events, file.actions.size(), [](const bms::Event &e) { return int64_t(e.action_index); },
	        [](const bms::Event &e) { return actions_read(e.action_count); }, action_event, action_record);
	if (splits(report.triggers) && !splits(report.actions)) {
		event = action_event;
		record = action_record;
	}
	report.event = event;
	report.record = record;
	if (!report.splits()) return false;
	out.reserve(file.events.size());
	for (const bms::Event &source : file.events) {
		EventChain chain;
		chain.event = source;
		const size_t triggers = triggers_read(source.trigger_count), actions = actions_read(source.action_count);
		if (triggers)
			chain.triggers.assign(file.triggers.begin() + source.trigger_index,
			                      file.triggers.begin() + source.trigger_index + std::ptrdiff_t(triggers));
		if (actions)
			chain.actions.assign(file.actions.begin() + source.action_index,
			                     file.actions.begin() + source.action_index + std::ptrdiff_t(actions));
		out.push_back(std::move(chain));
	}
	return true;
}

void join_event_chains(const std::vector<EventChain> &chains, bms::File &file) {
	file.events.clear();
	file.triggers.clear();
	file.actions.clear();
	file.events.reserve(chains.size());
	// The count byte as the chain's event holds it where it is 128..255 and the chain holds what the game reads
	// of it (mission_chains.h), else the chain's size.
	const auto count_byte = [](uint8_t held, size_t size, size_t (*read)(uint8_t)) {
		return held >= 128 && size == read(held) ? held : static_cast<uint8_t>(size);
	};
	for (const EventChain &chain : chains) {
		bms::Event event = chain.event;
		event.trigger_index = static_cast<int32_t>(file.triggers.size());
		event.trigger_count = count_byte(chain.event.trigger_count, chain.triggers.size(), triggers_read);
		event.action_index = static_cast<int32_t>(file.actions.size());
		event.action_count = count_byte(chain.event.action_count, chain.actions.size(), actions_read);
		file.events.push_back(event);
		file.triggers.insert(file.triggers.end(), chain.triggers.begin(), chain.triggers.end());
		file.actions.insert(file.actions.end(), chain.actions.begin(), chain.actions.end());
	}
	file.header.num_events = static_cast<uint32_t>(file.events.size());
	file.events_count = static_cast<int32_t>(file.events.size());
	file.trigger_count = static_cast<int32_t>(file.triggers.size());
	file.action_count = static_cast<int32_t>(file.actions.size());
}

} // namespace opennova::mission
