#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <editor/model/document.h>

namespace opennova::editor {

// The records of one kind that nest in one another inside a row (a menu's windows), as
// the tree a window draws them in: each with its owner (0 = the row) and its index among
// its owner's records of that kind, in pre-order. A record of the kind held by a record
// of another kind (a window a list box part holds) is left out with what it holds. Built
// by one walk of the row (Document::walk_records).
struct RecordTree {
	struct Entry {
		NodeAddress address;
		NodeId owner = 0;              // the tree record holding it (0 = the row)
		size_t index = 0;              // its index in its owner's collection of the kind
		int depth = 0;                 // 0 for a record the row holds
		std::vector<size_t> children;  // indices into entries, in order
	};
	NodeId row = 0;
	NodeKind kind = 0;
	std::vector<Entry> entries; // pre-order
	std::vector<size_t> roots;  // the row's own records of the kind, in order

	const Entry *find(NodeId id) const;
	// The records of the kind `owner` holds (0 = the row), as indices into entries.
	const std::vector<size_t> &children_of(NodeId owner) const;
	// True when `record` is `ancestor` or sits inside it.
	bool inside(NodeId record, NodeId ancestor) const;

private:
	friend RecordTree build_record_tree(const Document &document, const Node &row, NodeKind kind);
	std::unordered_map<NodeId, size_t> at_;
};

RecordTree build_record_tree(const Document &document, const Node &row, NodeKind kind);

// Where a dragged record lands on another: before it or after it among its siblings, or
// inside it, at the end.
enum class DropPlace { Before, Inside, After };

// The one Move each gesture makes (Edit::parent names the destination owner, the row's
// own id for the row's list; the position is the index there after the record leaves its
// place). False when the record cannot go there (onto or inside itself, the first
// sibling indented, a record the row holds outdented).
bool drop_edit(const RecordTree &tree, NodeId dragged, NodeId target, DropPlace place, Edit &out);
// Indent: into the sibling before it, at the end.
bool indent_edit(const RecordTree &tree, NodeId record, Edit &out);
// Outdent: out of its owner, right after it among the owner's siblings.
bool outdent_edit(const RecordTree &tree, NodeId record, Edit &out);

} // namespace opennova::editor
