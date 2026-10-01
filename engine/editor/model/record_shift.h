#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// Where an edit left the records of a collection other records name by their index (a Record
// reference, graph/reference_kinds: ADR 0046 S13 D8): every record of the collection's kind in the
// file, in the file's order, as the edit found them, each with the index it stands at now or none
// (the edit removed it). What Document::renumber_references is handed, to make every reference name
// the record it named.
struct RecordShift {
	static constexpr size_t kRemoved = SIZE_MAX;
	ReferenceKind reference = ReferenceKind::None; // the Record kind naming the collection
	NodeKind kind = 0;                               // the collection's records' kind
	std::vector<size_t> to;                          // by the index before the edit: the one after

	// The number of records the edit found.
	size_t before() const { return to.size(); }
	// Whether an index names a record the edit found (an index past them named none, and names none
	// the edit could move).
	bool found(int64_t index) const { return index >= 0 && size_t(index) < to.size(); }
	// The index a reference to record `index` names now (kRemoved: the record is gone); an index
	// past the records the edit found stays as it was.
	size_t now(int64_t index) const { return found(index) ? to[size_t(index)] : size_t(index); }
};

} // namespace opennova::editor
