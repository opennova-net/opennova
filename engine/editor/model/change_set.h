#pragma once

#include <algorithm>
#include <cstddef>
#include <variant>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// What changed in a document between two of its states (DocumentBase::changes_since, ADR 0046 S13
// D7), in the words of its kind: a record document's rows, a text's spans, a raster's regions. A
// caller that keeps what it made of a document (a viewport's picture, an outline's lines, the
// selection; S13 V8) follows these, and takes everything as changed when the document cannot say.

// A record document's rows: those the later state has and the earlier lacks (added), those it
// lacks (removed), and those both have in another version (changed: a field of the row or of a
// record it holds, or what it holds), each list sorted by identity; whether rows both have stand in
// another order among one another (reordered); and whether the file-wide state is another
// (file_state).
struct RowChanges {
	std::vector<NodeId> added, removed, changed;
	bool reordered = false;
	bool file_state = false;
	bool empty() const {
		return added.empty() && removed.empty() && changed.empty() && !reordered && !file_state;
	}
	// Whether the row `id` is among those added, removed or changed (a binary search of the list).
	bool was_added(NodeId id) const { return std::binary_search(added.begin(), added.end(), id); }
	bool was_removed(NodeId id) const { return std::binary_search(removed.begin(), removed.end(), id); }
	bool was_changed(NodeId id) const { return std::binary_search(changed.begin(), changed.end(), id); }
	// Whether the rows stand otherwise: one added or removed, or rows both states have in another
	// order (what a caller keeping something per row by its place makes again).
	bool reshapes() const { return !added.empty() || !removed.empty() || reordered; }
};

// A text document's spans (S13 D9 makes them): each run of text that changed, by the line and the
// column it starts at and its length in the later state.
struct TextSpan {
	size_t line = 0, column = 0, length = 0;
};
struct TextChanges {
	std::vector<TextSpan> spans;
};

// A raster's regions (the terrain slice makes them): each rectangle of cells that changed.
struct RasterRegion {
	int x = 0, y = 0, width = 0, height = 0;
};
struct RasterChanges {
	std::vector<RasterRegion> regions;
};

using ChangeSet = std::variant<RowChanges, TextChanges, RasterChanges>;

} // namespace opennova::editor
