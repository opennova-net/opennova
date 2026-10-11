#pragma once

// A mission's variable names (ADR 0046 S24 "Names"), kept in the .bms layer table: 32 records of 20
// bytes the original editor fills with its layer names and the game reads and drops [orig:
// Mission_LoadBMSFile @0x40f4e0, the layer loop @0x40fc05..0x40fc38 reading each record into a stack
// buffer nothing reads]. A table the editor names variables in is TAGGED: record 0 holds
// kVariableNamesTag, records 1..31 one name each as `V<index> <name>`, NUL-padded to 20 bytes (a full
// 20 with no terminator), the unused records zero. A table that is neither all zero nor tagged holds
// the original editor's layer names, and nothing here ever writes over it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/mission/bms.h>

namespace opennova::mission {

// The tag record: 20 printable characters filling the record with no terminator, spelling the project,
// what the table holds and the layout's version. No layer a designer names reads like it, and no shipped
// mission's table holds a layer name at all (mission_corpus_retail asserts it), so a table whose first
// record is this is the editor's.
inline constexpr char kVariableNamesTag[bms::kLayerRecordSize + 1] = "OPENNOVA-VAR-NAMES-1";
// The names a table holds: one per record after the tag.
inline constexpr size_t kMaxVariableNames = size_t(bms::kLayerRecordCount) - 1;
// The variables a name may be given: V0..V255, the numbered words a script's V# reaches (the compiler caps a
// V# at 255 [orig: WacScript_ResolveParameter @0x4F2AA4..0x4F2B06, "V# too big" @0x4f2ac5]) and the mission's
// variable trigger and action name by the same number. The bank's words past them hold the script's own VAR
// declarations, which the script names itself (runtime/world/var_store.h).
inline constexpr int32_t kVariableNameIndexMax = 255;

struct VariableName {
	int32_t index = 0;
	std::string name;
};

enum class LayerTable {
	Empty,         // every byte zero, as every shipped mission's
	VariableNames, // tagged: the editor's variable names
	LayerNames,    // the original editor's layer names: never written over
};

LayerTable layer_table(const bms::File &file);

// The names a tagged table holds, in record order; none for an empty table or one of layer names. A
// record that does not read as `V<index> <name>` by set_variable_names' rules names nothing, nor does a
// second record of an index (the first stands).
std::vector<VariableName> variable_names(const bms::File &file);

// The longest name variable `index` takes: its record's 20 bytes less `V<index> `.
size_t variable_name_room(int32_t index);

// Writes the names into the layer table, in index order after the tag; an empty list empties the
// table. Refused (false, `error` says why, the file untouched) when the table holds the original
// editor's layer names, past kMaxVariableNames names, for an index outside 0..kVariableNameIndexMax or
// named twice, and for a name that is empty, longer than its room, or holds a character outside
// printable ASCII, a double quote or a ';' (the .mis layer `description` cannot carry them: bms.h
// set_group_name) or a space at either end.
bool set_variable_names(bms::File &file, const std::vector<VariableName> &names, std::string &error);

} // namespace opennova::mission
