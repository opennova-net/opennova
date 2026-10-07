#include <editor/graph/graph_layer.h>

#include <algorithm>
#include <utility>

#include <editor/graph/graph_names.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

std::shared_ptr<const GraphLayer> GraphLayer::build(
		const std::vector<LayerFile> &files, const std::string &game, GraphStats *stats) {
	GraphLayerBuilder builder(game);
	for (const LayerFile &file : files) builder.add(file);
	if (stats) *stats = builder.stats();
	return builder.finish();
}

GraphLayerBuilder::GraphLayerBuilder(std::string game) :
		layer_(std::make_shared<GraphLayer>()), game_(std::move(game)) {}

uint64_t GraphLayerBuilder::add(const LayerFile &file) {
	if (!layer_) return 0;
	GraphIndex &index = layer_->index_;
	const std::string name = basename_of(file.name);
	const std::string name_key = graph_names::key(name);
	if (name_key.empty() || index.first_named(name_key) != GraphIndex::kNone) return 0;
	const uint32_t id = index.add(name, name, name_key, file.kind);
	if (!graph_reads_kind(file.kind)) return 0;
	GraphSlot &slot = index.slot(id);
	slot.read = true;
	++stats_.files_extracted;
	std::vector<uint8_t> bytes;
	std::string message;
	Extracted content;
	Diagnostic error;
	if (!file.read || !file.read(bytes, message) ||
			!extract_from_bytes(name, file.kind, bytes, game_, content, error)) {
		// No finding: the layer's names from this file are left out.
		slot.ok = false;
		++stats_.files_failed;
		return bytes.size();
	}
	slot.symbols = std::move(content.symbols);
	// A record set names what its own file's references resolve to (a Record reference, S13 D8):
	// no project file's, so the layer keeps none.
	slot.symbols.erase(std::remove_if(slot.symbols.begin(), slot.symbols.end(),
	                                  [](const GraphSymbol &symbol) {
		                                  return reference_row(symbol.kind).resolution ==
		                                         ReferenceResolution::Record;
	                                  }),
	                   slot.symbols.end());
	index.insert_content(id);
	return bytes.size();
}

std::shared_ptr<const GraphLayer> GraphLayerBuilder::finish() {
	std::shared_ptr<const GraphLayer> layer = std::move(layer_);
	layer_.reset();
	return layer;
}

const GraphSlot *GraphLayer::file_named(const std::string &name) const {
	const uint32_t id = index_.first_named(graph_names::key(basename_of(name)));
	return id == GraphIndex::kNone ? nullptr : &index_.slot(id);
}

} // namespace opennova::editor
