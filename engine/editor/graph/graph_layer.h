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
// (what the mount's files reference is the mount's business), no record sets (a Record reference
// resolves in its own file, S13 D8) and no findings (a file that does not read leaves its names
// out). Immutable once built, so one layer may sit under several graphs
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
	friend class GraphLayerBuilder;
	GraphIndex index_;
};

// A layer built a file at a time, so an operation steps it within its budget (session/base_layer_build.h):
// each file added as build() adds it (the first of a name kept, a kind the graph reads read through its
// extractor, one that does not read left out of the names), the layer taken once at the end.
class GraphLayerBuilder {
public:
	explicit GraphLayerBuilder(std::string game);
	// One file added; the bytes it read (0 for a file of a kind the graph does not read, or one of a name
	// added already).
	uint64_t add(const LayerFile &file);
	// The layer of the files added; the builder holds none after.
	std::shared_ptr<const GraphLayer> finish();
	const GraphStats &stats() const { return stats_; }

private:
	std::shared_ptr<GraphLayer> layer_;
	std::string game_;
	GraphStats stats_;
};

} // namespace opennova::editor
