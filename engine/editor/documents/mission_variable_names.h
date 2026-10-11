#pragma once

// A mission's variable names (ADR 0046 S24, Names): what the editor calls V0..V255 in the mission, kept in the
// layer table the game reads and drops, tagged as the editor's (formats/mission/mission_var_names.h). The names
// read here and the edits that change them: Set edits of the mission row's Layer records, one batch, so a name
// is undone as any edit is. A table of the original editor's layer names is never written over.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <formats/mission/mission_var_names.h>

namespace opennova::editor {

class MissionDocument;

// The names the mission gives, in the table's order; none where it holds none (an empty table, the
// original editor's layer names).
std::vector<mission::VariableName> mission_variable_names(const MissionDocument &document);
// The name V`index` has, "" for none.
std::string mission_variable_name(const MissionDocument &document, int64_t index);
// What the table is: empty (as every shipped mission's), the editor's names, or the original editor's layer
// names, which no name is written over.
mission::LayerTable mission_variable_table(const MissionDocument &document);
// How many more variables a name may be given (mission::kMaxVariableNames at most), 0 where the table holds
// the original editor's layer names.
size_t mission_variable_names_left(const MissionDocument &document);
// The edits that give V`index` the name `name` ("" takes its name away), as a batch an edit_record takes:
// a Set of each Layer record whose text changes. False with `error` where the format refuses (the table
// holds layer names, a 32nd name, an index past V255, a name too long for its record or holding a character
// the .mis cannot carry); true with no edits where nothing changes.
bool mission_variable_name_edits(const MissionDocument &document, int64_t index, const std::string &name,
                                 std::vector<Edit> &out, std::string &error);
// Whether `after`'s layer table is one a tagged table `before` may become: the same, emptied, or the names
// set_variable_names writes for the names it reads (a record edited as a layer name, the tag among them, is
// not: it would leave names the editor cannot read, or untag the table for good).
bool mission_variable_table_kept(const bms::File &before, const bms::File &after);

} // namespace opennova::editor
