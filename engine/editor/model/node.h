#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// A row of a document: the native record behind it belongs to the document type.
// Rows are immutable once committed (shared as `const`); an edit clones the row,
// changes the clone and commits it, which is what makes undo a row swap.
struct Node {
	NodeId id = 0;
	NodeKind kind = 0;
	// The identities of the nested records: the default identity store, one vector per
	// slot the document type keeps for this kind (the def catalogs, the string tables).
	// A type that nests deeper keeps its own store and overrides for_each_identity;
	// Document::collections is the owner-scoped view every caller reads.
	std::vector<std::vector<NodeId>> collections;

	virtual ~Node() = default;
	virtual std::shared_ptr<Node> clone() const = 0;
	virtual std::string name() const = 0;
	// Every nested identity in a fixed order, so the ids a load assigns are the same for
	// the same file (a rename reloads a document and finds its records again).
	virtual void for_each_identity(const std::function<void(NodeId &)> &fn) {
		for (auto &collection : collections)
			for (NodeId &id : collection) fn(id);
	}
};

// File-wide state that is not a row (the items.def vehicle spawn registry). Immutable
// once committed, like a row.
struct FileState {
	virtual ~FileState() = default;
	virtual std::shared_ptr<FileState> clone() const = 0;
};

} // namespace opennova::editor
