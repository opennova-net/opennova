#include <editor/graph/graph_layer.h>

#include <algorithm>
#include <utility>

#include <editor/graph/graph_names.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

std::shared_ptr<const GraphLayer> GraphLayer::build(
		const std::vector<LayerFile> &files, const std::string &game, GraphStats *stats) {
	auto layer = std::make_shared<GraphLayer>();
	GraphStats counted;
	for (const LayerFile &file : files) {
		const std::string name = basename_of(file.name);
		const std::string name_key = graph_names::key(name);
		if (name_key.empty() || layer->index_.first_named(name_key) != GraphIndex::kNone) continue;
		const uint32_t id = layer->index_.add(name, name, name_key, file.kind);
		if (!graph_reads_file(file.kind, name)) continue;
		GraphSlot &slot = layer->index_.slot(id);
		slot.read = true;
		++counted.files_extracted;
		std::vector<uint8_t> bytes;
		std::string message;
		Extracted content;
		Diagnostic error;
		if (!file.read || !file.read(bytes, message) ||
				!extract_from_bytes(name, file.kind, bytes, game, content, error)) {
			// No finding: the layer's names from this file are left out.
			slot.ok = false;
			++counted.files_failed;
			continue;
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
		layer->index_.insert_content(id);
	}
	if (stats) *stats = counted;
	return layer;
}

const GraphSlot *GraphLayer::file_named(const std::string &name) const {
	const uint32_t id = index_.first_named(graph_names::key(basename_of(name)));
	return id == GraphIndex::kNone ? nullptr : &index_.slot(id);
}

} // namespace opennova::editor
