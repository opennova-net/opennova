#include <editor/preview/mission_items.h>

#include <algorithm>
#include <optional>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/authoring.h>
#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

namespace {

// The pool a TYPE puts a record in, as the game's editor places it.
MissionKind pool_of_type(int type) {
	switch (mission::authoring::entity_kind_for_item_type(type)) {
	case mission::EntityKind::Building: return MissionKind::Building;
	case mission::EntityKind::Marker: return MissionKind::Marker;
	case mission::EntityKind::Organic: return MissionKind::Organic;
	default: return MissionKind::Item;
	}
}

// The model a record's `graphic` loads (its project file), "" for none.
std::string model_of(const AssetGraph &graph, const GraphSymbol &item) {
	for (const GraphEdge *edge : graph.references_of(item.file)) {
		if (edge->record != item.record || edge->field != "graphic") continue;
		std::string file;
		if (graph.resolve(*edge, &file) == ReferenceStatus::Present) return file;
		return std::string();
	}
	return std::string();
}

} // namespace

void mission_model_point(const float model[3], double out[3]) {
	out[0] = -double(model[0]);
	out[1] = -double(model[2]);
	out[2] = double(model[1]);
}

bool mission_item_facts(const SessionView &view, int64_t item, MissionItemFacts &out, std::string &error) {
	out = MissionItemFacts();
	out.item = item;
	const AssetGraph *graph = view.findings.graph.get();
	const GraphSymbol *symbol = graph ? graph->resolve_symbol(ReferenceKind::Item, std::to_string(item)) : nullptr;
	if (!symbol) {
		error = "No item catalog of the project defines item " + std::to_string(item) + ".";
		return false;
	}
	out.name = symbol->record;
	if (const std::optional<int> type = strutil::parse_int(symbol->value)) {
		out.type = *type;
		out.pool = pool_of_type(*type);
	}
	out.model = model_of(*graph, *symbol);
	// The model's ground anchor, read from the project's file as the game reads it.
	std::vector<uint8_t> bytes;
	if (!out.model.empty() && view.findings.assets) {
		const size_t slash = out.model.find_last_of('/');
		const std::string name = slash == std::string::npos ? out.model : out.model.substr(slash + 1);
		if (view.findings.assets->read(name, bytes) && !bytes.empty()) {
			threedi::Threedi3di3 model{};
			if (threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0) {
				float anchor[3] = { 0.0f, 0.0f, 0.0f };
				if (threedi::threedi_3di3_ground_anchor(&model, anchor)) mission_model_point(anchor, out.anchor);
			}
			threedi::threedi_3di3_free(&model);
		}
	}
	return true;
}

std::vector<int64_t> mission_items_of_model(const SessionView &view, const std::string &file) {
	std::vector<int64_t> out;
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return out;
	for (const GraphEdge *edge : graph->referrers_of_file(file)) {
		if (edge->field != "graphic") continue;
		for (const GraphSymbol *symbol : graph->symbols_of(edge->source, edge->record)) {
			if (symbol->kind != ReferenceKind::Item) continue;
			const std::optional<int> id = strutil::parse_int(symbol->name);
			if (id && std::find(out.begin(), out.end(), int64_t(*id)) == out.end()) out.push_back(int64_t(*id));
		}
	}
	return out;
}

} // namespace opennova::editor
