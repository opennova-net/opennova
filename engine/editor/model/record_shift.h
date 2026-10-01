#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// Where an edit left the records of a collection other records name by their index (a Record
// reference, graph/reference_kinds: ADR 0046 S13 D8): every record of the collection's kind in the
// file, in the file's order, as the edit found them, each with the index it stands at now or none
// (the edit removed it), and how many the edit left. What Document::renumber_references is handed,
// to make every reference name the record it named, and an index past the collection (naming no
// record) name none still.
struct RecordShift {
	static constexpr size_t kRemoved = SIZE_MAX;
	ReferenceKind reference = ReferenceKind::None; // the Record kind naming the collection
	NodeKind kind = 0;                               // the collection's records' kind
	std::vector<size_t> to;                          // by the index before the edit: the one after
	size_t after = 0;                                // the records the edit left

	// The number of records the edit found.
	size_t before() const { return to.size(); }
	// Whether an index named a record the edit found.
	bool found(int64_t index) const { return index >= 0 && size_t(index) < to.size(); }
	// The index a reference to `index` names after the edit: its record's place now (kRemoved: the
	// record is gone). An index past the records the edit found named none, and moves by what the
	// collection grew or shrank, so it names none still: after a register added at the front, an
	// index one past the table is one past it again, never the register that slid into its place.
	size_t now(int64_t index) const {
		if (found(index)) return to[size_t(index)];
		if (index < 0) return size_t(index);
		return size_t(index + int64_t(after) - int64_t(to.size()));
	}
	// Whether the edit moves any index: a record moved or removed, or the collection grew or shrank
	// (an index past it moves).
	bool moves() const {
		if (after != to.size()) return true;
		for (size_t k = 0; k < to.size(); ++k)
			if (to[k] != k) return true;
		return false;
	}
};

} // namespace opennova::editor
