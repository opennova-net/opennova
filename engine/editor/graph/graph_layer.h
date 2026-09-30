#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_index.h>

namespace opennova::editor {

// One file of a base layer: the logical name the engine looks it up by (what the layer's symbols
// and files name it by), its kind, and its bytes as stored (the game's loader decodes them; a
// payload already decoded passes that decode unchanged), false with why when they cannot be read.
struct LayerFile {
	std::string name;
	AssetKind kind = AssetKind::Unknown;
	std::function<bool(std::vector<uint8_t> &bytes, std::string &error)> read;
};

// A base layer of the asset graph (ADR 0046 d10, S13 D3): the files of a read-only dependency
// mount (a game install an expansion-type project builds on), read once through the same
// extractors as the project's files, with the names each defines, and nothing else: no edges
// (what the mount's files reference is the mount's business) and no findings (a file that does not
// read leaves its names out). Immutable once built, so one layer may sit under several graphs
// (AssetGraph::set_base, where the project's files win over the layer's of the same name).
class GraphLayer {
public:
	// The layer of `files` (each name once: the first of a name kept) for `game`'s decoding;
	// `stats`, when given, counts the files read and those that did not read.
	static std::shared_ptr<const GraphLayer> build(const std::vector<LayerFile> &files,
			const std::string &game, GraphStats *stats = nullptr);

	// The files by logical name and their symbols, in the same index a project's graph keeps.
	const GraphIndex &index() const { return index_; }
	// The layer's file of a name (its logical name, or a path's file name); null for none.
	const GraphSlot *file_named(const std::string &name) const;
	size_t file_count() const { return index_.slot_count(); }
	size_t symbol_count() const { return index_.symbol_count(); }

private:
	GraphIndex index_;
};

} // namespace opennova::editor
