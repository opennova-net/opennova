#include <editor/preview/mission_items.h>

#include <algorithm>
#include <optional>

#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/mission/authoring.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/model_geometry.h>

namespace opennova::editor {

namespace {

// The models an item's record loads (their project files, "" for none): its `graphic`, and its
// `husk` (the first husk, which the entity's bound takes the larger of).
struct ItemModels {
	std::string graphic, husk;
};
void take_model(const AssetGraph &graph, const GraphEdge &edge, ItemModels &out) {
	std::string *into = edge.field == "graphic" ? &out.graphic : edge.field == "husk" ? &out.husk : nullptr;
	std::string file;
	if (into && into->empty() && graph.resolve(edge, &file) == ReferenceStatus::Present) *into = file;
}
ItemModels models_of(const AssetGraph &graph, const GraphSymbol &item) {
	ItemModels out;
	for (const GraphEdge *edge : graph.references_of(item.file))
		if (edge->record == item.record) take_model(graph, *edge, out);
	return out;
}

// A project file by its name as the asset source serves it (a path's file name).
std::string served_name(const std::string &file) {
	const size_t slash = file.find_last_of('/');
	return slash == std::string::npos ? file : file.substr(slash + 1);
}

// What the bound reads of a parsed model: GHDR's radius and whether it has a collision block.
void model_bound(const threedi::Threedi3di3 &model, int32_t &radius_q16, bool &collision) {
	radius_q16 = world::model_bound_radius_q16_from_3di(model);
	collision = model.collision != nullptr;
}

// A model file of the project parsed: false when it is not there or does not read.
bool read_model(const SessionView &view, const std::string &file, int32_t &radius_q16, bool &collision) {
	std::vector<uint8_t> bytes;
	if (file.empty() || !view.findings.assets || !view.findings.assets->read(served_name(file), bytes) || bytes.empty())
		return false;
	threedi::Threedi3di3 model{};
	const bool read = threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0;
	if (read) model_bound(model, radius_q16, collision);
	threedi::threedi_3di3_free(&model);
	return read;
}

// An item catalog's SCALE by item id (the first definition of an id the file holds).
std::unordered_map<int64_t, int32_t> read_scales(const SessionView &view, const std::string &file) {
	std::unordered_map<int64_t, int32_t> out;
	std::vector<uint8_t> bytes;
	if (!view.findings.assets || !view.findings.assets->read(served_name(file), bytes) || bytes.empty()) return out;
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0)
		for (size_t i = 0; i < items.count; ++i) out.emplace(int64_t(items.entries[i].id), items.entries[i].scale_q16);
	def::def_free_items(&items);
	return out;
}

} // namespace

double mission_item_bound_radius(int32_t model_q16, bool collision, int32_t scale_q16, bool has_husk, int32_t husk_q16) {
	world::EntityBoundRadiusInputs inputs;
	inputs.model_radius_q16 = model_q16;
	inputs.uniform_scale_q16 = scale_q16;
	inputs.has_collision_block = collision;
	inputs.has_first_husk = has_husk;
	inputs.first_husk_radius_q16 = husk_q16;
	const int32_t radius = world::entity_bound_radius_q16(inputs);
	return radius > 0 ? double(radius) / io::kFp16OneD : 0.0;
}

MissionKind mission_item_pool_of_type(int type) {
	switch (mission::authoring::entity_kind_for_item_type(type)) {
	case mission::EntityKind::Building: return MissionKind::Building;
	case mission::EntityKind::Marker: return MissionKind::Marker;
	case mission::EntityKind::Organic: return MissionKind::Organic;
	default: return MissionKind::Item;
	}
}

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
		out.pool = mission_item_pool_of_type(*type);
	}
	const ItemModels models = models_of(*graph, *symbol);
	out.model = models.graphic;
	// The model's ground anchor, read from the project's file as the game reads it. NEEDS-RE (ADR 0046
	// S14, review m9): the original editor subtracts the Ground userpoint's +0/+4/+8 words from the
	// entity's position unrotated (docs/world/world-wac-ai-re.md section 12, dfx2med.exe @ 0x401f6e,
	// 0x4021fe); whether those are the file's raw x/y/z (forward, left, up) taken as mission x/y/z, or
	// the presentation swizzle used here (mission x, y = the raw y, -x), is unwitnessed. z agrees either
	// way; an anchor off the model's vertical axis moves the stored x and y. Read 0x401f6e in a dfx2med
	// database to settle it.
	std::vector<uint8_t> bytes;
	int32_t radius_q16 = 0;
	bool collision = false;
	if (!out.model.empty() && view.findings.assets && view.findings.assets->read(served_name(out.model), bytes) &&
			!bytes.empty()) {
		threedi::Threedi3di3 model{};
		if (threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0) {
			float anchor[3] = { 0.0f, 0.0f, 0.0f };
			if (threedi::threedi_3di3_ground_anchor(&model, anchor)) mission_model_point(anchor, out.anchor);
			model_bound(model, radius_q16, collision);
		}
		threedi::threedi_3di3_free(&model);
	}
	// Its entity's bound: the model's, by the item's SCALE, at least its first husk's.
	if (collision) {
		const std::unordered_map<int64_t, int32_t> scales = read_scales(view, symbol->file);
		const auto scale = scales.find(item);
		int32_t husk_q16 = 0;
		bool husk_collision = false;
		const bool husk = read_model(view, models.husk, husk_q16, husk_collision);
		out.radius = mission_item_bound_radius(radius_q16, collision, scale == scales.end() ? 0 : scale->second, husk, husk_q16);
	}
	return true;
}

// --- MissionItemBounds ---------------------------------------------------------------------------

const MissionItemBounds::Model &MissionItemBounds::model_(const SessionView &view, const std::string &file) {
	Model &model = models_[file];
	const uint64_t stamp = view.findings.assets ? view.findings.assets->stamp(served_name(file)) : 0;
	if (model.stamp == stamp && stamp != 0) return model;
	model = Model();
	model.stamp = stamp;
	++files_read_;
	model.read = read_model(view, file, model.radius_q16, model.collision);
	return model;
}

const MissionItemBounds::Catalog &MissionItemBounds::catalog_(const SessionView &view, const std::string &file) {
	Catalog &catalog = catalogs_[file];
	const uint64_t stamp = view.findings.assets ? view.findings.assets->stamp(served_name(file)) : 0;
	if (catalog.stamp == stamp && stamp != 0) return catalog;
	catalog = Catalog();
	catalog.stamp = stamp;
	++files_read_;
	catalog.scale_q16 = read_scales(view, file);
	return catalog;
}

void MissionItemBounds::refresh(const SessionView &view, const std::vector<int64_t> &items) {
	const AssetGraph *graph = view.findings.graph.get();
	const uint64_t generation = graph ? graph->generation() : 0;
	if (graph_ != (graph != nullptr) || generation_ != generation) {
		// Another graph: every item asked again (each file read again only where its stamp moved).
		graph_ = graph != nullptr;
		generation_ = generation;
		asked_.clear();
		radii_.clear();
	}
	if (!graph) return;
	// The edges naming each record's models, gathered once per catalog a refresh reaches (a catalog's
	// edges walked once, not once per item).
	std::map<std::string, std::unordered_map<std::string, std::vector<const GraphEdge *>>> loads;
	for (const int64_t item : items) {
		if (!asked_.insert(item).second) continue;
		const GraphSymbol *symbol = graph->resolve_symbol(ReferenceKind::Item, std::to_string(item));
		if (!symbol) continue;
		auto catalog = loads.find(symbol->file);
		if (catalog == loads.end()) {
			catalog = loads.emplace(symbol->file, std::unordered_map<std::string, std::vector<const GraphEdge *>>()).first;
			for (const GraphEdge *edge : graph->references_of(symbol->file))
				if (edge->field == "graphic" || edge->field == "husk") catalog->second[edge->record].push_back(edge);
		}
		const auto found = catalog->second.find(symbol->record);
		if (found == catalog->second.end()) continue;
		ItemModels models;
		for (const GraphEdge *edge : found->second) take_model(*graph, *edge, models);
		if (models.graphic.empty()) continue;
		const Model &model = model_(view, models.graphic);
		if (!model.read || !model.collision) continue;
		const Catalog &scales = catalog_(view, symbol->file);
		const auto scale = scales.scale_q16.find(item);
		const Model *husk = models.husk.empty() ? nullptr : &model_(view, models.husk);
		const double radius = mission_item_bound_radius(model.radius_q16, model.collision,
				scale == scales.scale_q16.end() ? 0 : scale->second, husk && husk->read, husk ? husk->radius_q16 : 0);
		if (radius > 0.0) radii_[item] = float(radius);
	}
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
