#pragma once

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
// its fields, its ACTIONs, a SOUND, an ITEM. One row per batch, as every batch.
struct RecordBatch {
	std::vector<Edit> edits;
	// One per edit that makes a record (an add, a duplicate, and each add of a replace_list), in
	// order: the label it gave with `as` ("" for none). The records the batch made are then the
	// request's outcome's `added` (Document::last_added_records()), in the same order.
	std::vector<std::string> made_labels;
};

// What a batch's edits are: changes of records (edit_record), or the fields whose saved value
// comes back (revert_to_saved: each edit {id, field}).
enum class RecordBatchForm { Edits, Fields };

// A batch from its wire form, `edits` a list of one edit or more, each refused by its place
// ("edits[1]: ..."). Edits: {op, id, parent, kind, field, value, position, as, coalesce, gesture,
// list, records}, `op` one of set, clear, write, add, duplicate, remove, move, set_file_value or
// replace_list. `id` names the record every op but add and set_file_value changes (an identity, or
// a label an earlier add or duplicate of the batch gave with `as`); `field` and `value` a set's (a
// clear's and a write's field; an add's field its `value` sets in the same step; a set_file_value's
// file-wide field); `kind` an add's record kind token ("window", "action"); `parent` an add's owner
// (a record, a label, or a row's own identity; none adds a row) and a move's destination;
// `position` an index in the owner's collection (an add goes at the end, a duplicate right after
// its record, a move needs one); `coalesce` a set that folds into the one before on its field
// (typing); `gesture` edits that fold into one undo step until end_edit (a drag); a replace_list's
// `list` (a collection's kind token: "action", "sound", "items.item") of the record `id` replaced
// by `records` ([{field: value, ...}], each added at the end with its fields set in the order
// written). Fields: {id, field}. Strict: an unknown op, member, label, identity or kind is refused
// with the reason, and nothing is read; so are an apply (its change is made in C++ by its document
// type, Edit::payload, S13 D6), a `payload` and a paste. `names` is the record document the
// request acts on; with none, an edit naming a record or a kind is refused. `resolve` false: a
// first read before the document opens (RequestNames::unresolved): a record's identity is read as
// a whole number and not looked for in `names` (a blank of the type, which names kinds alone), a
// replaced list's records and a duplicate's place wait for the read after it opens, and `out` is
// no batch to apply.
bool record_batch_from_json(const io::JsonValue &edits, const Document *names, RecordBatchForm form,
		RecordBatch &out, std::string &error, bool resolve = true);

// The batch form of `edits`, which record_batch_from_json reads back as they are: each record by
// its identity (one an earlier edit of the batch makes by a label, "edit<i>", the edit that makes
// it giving it with `as`), an add's kind by its token in `names` ("" without it), a record added
// straight into a row naming the row's identity as its `parent`. A Paste has no batch form (the
// paste request carries the clipboard): written as op "paste", which the reader refuses; nor
// has an Apply (S13 D6), written as op "apply" with its record and its payload's token, which
// the reader refuses too.
io::JsonValue record_batch_to_json(
		const std::vector<Edit> &edits, const Document *names, RecordBatchForm form);

} // namespace opennova::editor
