#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/graph/graph_edge.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/value.h>

namespace opennova::editor {

// A style variable's definition as its own file's lookup makes it (the extraction:
// Document::refine_symbol's inert, where a later line defines the name again or the game stops
// reading the file before it), kept beside the symbol the graph publishes, whose inert is the
// game's reading of the whole project (the shell's stylesheets, brand.mns over menu_style.mns).
struct OwnFacts {
	bool inert = false;
	std::string inert_reason;
};

// How one edge of a file resolves, as the graph last resolved it: whether it is Present, the file
// it loads (a File reference that resolves: what "Referenced by" lists it under), whether it is
// one of the missing ones and, while it is, its finding (AssetGraph::missing_finding: made as the
// edge resolves, and again when what its words read changes). `resolved` false: not resolved since
// the file was read, so none of its entries is in the index.
struct EdgeResolution {
	bool resolved = false;
	ReferenceStatus status = ReferenceStatus::NotAReference;
	std::string file;
	bool missing = false;
	Diagnostic finding;
};

// One file of the project (ADR 0046 S13 D3), or of a base layer: its row in the scan, what the
// graph read from it (its extraction, and what that was read from) and how each of its edges
// resolves. A file the graph does not read (a texture, a mission's .mis) holds no extraction; its
// row still counts (the file set).
struct GraphSlot {
	std::string path;         // project-relative; a base layer's file: its logical name
	std::string logical_name; // the file's name, as the scan lists it
	std::string key;          // its normalized logical name: the files go in (key, path) order
	AssetKind kind = AssetKind::Unknown;
	// What the extraction was read from: a closed file's size and last write, or an open
	// document's instance and revision. `read` false: the graph does not read the file.
	bool read = false;
	bool open = false;
	uint64_t size = 0;
	int64_t modified = 0;
	uint64_t identity = 0;
	uint64_t revision = 0;
	// Whether the file read (else what it references goes unchecked), and the graph.unreadable
	// warning when it did not (no code where a document type's validation reports it).
	bool ok = true;
	Diagnostic failure;
	// What it references, each edge's target as last resolved, and what it defines, a style
	// variable's inert as the game reads the project; both in the file's order.
	std::vector<GraphEdge> edges;
	std::vector<GraphSymbol> symbols;
	std::vector<EdgeResolution> resolutions; // one per edge
	std::vector<OwnFacts> own;               // one per symbol if the file defines a style variable
	// The file's own indexes: its edges and its symbols by the record's locator (by (source,
	// locator)), its symbols by the record's path (by (file, record)).
	std::unordered_map<std::string, std::vector<uint32_t>> edges_at;
	std::unordered_map<std::string, std::vector<uint32_t>> symbols_at;
	std::unordered_map<std::string, std::vector<uint32_t>> symbols_in;

	// A symbol's inert, and why, as its own file's lookup makes them.
	bool own_inert(size_t symbol) const {
		return own.empty() ? symbols[symbol].inert : own[symbol].inert;
	}
	const std::string &own_reason(size_t symbol) const {
		return own.empty() ? symbols[symbol].inert_reason : own[symbol].inert_reason;
	}
};

// The asset graph's per-file slots and the indexes over them (ADR 0046 S13 D3): the symbols by
// kind and name, by kind, by (file, record) and by (file, locator); the edges by kind and target,
// by source and by (source, locator), and by the style variable their value names; the file edges
// that load each file (its users) and the missing edges. A slot's content goes out of the indexes
// and back in as one (erase_content, insert_content), so an update patches the slots that changed
// and nothing else; an edge's target, its file and its missing entry go in as it is resolved
// (AssetGraph). Every list is in the files' order, (key, path) as the scan sorts them, then each
// file's own. A base layer (GraphLayer) is the same index over its files' symbols, with no edges.
// A copy is whole: a slot is addressed by its id, which the copy keeps.
class GraphIndex {
public:
	// A place in the index: a slot, and an edge's or a symbol's index in it.
	struct Ref {
		uint32_t slot = 0;
		uint32_t index = 0;
	};
	static constexpr uint32_t kNone = UINT32_MAX;

	// The key of a name in a namespace: an edge's kind and target, a symbol's kind and name.
	static std::string key_of(ReferenceKind kind, const std::string &name);

	// --- the slots ---
	// The slot at a path; kNone for none.
	uint32_t find(const std::string &path) const;
	// The first slot, by path, of a normalized logical name (the file the name resolves to);
	// kNone for none.
	uint32_t first_named(const std::string &key) const;
	const GraphSlot &slot(uint32_t id) const { return slots_[id]; }
	GraphSlot &slot(uint32_t id) { return slots_[id]; }
	// A slot for a file, empty: its id.
	uint32_t add(const std::string &path, const std::string &logical_name, const std::string &key,
			AssetKind kind);
	// The slot gone; its content must be out of the indexes (erase_content).
	void remove(uint32_t id);
	// Every slot's id in the files' order.
	template <typename Visit> void for_each_slot(Visit visit) const {
		for (const auto &entry : order_) visit(entry.second);
	}
	size_t slot_count() const { return order_.size(); }
	size_t edge_count() const { return edges_; }
	size_t symbol_count() const { return symbols_; }
	const GraphEdge &edge(Ref ref) const { return slots_[ref.slot].edges[ref.index]; }
	const GraphSymbol &symbol(Ref ref) const { return slots_[ref.slot].symbols[ref.index]; }
	// The order of two places: the files' order, then the place in the file.
	bool before(Ref a, Ref b) const;

	// --- a slot's content ---
	// Its symbols and edges out of every index, their resolutions' entries too (by target, by
	// user, the missing), and out of the counts; the slot keeps them.
	void erase_content(uint32_t id);
	// Its symbols and edges into the indexes, each edge unresolved (its resolution's entries go in
	// as it is resolved), and the file's own indexes made again.
	void insert_content(uint32_t id);

	// --- an edge's resolution's entries ---
	void add_target(const std::string &key, Ref ref) { insert(targets_[key], ref); }
	void remove_target(const std::string &key, Ref ref) { erase_from(targets_, key, ref); }
	void add_user(const std::string &file, Ref ref) { insert(users_[file], ref); }
	void remove_user(const std::string &file, Ref ref) { erase_from(users_, file, ref); }
	void add_missing(Ref ref) { insert(missing_, ref); }
	void remove_missing(Ref ref) { erase(missing_, ref); }

	// --- the lookups ---
	// The symbols of a kind and name (key_of), in order.
	const std::vector<Ref> &symbols_named(const std::string &key) const {
		return list(names_, key);
	}
	const std::vector<Ref> &symbols_of_kind(ReferenceKind kind) const;
	// The edges of a kind and target (key_of), in order.
	const std::vector<Ref> &edges_targeting(const std::string &key) const {
		return list(targets_, key);
	}
	// The edges whose value is the style variable `name` (its %NAME%, upper case as the graph keys
	// it): the variable's own edges, and the files named through it.
	const std::vector<Ref> &edges_through(const std::string &name) const {
		return list(variables_, name);
	}
	// The file edges that load a file (by its path), in order.
	const std::vector<Ref> &users_of(const std::string &file) const { return list(users_, file); }
	const std::vector<Ref> &missing() const { return missing_; }

private:
	using Lists = std::unordered_map<std::string, std::vector<Ref>>;
	static const std::vector<Ref> &list(const Lists &lists, const std::string &key);
	// The first place of a list (in the files' order) not before `ref`.
	std::vector<Ref>::iterator lower(std::vector<Ref> &list, Ref ref) const;
	void insert(std::vector<Ref> &list, Ref ref);
	void erase(std::vector<Ref> &list, Ref ref);
	void erase_from(Lists &lists, const std::string &key, Ref ref);

	std::deque<GraphSlot> slots_; // by id; a freed id is taken by the next file added
	std::vector<uint32_t> free_;
	std::unordered_map<std::string, uint32_t> paths_;               // path -> id (by source)
	std::map<std::pair<std::string, std::string>, uint32_t> order_; // (key, path) -> id
	Lists names_;                                                   // kind + name -> symbols
	std::array<std::vector<Ref>, kReferenceKindCount> kinds_;       // kind -> symbols
	Lists targets_;                                                 // kind + target -> edges
	Lists variables_;                                               // variable -> edges naming it
	Lists users_;                                                   // path -> the edges loading it
	std::vector<Ref> missing_;
	size_t edges_ = 0;
	size_t symbols_ = 0;
};

} // namespace opennova::editor
