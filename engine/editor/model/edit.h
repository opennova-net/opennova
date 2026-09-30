#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/model/value.h>

namespace opennova::editor {

// What the windows and the shell ask a document to change: one typed operation on one
// address, or a batch of them on one row (DocumentBase::apply; the record document's,
// Document::apply). A document applies either as a single undoable change.
enum class EditOperation {
	Set,          // field := value at address
	Clear,        // an optional field left out of the file (its latent value kept)
	Write,        // an optional field the file leaves out, written again with the value it
	              // reads (Clear's inverse: a Set of that same value leaves it out)
	Add,          // a new record of address.kind: a row (address.row = 0, parent = 0) or a record
	              // in the owner `parent` (0 = the row address.row); with `field` named, the new
	              // record's field := value in the same step (a window added with its type)
	Duplicate,    // the addressed record, inserted at position in its owner's collection
	Remove,       // the addressed record
	Move,         // the addressed record to position in the collection of `parent` (0 = its own owner)
	Paste,        // value = a payload Document::copy made: its records into the owner `parent`
	              // (0 = the row address.row) at position
	SetFileValue, // a file-wide value (document type specific): field / position / value
	Apply,        // payload: a change the document's type made in C++, applied to address (a record
	              // type through Document::apply_payload; another kind of document takes its own)
};

// The change an Apply edit carries (ADR 0046 S13 D6): made in C++ by the document type that takes
// it (a raster's brush stroke, a text's span replaced, a record type's own change) and immutable
// once made, so whatever keeps it (a batch, a history of payloads) shares it. Its token names its
// kind: a type takes the payloads it makes and refuses another's. The editor's JSON writes an
// edit's payload as its token and reads none back: the editor MCP cannot send an Apply edit yet.
struct EditPayload {
	virtual ~EditPayload();
	virtual const char *token() const = 0;
};

struct Edit {
	EditOperation operation = EditOperation::Set;
	NodeAddress address; // the record the edit is about. Add / Paste: {row (0 = top level), kind, 0}
	NodeId parent = 0;   // Add / Paste: the owner (0 = the row). Move: the destination owner (0 = unchanged)
	std::string field;   // Set / Clear / Write: the field; Add: the new record's field `value` sets ("" = none)
	Value value = int64_t(0);
	// Add / Duplicate / Move / Paste: an index inside the owner's collection of the record's
	// kind (SIZE_MAX = the end).
	size_t position = SIZE_MAX;
	// Consecutive Sets of the same field fold into one undo step, each applied to the record
	// as the group's first Set found it (the step is that record plus the latest value).
	bool coalesce = false;
	// Sets, Clears, Writes and edits inside a row carrying the same nonzero gesture (a drag) fold into
	// one undo step on that row until the edit group ends (EndEdit); each applies to the
	// record as the previous one left it. next_edit_gesture() hands out a fresh one.
	uint64_t gesture = 0;
	// Apply: the change (null for any other operation).
	std::shared_ptr<const EditPayload> payload;
};

// A gesture token no edit has carried yet in this process.
uint64_t next_edit_gesture();

// Inside one batch (Document::apply), the record an earlier edit of that batch made:
// batch_made(i) names what the batch's i-th edit added (an Add's record, a Duplicate's
// copy, a Paste's first record), as a later edit's record (address.child) or owner
// (parent), so one undo step can add a record and fill it in (the editor MCP's
// editor_menu edit). Such an edit names the batch's row, or row 0 for "the batch's".
// No document hands out such an identity; one naming an edit that made nothing, or
// that comes later, refuses the batch (document.batch).
constexpr NodeId kBatchMadeBase = NodeId(1) << 62;
constexpr NodeId batch_made(size_t index) { return kBatchMadeBase + NodeId(index); }
constexpr bool is_batch_made(NodeId id) { return id >= kBatchMadeBase; }

} // namespace opennova::editor
