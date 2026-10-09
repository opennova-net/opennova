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
#include <formats/mission/bms_edit.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/mission/placement_traits.h>
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

// A file's stamp as the asset source serves it (0: none).
uint64_t stamp_of(const SessionView &view, const std::string &file) {
	return view.findings.assets && !file.empty() ? view.findings.assets->stamp(served_name(file)) : 0;
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

void mission_model_words(const float model[3], double out[3]) {
	out[0] = double(model[2]);
	out[1] = -double(model[0]);
	out[2] = double(model[1]);
}

void mission_anchor_offset(const double words[3], int32_t scale_q16, double pitch, double yaw, double roll, double out[3]) {
	// The point as the device's model holds it (ObjectData's godot_vec3 of the threedi_user_point_position
	// order: x mirrored), carried by the entity's placement basis at the item's scale (the placer's
	// item_entity_transform, mission_effects' owner_pose), and back into the mission's frame.
	const double local[3] = { words[1], words[2], words[0] };
	const mission::PlacementBasis basis = mission::bms_to_presentation_basis(float(pitch), float(yaw), float(roll));
	const double scale = scale_q16 != 0 ? double(scale_q16) / io::kFp16OneD : 1.0;
	const mission::PlacementVec3 at{
		float((basis.x.x * local[0] + basis.y.x * local[1] + basis.z.x * local[2]) * scale),
		float((basis.x.y * local[0] + basis.y.y * local[1] + basis.z.y * local[2]) * scale),
		float((basis.x.z * local[0] + basis.y.z * local[1] + basis.z.z * local[2]) * scale) };
	const mission::PlacementVec3 mission_at = mission::presentation_to_bms_position(at);
	out[0] = mission_at.x;
	out[1] = mission_at.y;
	out[2] = mission_at.z;
}

// --- MissionItemCache ----------------------------------------------------------------------------

const MissionItemCache::Model &MissionItemCache::model_(const SessionView &view, const std::string &file) {
	Model &model = models_[file];
	const uint64_t stamp = stamp_of(view, file);
	if (model.stamp == stamp && stamp != 0) return model;
	model = Model();
	model.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (file.empty() || !view.findings.assets || !view.findings.assets->read(served_name(file), bytes) || bytes.empty())
		return model;
	++files_read_;
	threedi::Threedi3di3 parsed{};
	if (threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &parsed) == 0) {
		model.read = true;
		model.radius_q16 = world::model_bound_radius_q16_from_3di(parsed);
		model.collision = parsed.collision != nullptr;
		// The model's ground anchor, read from the project's file as the game reads it, in the model's own
		// axes: where it stands from an entity is the game's placement matrix over it at the entity's
		// angles (mission_anchor_offset).
		float anchor[3] = { 0.0f, 0.0f, 0.0f };
		if (threedi::threedi_3di3_ground_anchor(&parsed, anchor)) mission_model_words(anchor, model.anchor);
	}
	threedi::threedi_3di3_free(&parsed);
	return model;
}

const MissionItemCache::Catalog &MissionItemCache::catalog_(const SessionView &view, const std::string &file) {
	Catalog &catalog = catalogs_[file];
	const uint64_t stamp = stamp_of(view, file);
	if (catalog.stamp == stamp && stamp != 0) return catalog;
	catalog = Catalog();
	catalog.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (!view.findings.assets || !view.findings.assets->read(served_name(file), bytes) || bytes.empty()) return catalog;
	++files_read_;
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0)
		for (size_t i = 0; i < items.count; ++i) {
			const def::DefItemDef &row = items.entries[i];
			catalog.scale_q16.emplace(int64_t(row.id), row.scale_q16);
			Catalog::Seed seed;
			seed.good = row.attrib_good != 0;
			seed.evil = row.attrib_evil != 0;
			seed.sid = row.sid_derived ? std::string() : std::string(row.sid);
			seed.default_aip = row.default_aip;
			seed.min_engagement = row.min_engagement_dist;
			seed.max_engagement = row.max_engagement_dist;
			seed.max_attack = row.max_attack_dist;
			seed.fire_timer = row.fire_timer;
			catalog.seeds.emplace(int64_t(row.id), std::move(seed));
		}
	def::def_free_items(&items);
	return catalog;
}

bool MissionItemCache::row(const SessionView &view, int64_t item, MissionItemFacts &out) {
	out = MissionItemFacts();
	out.item = item;
	const AssetGraph *graph = view.findings.graph.get();
	const GraphSymbol *symbol = graph ? graph->resolve_symbol(ReferenceKind::Item, std::to_string(item)) : nullptr;
	if (!symbol) return false;
	out.name = symbol->record;
	if (const std::optional<int> type = strutil::parse_int(symbol->value)) {
		out.type = *type;
		out.pool = mission_item_pool_of_type(*type);
	}
	// What a record placed of it takes from its row (D-MIS-10).
	{
		const Catalog &catalog = catalog_(view, symbol->file);
		const auto seed = catalog.seeds.find(item);
		if (seed != catalog.seeds.end()) {
			out.seeded = true;
			out.team = seed->second.evil ? 2 : seed->second.good ? 1 : 0;
			const std::string &sid = seed->second.sid;
			out.ai_class = sid.substr(0, sid.find('.')).substr(0, 8);
			// The profile as the original names its file: the name's extension made .aip [orig: JOTACmed.exe
			// sub_462CB0 @ 0x462cb0 with "aip" @ 0x5b2d40, the file asked for by sub_529FA0 @ 0x529fa0].
			const std::string &aip = seed->second.default_aip;
			if (!aip.empty() && graph->has_file(aip.substr(0, aip.find('.')) + ".aip")) out.ai_script = aip.substr(0, 8);
			out.min_engagement = seed->second.min_engagement;
			out.max_engagement = seed->second.max_engagement;
			out.max_attack = seed->second.max_attack;
			out.fire_timer = seed->second.fire_timer;
		}
	}
	return true;
}

bool MissionItemCache::facts(const SessionView &view, int64_t item, MissionItemFacts &out, std::string &error) {
	if (!row(view, item, out)) {
		error = "No item catalog of the project defines item " + std::to_string(item) + ".";
		return false;
	}
	const AssetGraph *graph = view.findings.graph.get();
	const GraphSymbol *symbol = graph->resolve_symbol(ReferenceKind::Item, std::to_string(item));
	const ItemModels models = models_of(*graph, *symbol);
	out.model = models.graphic;
	if (out.model.empty()) return true;
	const Model &model = model_(view, out.model);
	if (!model.read) return true;
	for (int i = 0; i < 3; ++i) out.anchor[i] = model.anchor[i];
	const Catalog &catalog = catalog_(view, symbol->file);
	const auto scale = catalog.scale_q16.find(item);
	out.scale_q16 = scale == catalog.scale_q16.end() ? 0 : scale->second;
	// Its entity's bound: the model's, by the item's SCALE, at least its first husk's.
	if (model.collision) {
		const Model *husk = models.husk.empty() ? nullptr : &model_(view, models.husk);
		out.radius = mission_item_bound_radius(model.radius_q16, true, out.scale_q16, husk && husk->read,
				husk ? husk->radius_q16 : 0);
	}
	return true;
}

bool MissionItemCache::model_facts(const SessionView &view, int64_t item, int type, const std::string &file,
		MissionItemFacts &out) {
	out = MissionItemFacts();
	out.item = item;
	out.type = type;
	out.pool = mission_item_pool_of_type(type);
	out.model = file;
	const Model &model = model_(view, file);
	if (!model.read) return false;
	for (int i = 0; i < 3; ++i) out.anchor[i] = model.anchor[i];
	if (model.collision) out.radius = mission_item_bound_radius(model.radius_q16, true, 0, false, 0);
	return true;
}

void MissionItemCache::ask_(const SessionView &view, int64_t item, const Reads &reads) {
	++items_asked_;
	radii_.erase(item);
	if (reads.graphic.empty()) return;
	const Model &model = model_(view, reads.graphic);
	if (!model.read || !model.collision) return;
	const int32_t radius = model.radius_q16;
	const Catalog &catalog = catalog_(view, reads.catalog);
	const auto scale = catalog.scale_q16.find(item);
	const Model *husk = reads.husk.empty() ? nullptr : &model_(view, reads.husk);
	const double bound = mission_item_bound_radius(radius, true, scale == catalog.scale_q16.end() ? 0 : scale->second,
			husk && husk->read, husk ? husk->radius_q16 : 0);
	if (bound > 0.0) radii_[item] = float(bound);
}

void MissionItemCache::refresh(const SessionView &view, const std::vector<int64_t> &items) {
	const AssetGraph *graph = view.findings.graph.get();
	const uint64_t generation = graph ? graph->generation() : 0;
	if (graph_ != (graph != nullptr) || graph_generation_ != generation) {
		// Another graph: every item asked again (each file read again only where its stamp moved).
		graph_ = graph != nullptr;
		graph_generation_ = generation;
		asked_.clear();
		radii_.clear();
	}
	if (!graph) return;
	// A file a stamp of which may have moved (the asset source's generation: an open document edited, a
	// rescan): the items whose files moved asked again, the others standing.
	const uint64_t files = view.findings.assets ? view.findings.assets->generation() : 0;
	if (files != files_generation_) {
		files_generation_ = files;
		std::unordered_set<std::string> moved;
		for (const auto &model : models_)
			if (stamp_of(view, model.first) != model.second.stamp) moved.insert(model.first);
		for (const auto &catalog : catalogs_)
			if (stamp_of(view, catalog.first) != catalog.second.stamp) moved.insert(catalog.first);
		if (!moved.empty())
			for (auto &each : asked_)
				if (moved.count(each.second.catalog) || moved.count(each.second.graphic) || moved.count(each.second.husk))
					ask_(view, each.first, each.second);
	}
	// The edges naming each record's models, gathered once per catalog a refresh reaches (a catalog's
	// edges walked once, not once per item).
	std::map<std::string, std::unordered_map<std::string, std::vector<const GraphEdge *>>> loads;
	for (const int64_t item : items) {
		if (asked_.count(item)) continue;
		Reads &reads = asked_[item];
		const GraphSymbol *symbol = graph->resolve_symbol(ReferenceKind::Item, std::to_string(item));
		if (!symbol) {
			++items_asked_;
			continue;
		}
		reads.catalog = symbol->file;
		auto catalog = loads.find(symbol->file);
		if (catalog == loads.end()) {
			catalog = loads.emplace(symbol->file, std::unordered_map<std::string, std::vector<const GraphEdge *>>()).first;
			for (const GraphEdge *edge : graph->references_of(symbol->file))
				if (edge->field == "graphic" || edge->field == "husk") catalog->second[edge->record].push_back(edge);
		}
		ItemModels models;
		if (const auto found = catalog->second.find(symbol->record); found != catalog->second.end())
			for (const GraphEdge *edge : found->second) take_model(*graph, *edge, models);
		reads.graphic = models.graphic;
		reads.husk = models.husk;
		ask_(view, item, reads);
	}
}

void plan_item_seeds(const SessionView &view, MissionItemCache &cache, const MissionDocument &mission,
                     std::vector<Edit> &edits) {
	struct Placed {
		NodeAddress at;
		int64_t item = 0;
	};
	std::vector<Placed> placed;
	const size_t count = edits.size();
	for (size_t i = 0; i < count; ++i) {
		const Edit &edit = edits[i];
		const int64_t *item = std::get_if<int64_t>(&edit.value);
		if (!item || edit.field != "item" || !is_entity_kind(edit.address.kind) || edit.address.child) continue;
		if (edit.operation == EditOperation::Add && !edit.address.row) {
			placed.push_back({ NodeAddress{ batch_made(i), edit.address.kind, 0 }, *item });
		} else if (edit.operation == EditOperation::Set && edit.address.row && !is_batch_made(edit.address.row)) {
			// A record naming no item yet (an Add's own, item 0) taking its first.
			const Node *row = mission.row(edit.address.row);
			if (row && is_entity_kind(row->kind) && static_cast<const EntityRow &>(*row).native.type_id == 0)
				placed.push_back({ NodeAddress{ row->id, row->kind, 0 }, *item });
		}
	}
	for (const Placed &each : placed) {
		MissionItemFacts facts;
		if (!cache.row(view, each.item, facts) || !facts.seeded) continue;
		const auto seed = [&](const char *field, Value value) {
			for (size_t i = 0; i < count; ++i)
				if (edits[i].operation == EditOperation::Set && edits[i].address == each.at && edits[i].field == field) return;
			Edit set;
			set.address = each.at;
			set.field = field;
			set.value = std::move(value);
			set.gesture = edits.front().gesture;
			edits.push_back(std::move(set));
		};
		seed("team", int64_t(facts.team));
		seed("name2", facts.ai_script);
		seed("min_engagement_distance", int64_t(facts.min_engagement));
		seed("max_engagement_distance", int64_t(facts.max_engagement));
		seed("max_attack_distance", int64_t(facts.max_attack));
		seed("advancetimer", int64_t(facts.fire_timer));
	}
}

std::shared_ptr<const MissionItemClasses> mission_item_classes(const SessionView &view, MissionItemCache &cache,
                                                               const MissionDocument &mission) {
	auto classes = std::make_shared<MissionItemClasses>();
	std::unordered_set<int64_t> asked;
	for (const auto &row : mission.rows()) {
		if (!row || !is_entity_kind(row->kind)) continue;
		const int64_t item = mission::entity_item_id(static_cast<const EntityRow &>(*row).native);
		if (!asked.insert(item).second) continue;
		MissionItemFacts facts;
		if (cache.row(view, item, facts) && facts.seeded) (*classes)[item] = facts.ai_class;
	}
	return classes;
}

bool mission_item_facts(const SessionView &view, int64_t item, MissionItemFacts &out, std::string &error) {
	MissionItemCache cache;
	return cache.facts(view, item, out, error);
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
