#pragma once

#include <base/io/json.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

// What one request came to, on the wire (ADR 0046 S13 A5): {done, unsaved_prompt, operation,
// findings, added} and, for an import that wrote files, `imported` and `not_imported` (S13 A7).
// `done` is false when it was refused or failed (an error among the findings) or waits on the
// unsaved prompt; `operation` names the operation it started or joined (0: none); `added` the
// records its edits made, in order (an edit_record's adds and duplicates, a paste's, a duplicate's
// copies); `imported` the files an import wrote and `not_imported` those it did not reach when it
// stopped part way (each written only when it has any). A header of its own, light, so a client of
// the session reads it without the document and graph headers session_json.h brings (the command
// line's, apps/project).
io::JsonValue action_outcome_to_json(const ActionOutcome &outcome);

} // namespace opennova::editor
