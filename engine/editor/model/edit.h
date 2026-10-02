#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/model/value.h>

namespace opennova::editor {

// What the windows and the shell ask a document to change: one typed operation on one
// address, or a batch of them over any rows (DocumentBase::apply; the record document's,
// Document::apply, S13 D7). A document applies either as a single undoable change.
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
// edit's payload as its token and reads none back but a text document's span replaced
// (TextSpanEdit, "text.span", S13 D9): the editor MCP sends no other Apply edit.
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
	// kind (SIZE_MAX: the end; a Duplicate's, right after its record as the edits before it in
	// its batch left it).
	size_t position = SIZE_MAX;
	// Consecutive Sets of the same field fold into one undo step, each applied to the record
	// as the group's first Set found it (the step is that record plus the latest value).
	bool coalesce = false;
	// Batches whose edits carry the same nonzero gesture (a drag) fold into one undo step over
	// every row they change until the edit group ends (EndEdit; a batch that adds, removes or
	// moves a row is a step of its own and ends it); each applies to the rows as the one before
	// left them. next_edit_gesture() hands out a fresh one.
	uint64_t gesture = 0;
	// Apply: the change (null for any other operation).
	std::shared_ptr<const EditPayload> payload;
};

// Two edits that ask the same (a request read back from its wire form, S13 A4).
inline bool operator==(const Edit &a, const Edit &b) {
	return a.operation == b.operation && a.address == b.address && a.parent == b.parent &&
	       a.field == b.field && a.value == b.value && a.position == b.position &&
	       a.coalesce == b.coalesce && a.gesture == b.gesture && a.payload == b.payload;
}
inline bool operator!=(const Edit &a, const Edit &b) { return !(a == b); }

// A gesture token no edit has carried yet in this process.
uint64_t next_edit_gesture();

// Inside one batch (Document::apply), what an earlier edit of that batch made: batch_made(i)
// names what the batch's i-th edit made (an Add's row or record, a Duplicate's copy, a Paste's
// first), as a later edit's row, record (address.child) or owner (parent); a made row named as
// a record or an owner is the row itself. So one undo step can add a record and fill it in (a
// request's edits in the batch form, record_batch.h). Such an edit names the row the record was
// made in, or row 0. No document hands out such an identity; one naming an edit that made
// nothing, one that comes later, or a record made in another row than the edit names refuses
// the batch (document.batch), as an edit naming a row an earlier edit removed does.
constexpr NodeId kBatchMadeBase = NodeId(1) << 62;
constexpr NodeId batch_made(size_t index) { return kBatchMadeBase + NodeId(index); }
constexpr bool is_batch_made(NodeId id) { return id >= kBatchMadeBase; }

} // namespace opennova::editor
