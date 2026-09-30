#pragma once

#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/document.h>
#include <editor/model/edit.h>

namespace opennova::editor {

class ProjectSession;

// Edits named the way the editor MCP names records (ADR 0046 S9m, `editor_menu`): a record
// by its identity, a record kind by its token, and a record the batch itself makes by the
// label its edit gave it (the core's batch_made underneath), so one undo step can add a
// window and fill it in: its fields, its ACTIONs, a SOUND, an ITEM. Any document type; one
// row per batch, as every batch.
struct RecordBatch {
	std::vector<Edit> edits;
	// One per edit that makes a record (an add, a duplicate), in order: the label it gave
	// with `as` ("" for none). The records it made are then the document's
	// last_added_records(), in the same order.
	std::vector<std::string> made_labels;
};

// A batch from [{op, id, field, value, kind, parent, position, as}]: `op` is set, clear,
// write, add, duplicate, remove or move; `id` names the record (an identity, or the label
// an earlier add or duplicate of the batch gave it with `as`); `field` and `value` a set's
// (a clear's and a write's field); `kind` an add's record kind token ("window", "action");
// `parent` an add's owner (a record, a label, or the row itself; none adds a row) and a
// move's destination; `position` an index inside the owner's collection (an add goes at the
// end, a duplicate right after its record, a move needs one). Strict: an unknown op,
// member, label, identity or kind is refused with the reason, and nothing is applied.
bool record_batch_from_json(const io::JsonValue &edits, const Document &document, RecordBatch &out,
                            std::string &error);

// The records of the list `list` (a collection's kind token: "action", "sound",
// "items.item") that `owner` holds, replaced by `records` ([{field: value, ...}], each added
// at the end with its fields set in the order written): one batch.
bool list_batch_from_json(const Document &document, NodeId owner, const std::string &list,
                          const io::JsonValue &records, RecordBatch &out, std::string &error);

// A batch named this way, on one document. `request` is {edits: [...]}
// (record_batch_from_json) or {id, list, records} (list_batch_from_json) for the document at
// `path` ("" = the active one; opened first when it is not open); the editor MCP's
// editor_menu edit and list reach it through menu_edit_request (preview/menu_report.h),
// which names the menu the way its tree and analyze do. The batch goes to the
// session as one EditRecord request: one undo step; a name set is its record's edit alone,
// as any Set is (Rename everywhere, RenameSymbol, renames a name with its uses). Answers
// {ok, error?, outcome (the request's outcome), made {label: identity}, added [the
// identities of the records it made, in order], revision}; ok says the batch parsed (nothing
// is asked of the session when it did not), the outcome's done whether it went through.
io::JsonValue record_batch_request(ProjectSession &session, const std::string &path, const io::JsonValue &request);

} // namespace opennova::editor
