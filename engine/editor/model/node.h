#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// The bytes a row's own content adds up to (Node::footprint): a string's characters and a vector's
// elements as they stand (their sizes, not what the allocator reserved, so the same on every
// platform's library but for the elements' own sizes).
inline size_t footprint_of(const std::string &text) { return text.size(); }
template <class T> size_t footprint_of(const std::vector<T> &items) { return items.size() * sizeof(T); }

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
	// The bytes this version of the row holds of its own (ADR 0046 S13 D7): what its clone copies,
	// so what an undo step keeping it costs (EditHistory's budget). What it shares with its other
	// versions (a model's or a clip's parsed base) is not its own. An estimate from the sizes of what
	// it holds (footprint_of), never less than the row's own object; a committed row's never changes.
	virtual size_t footprint() const = 0;

protected:
	// The identity store's part of a footprint.
	size_t collections_footprint() const {
		size_t bytes = footprint_of(collections);
		for (const auto &collection : collections) bytes += footprint_of(collection);
		return bytes;
	}
};

// File-wide state that is not a row (the items.def vehicle spawn registry). Immutable
// once committed, like a row.
struct FileState {
	virtual ~FileState() = default;
	virtual std::shared_ptr<FileState> clone() const = 0;
};

} // namespace opennova::editor
