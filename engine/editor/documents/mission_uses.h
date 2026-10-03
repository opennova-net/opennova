#pragma once

// What happens when (ADR 0046 S15, Events and scripts): for a record of a mission (an entity, an area
// trigger, a waypoint path, a group, an event), the events whose triggers or actions name it, each in
// the mission's words, the trigger's or the action's words and its event's sentence, with the field
// that names it (what a Go to selects). What names it is the game's reading of the parameter
// (formats/mission/mission_params.h: an Entity parameter names an SSN, a Zone a zone id, a Path a
// waypoint path's number, a Group a group's index, an Event an event's index), so an SSN or a zone id
// a second record carries is the first holder's alone, as the game's lookups find it [orig:
// EntityPool_FindByNetId @0x4f0a20; EventTrigger_ResolveZoneTriggerRefs @0x453000]: the uses of a
// second holder are none, and its `inert` says why.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_sentence.h>
#include <editor/model/value.h>

namespace opennova::editor {

class MissionDocument;

// One use: the event (its row) and its place among the events, the trigger or the action and whether
// it is an action, the parameter's slot and field, the record's words and its event's sentence.
struct MissionUse {
	NodeAddress event;
	size_t event_index = 0;
	NodeAddress record;
	bool action = false;
	int slot = 0;
	std::string field;
	std::string words;
	std::string sentence;
};

// What names `record`, in the events' order, each record's parameters in their order. `what` is what
// the record is to the events ("Hostage #10034", "Zone 3", "path 5", "group 4", "event 2"; "" for a
// record no parameter can name: a building's item is no event's business); `inert` says why a second
// holder of an SSN or a zone id is named by none ("" otherwise).
struct MissionUses {
	std::string what;
	std::string inert;
	std::vector<MissionUse> uses;
};
MissionUses mission_uses(const MissionDocument &document, const NodeAddress &record, const MissionNames &names);

// The uses of one record kept while the document (its identity, load and revision), the record and the
// names (their source's generation, `names_generation`, 0 for none) stand: the Inspector asks every
// frame a record is selected, which would otherwise word every use's sentence each frame (S15).
class MissionUsesCache {
public:
	const MissionUses &uses(const MissionDocument &document, const NodeAddress &record, const MissionNames &names,
	                        bool has_names, uint64_t names_generation);
	// How many times it worded them (for the tests).
	size_t made() const { return made_; }

private:
	uint64_t identity_ = 0, load_ = 0, revision_ = 0, generation_ = 0;
	bool has_names_ = false, held_ = false;
	NodeAddress record_;
	MissionUses uses_;
	size_t made_ = 0;
};

} // namespace opennova::editor
