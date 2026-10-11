#pragma once

// A mission's variables as the board's Variables fold lists them (ADR 0046 S24, The board): each variable its
// triggers and actions read or change, by its number (a MissionVar parameter: formats/mission/mission_params.h),
// with its name (MissionNames::variable: "V12", or the name the mission gives it) and where it is used; and the
// number a new variable takes. The script's V# lines join the uses once the script's references are read (S24 C4).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_sentence.h>
#include <editor/model/value.h>

namespace opennova::editor {

class MissionDocument;

// One use: the event (its row and its place among the events), the trigger or the action, its parameter's slot,
// and the record's words.
struct MissionVariableUse {
	NodeAddress event;
	size_t event_index = 0;
	NodeAddress record;
	bool action = false;
	int slot = 0;
	std::string words;
};

struct MissionVariable {
	int64_t index = 0;
	std::string name;
	std::vector<MissionVariableUse> uses;
};

// The variables the events use, by number, each with its uses in the events' order.
std::vector<MissionVariable> mission_variables(const MissionDocument &document, const MissionNames &names);
// The number a new variable takes: the lowest no trigger or action uses, from 0.
int64_t mission_new_variable(const MissionDocument &document);

} // namespace opennova::editor
