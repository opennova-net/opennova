#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// The bytes a row's own content adds up to (Node::footprint): a string's characters and a vector's
// elements as they stand (their sizes, not what the allocator reserved, so the same on every
// platform's library but for the elements' own sizes).
inline size_t footprint_of(const std::string &text) { return text.size(); }
template <class T> size_t footprint_of(const std::vector<T> &items) {
	return items.size() * sizeof(T);
}
template <class K, class V> size_t footprint_of(const std::unordered_map<K, V> &entries) {
	return entries.size() * sizeof(typename std::unordered_map<K, V>::value_type);
}

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
	// The bytes this version of the row holds (ADR 0046 S13 D7): its own object and everything it
	// holds (each word, each list and what each element of it holds), so what an undo step keeping
	// it costs (EditHistory's budget). What every version of the row shares while the document
	// holds it (a model's or a clip's parsed base) is not its own; an index of its records it keeps
	// (made again by a structural edit, shared by a clone until then) is counted in each version
	// that keeps it. An estimate from the sizes of what it holds (footprint_of); a committed row's
	// never changes.
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
	// The bytes this version holds, as a row's (Node::footprint): what an undo step that changes
	// the file-wide state keeps of it (EditHistory's budget), never less than its own object.
	virtual size_t footprint() const = 0;
};

} // namespace opennova::editor
