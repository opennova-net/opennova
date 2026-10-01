#pragma once

#include <base/io/json.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

// What one request came to, on the wire (ADR 0046 S13 A5): {done, unsaved_prompt, operation,
// findings, added}. `done` is false when it was refused or failed (an error among the findings) or
// waits on the unsaved prompt; `operation` names the operation it started or joined (0: none), whose
// end, findings and, for an import's write, files are what it came to (the view's last_operation,
// operation_outcome_to_json); `added` the records its edits made, in order (an edit_record's adds
// and duplicates, a paste's, a duplicate's copies). A header of its own, light, so a client of the
// session reads it without the document and graph headers session_json.h brings (the command
// line's, apps/project).
io::JsonValue action_outcome_to_json(const ActionOutcome &outcome);

} // namespace opennova::editor
