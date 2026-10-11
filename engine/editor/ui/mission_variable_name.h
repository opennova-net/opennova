#pragma once

// A variable's name as the board's Variables fold edits it (ADR 0046 S24, Names; documents/mission_variable_names):
// the name the mission gives it, typed and set when Enter is pressed or the field is left, one edit_record (one
// undo); the format's refusal said beside it; how many more variables take a name.

#include <cstdint>

#include <editor/ui/workspace.h>

namespace opennova::editor {

class MissionDocument;

// The name field of variable `index` of `mission` ("" takes its name away). Disabled when not `editable`, and
// where the mission's layer table holds the original editor's layer names, which no name is written over.
void draw_variable_name_field(Workspace &workspace, const MissionDocument &mission, int64_t index, bool editable);
// One line: how many more variables can take a name (31 at most), or why none can.
void draw_variable_names_left(const MissionDocument &mission);

} // namespace opennova::editor
