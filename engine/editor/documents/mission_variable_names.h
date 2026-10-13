#pragma once

// A mission's variable names (ADR 0046 S24, Names): what the editor calls V0..V255 in the mission. No .bms byte
// holds them: they ride the .mis beside the mission as its `//var` lines, which both original readers skip
// (formats/mission/mission_mis.h kMisVariableLine; master #1095, D-MIS-13), read with the mission's other names from
// a matching .mis (mission_names_file.h) and held as the document's file-wide values (MissionFileState::variables).
// The names read here and the edit that changes one: a file-wide value, so a name is undone as any edit is.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/edit.h>

namespace opennova::editor {

class MissionDocument;

// V`index` named `name`.
struct MissionVariableName {
	int32_t index = 0;
	std::string name;
};

// The names the mission gives, by their numbers.
std::vector<MissionVariableName> mission_variable_names(const MissionDocument &document);
// The name V`index` has, "" for none.
std::string mission_variable_name(const MissionDocument &document, int64_t index);
// How many variables have no name yet (of V0..V255).
size_t mission_variable_names_left(const MissionDocument &document);
// The edit that gives V`index` the name `name` ("" takes its name away), as a batch an edit_record takes. False with
// `error` where the .mis refuses it (an index past V255, a name longer than a description or holding a character the
// .mis cannot carry: mission::mis_name_allowed); true with no edits where nothing changes.
bool mission_variable_name_edits(const MissionDocument &document, int64_t index, const std::string &name,
                                 std::vector<Edit> &out, std::string &error);

} // namespace opennova::editor
