#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/document.h>
#include <editor/model/edit.h>

namespace opennova::editor {

// A request's edits in their wire form (ADR 0046 S9m, S13 A5): the batch form every client names
// records by. A record by its identity, or by the label (`as`) an earlier add or duplicate of the
// batch gave it (the core's batch_made underneath), and a record kind by its token, each in the
// record document the request acts on (`names`), so one undo step can add a window and fill it in:
// its fields, its ACTIONs, a SOUND, an ITEM. A batch is over any rows of the document, as every
// batch is (S13 D7): records, rows and file-wide values together, a row an add or a duplicate
// makes named by its label as a record is.
struct RecordBatch {
	std::vector<Edit> edits;
	// One per edit, by its index: the label it gave with `as` ("" for none, and for every edit but
	// an add or a duplicate). The request's outcome names what each label's edit made
	// (Document::last_made()).
	std::vector<std::string> labels;
};

// What a batch's edits are: changes of records (edit_record), the fields whose saved value comes
// back (revert_to_saved: each edit {id, field}), or a text document's spans replaced (edit_record
// over one, S13 D9: each edit {op: apply, payload: "text.span", line, column, length, text}).
enum class RecordBatchForm { Edits, Fields, Spans, kCount };

inline constexpr size_t kRecordBatchFormCount = static_cast<size_t>(RecordBatchForm::kCount);

// The batch form's vocabulary, one table (ADR 0046 S13 A5, S13 D9): every op an edit names, the
// form it is read in and what it does; every member an edit takes, its JSON type, the forms that
// read it and what it carries. The readers (record_batch_from_json) take their ops and members from
// these rows, anything else refused as unknown, and editor_query catalog writes them as its `batch`
// section, from which the editor MCP makes its edit schema (editor_mcp_catalog.gd).
enum class BatchJson : uint8_t {
	String,  // a string
	Integer, // a whole number, BatchMember::minimum or more
	Id,      // a record: its identity (a whole number) or a label an earlier edit gave (a string)
	Boolean, // true or false
	Value,   // a field's value: a number, a string or a bool
	Records, // a list of records, each {field: value}
};

struct BatchOp {
	const char *token;
	RecordBatchForm form;
	const char *doc;
	// Read in the Spans form too, by its op alone (a change every document takes: the line-ends fix,
	// restore_line_ends); `form` is where the catalog lists it.
	bool every_form = false;
};

struct BatchMember {
	const char *name;
	BatchJson json;
	uint8_t forms;     // the forms that read it, by batch_form_bit
	int64_t minimum;   // an Integer's least value (-1 for none)
	const char *only;  // the one string it takes ("" for any): an apply's payload token
	const char *doc;
};

constexpr uint8_t batch_form_bit(RecordBatchForm form) {
	return static_cast<uint8_t>(1u << static_cast<unsigned>(form));
}

template <typename Row> struct BatchRows {
	const Row *rows = nullptr;
	size_t count = 0;
	const Row *begin() const { return rows; }
	const Row *end() const { return rows + count; }
};

// The ops, in the order the forms list them; the members, in the order an edit's schema lists them.
BatchRows<BatchOp> batch_ops();
BatchRows<BatchMember> batch_members();
// A form's token ("edits", "fields", "spans") and what an edit of it is; a JSON type's word
// ("string", "integer", "id", "boolean", "value", "records").
const char *batch_form_token(RecordBatchForm form);
const char *batch_form_doc(RecordBatchForm form);
const char *batch_json_token(BatchJson json);

// A batch from its wire form, `edits` a list of one edit or more, each refused by its place
// ("edits[1]: ..."). Edits: {op, id, parent, kind, field, value, position, as, coalesce, gesture,
// list, records}, `op` one of set, clear, write, add, duplicate, remove, move, set_file_value or
// replace_list. `id` names the record every op but add and set_file_value changes (an identity, or
// a label an earlier add or duplicate of the batch gave with `as`); `field` and `value` a set's (a
// clear's and a write's field; an add's field its `value` sets in the same step; a set_file_value's
// file-wide field); `kind` an add's record kind token ("window", "action"); `parent` an add's owner
// (a record, a label, or a row's own identity; none adds a row) and a move's destination;
// `position` an index in the owner's collection (an add goes at the end, a duplicate right after
// its record as the edits before it left it, a move needs one); `coalesce` a set that folds into
// the one before on its field
// (typing); `gesture` edits that fold into one undo step until end_edit (a drag); a replace_list's
// `list` (a collection's kind token: "action", "sound", "items.item") of the record `id` replaced
// by `records` ([{field: value, ...}], each added at the end with its fields set in the order
// written). Fields: {id, field}. Spans (a text document's, S13 D9): {op: "apply", payload:
// "text.span", line, column, length, text, coalesce, gesture}: the `length` characters from `line`
// and `column` (1-based; a line end counts its own) replaced by `text`, which is UTF-8 and stored in
// the game's code page (Windows-1252: a character it has no byte for is refused); `length` 0 and
// `text` "" by default; the spans of a batch each against the text as the ones before left it
// (TextDocument::replace). Strict: an unknown op, member, label, identity or kind is refused with
// the reason, and nothing is read; so are, over records, an apply (its change is made in C++ by its
// document type, Edit::payload, S13 D6), a `payload` and a paste. `names` is the record document the
// request acts on; with none, an edit naming a record or a kind is refused. `resolve` false: a
// first read before the document opens (RequestNames::unresolved): a record's identity is read as
// a whole number and not looked for in `names` (a blank of the type, which names kinds alone), a
// replaced list's records wait for the read after it opens, and `out` is no batch to apply.
bool record_batch_from_json(const io::JsonValue &edits, const Document *names, RecordBatchForm form,
		RecordBatch &out, std::string &error, bool resolve = true);

// The batch form of `edits`, which record_batch_from_json reads back as they are: each record by
// its identity (one an earlier edit of the batch makes by a label, "edit<i>", the edit that makes
// it giving it with `as`), an add's kind by its token in `names` ("" without it), a record added
// straight into a row naming the row's identity as its `parent`. A Paste has no batch form (the
// paste request carries the clipboard): written as op "paste", which the reader refuses; nor
// has an Apply (S13 D6), written as op "apply" with its record and its payload's token, which
// the reader refuses too, but a text document's span replaced (TextSpanEdit, S13 D9), written as
// the Spans form reads it.
io::JsonValue record_batch_to_json(
		const std::vector<Edit> &edits, const Document *names, RecordBatchForm form);

} // namespace opennova::editor
