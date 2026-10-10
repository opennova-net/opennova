#include <editor/preview/mission_palette.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_map_outline.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

struct GroupRow {
	MissionPaletteGroup group;
	const char *token;
	const char *words;
};
constexpr GroupRow kGroups[] = {
	{ MissionPaletteGroup::Recent, "recent", "Recently placed" },
	{ MissionPaletteGroup::People, "people", "People" },
	{ MissionPaletteGroup::Vehicles, "vehicles", "Vehicles" },
	{ MissionPaletteGroup::Objects, "objects", "Objects" },
	{ MissionPaletteGroup::Buildings, "buildings", "Buildings" },
	{ MissionPaletteGroup::Decoration, "decoration", "Decoration and foliage" },
	{ MissionPaletteGroup::Markers, "markers", "Markers" },
	{ MissionPaletteGroup::Effects, "effects", "Effects" },
	{ MissionPaletteGroup::Other, "other", "Other" },
};
static_assert(std::size(kGroups) == kMissionPaletteGroupCount, "a row per palette group");

// The file name of a project path (a model's), for the filter and the wire.
std::string file_name(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

const char *mission_palette_group_words(MissionPaletteGroup group) {
	return size_t(group) < std::size(kGroups) ? kGroups[size_t(group)].words : "Other";
}

const char *mission_palette_group_token(MissionPaletteGroup group) {
	return size_t(group) < std::size(kGroups) ? kGroups[size_t(group)].token : "other";
}

MissionPaletteGroup mission_palette_group_of_type(int type) {
	// The TYPE vocabulary [orig: ItemDef_ParseProperty @ 0x49eb00]: 1 vehicle, 2 decoration/foliage, 3
	// person, 4 marker, 5 building, 6 powerup/object, 8 effect (formats/def DefItemType).
	switch (type) {
	case def::DEF_ITEM_TYPE_VEHICLE: return MissionPaletteGroup::Vehicles;
	case def::DEF_ITEM_TYPE_DECORATION: return MissionPaletteGroup::Decoration;
	case def::DEF_ITEM_TYPE_PERSON: return MissionPaletteGroup::People;
	case def::DEF_ITEM_TYPE_MARKER: return MissionPaletteGroup::Markers;
	case def::DEF_ITEM_TYPE_BUILDING: return MissionPaletteGroup::Buildings;
	case def::DEF_ITEM_TYPE_POWERUP: return MissionPaletteGroup::Objects;
	case def::DEF_ITEM_TYPE_EFFECT: return MissionPaletteGroup::Effects;
	default: return MissionPaletteGroup::Other;
	}
}

const char *mission_pool_words(MissionKind pool) {
	switch (pool) {
	case MissionKind::Organic: return "organics";
	case MissionKind::Building: return "buildings";
	case MissionKind::Marker: return "markers";
	default: return "items";
	}
}

MissionPalette mission_palette(const AssetGraph &graph, const std::string &filter, const std::vector<int64_t> &recent) {
	MissionPalette out;
	const std::vector<const GraphSymbol *> symbols = graph.symbols_of_kind(ReferenceKind::Item);
	// Each catalog's `graphic` edges, resolved once per catalog: its record's model.
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> models;
	const auto model_of = [&](const GraphSymbol &symbol) -> const std::string & {
		auto catalog = models.find(symbol.file);
		if (catalog == models.end()) {
			catalog = models.emplace(symbol.file, std::unordered_map<std::string, std::string>()).first;
			for (const GraphEdge *edge : graph.references_of(symbol.file)) {
				if (edge->field != "graphic" || catalog->second.count(edge->record)) continue;
				std::string file;
				if (graph.resolve(*edge, &file) != ReferenceStatus::Present) file.clear();
				catalog->second.emplace(edge->record, file);
			}
		}
		static const std::string none;
		const auto model = catalog->second.find(symbol.record);
		return model == catalog->second.end() ? none : model->second;
	};
	const std::string wanted = strutil::to_lower(filter);
	std::unordered_map<int64_t, size_t> by_id;
	std::unordered_set<int64_t> seen;
	for (const GraphSymbol *symbol : symbols) {
		if (symbol->inert) continue;
		const std::optional<int> id = strutil::parse_int(symbol->name);
		if (!id) continue;
		// The first definition of an id stands (as the lookup finds it): an id defined twice counts once.
		if (!seen.insert(*id).second) continue;
		++out.count;
		MissionPaletteItem item;
		item.item = *id;
		item.name = symbol->record;
		if (const std::optional<int> type = strutil::parse_int(symbol->value)) {
			item.type = *type;
			item.pool = mission_item_pool_of_type(*type);
		}
		item.group = mission_palette_group_of_type(item.type);
		item.model = model_of(*symbol);
		item.file = symbol->file;
		if (!wanted.empty()) {
			const std::string words = strutil::to_lower(item.name + " " + symbol->name + " " + file_name(item.model));
			if (words.find(wanted) == std::string::npos) continue;
		}
		by_id.emplace(item.item, out.items.size());
		out.items.push_back(std::move(item));
	}
	// The recently placed, as placed, then each group's by name.
	MissionPaletteSection recently{ MissionPaletteGroup::Recent, {} };
	for (const int64_t id : recent) {
		const auto found = by_id.find(id);
		if (found == by_id.end()) continue;
		out.items[found->second].recent = true;
		recently.items.push_back(found->second);
	}
	if (!recently.items.empty()) out.sections.push_back(std::move(recently));
	std::vector<MissionPaletteSection> groups(kMissionPaletteGroupCount);
	for (size_t i = 0; i < out.items.size(); ++i) groups[size_t(out.items[i].group)].items.push_back(i);
	for (size_t g = 1; g < groups.size(); ++g) {
		MissionPaletteSection &section = groups[g];
		if (section.items.empty()) continue;
		section.group = MissionPaletteGroup(g);
		std::sort(section.items.begin(), section.items.end(), [&](size_t a, size_t b) {
			const std::string left = strutil::to_lower(out.items[a].name), right = strutil::to_lower(out.items[b].name);
			return left != right ? left < right : out.items[a].item < out.items[b].item;
		});
		out.sections.push_back(std::move(section));
	}
	return out;
}

io::JsonValue mission_palette_to_json(const MissionPalette &palette, const JsonPage &page) {
	JsonValue out = JsonValue::make_object();
	out.set("count", json_number(double(palette.count)));
	out.set("matching", json_number(double(palette.items.size())));
	JsonValue groups = JsonValue::make_array();
	std::vector<std::pair<MissionPaletteGroup, size_t>> order;
	for (const MissionPaletteSection &section : palette.sections) {
		JsonValue group = JsonValue::make_object();
		group.set("group", json_string(mission_palette_group_token(section.group)));
		group.set("words", json_string(mission_palette_group_words(section.group)));
		if (section.group != MissionPaletteGroup::Recent && !section.items.empty())
			group.set("pool", json_string(mission_pool_words(palette.items[section.items.front()].pool)));
		group.set("count", json_number(double(section.items.size())));
		groups.push(std::move(group));
		for (const size_t index : section.items) order.emplace_back(section.group, index);
	}
	out.set("groups", std::move(groups));
	JsonValue items = JsonValue::make_array();
	for (size_t i = page.first(order.size()); i < page.last(order.size()); ++i) {
		const MissionPaletteItem &item = palette.items[order[i].second];
		JsonValue row = JsonValue::make_object();
		row.set("item", json_number(double(item.item)));
		row.set("name", json_string(item.name));
		row.set("type", item.type < 0 ? JsonValue::make_null() : json_number(double(item.type)));
		row.set("type_words", json_string(item.type < 0 ? "unknown" : def::def_item_type_name(item.type)));
		row.set("group", json_string(mission_palette_group_token(order[i].first)));
		row.set("pool", json_string(mission_pool_words(item.pool)));
		row.set("model", json_string(item.model));
		row.set("file", json_string(item.file));
		row.set("recent", JsonValue::make_bool(item.recent));
		items.push(std::move(row));
	}
	out.set("items", std::move(items));
	set_page(out, page, order.size());
	return out;
}

// --- the pictures ---------------------------------------------------------------------------------

namespace {

// A project file by the name the asset source serves it by (a path's file name).
std::string served_file(const std::string &file) {
	const size_t slash = file.find_last_of('/');
	return slash == std::string::npos ? file : file.substr(slash + 1);
}

uint64_t stamp_of(const SessionView &view, const std::string &model) {
	return view.findings.assets ? view.findings.assets->stamp(served_file(model)) : 0;
}

} // namespace

std::shared_ptr<const MissionPalettePicture> MissionPalettePictures::get(const SessionView &view,
		const std::string &model) const {
	if (model.empty() || !view.findings.assets) return nullptr;
	const auto found = pictures_.find(model);
	const bool current = found != pictures_.end() && found->second->stamp == stamp_of(view, model);
	if (!current && std::find(queue_.begin(), queue_.end(), model) == queue_.end()) queue_.push_back(model);
	return found == pictures_.end() ? nullptr : found->second;
}

bool MissionPalettePictures::step(const SessionView &view, int64_t budget_us) {
	if (queue_.empty() || !view.findings.assets) return false;
	const auto start = std::chrono::steady_clock::now();
	bool made = false;
	while (!queue_.empty()) {
		if (made && std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() >=
				budget_us)
			break;
		const std::string model = queue_.front();
		queue_.erase(queue_.begin());
		auto picture = std::make_shared<MissionPalettePicture>();
		picture->model = model;
		picture->stamp = stamp_of(view, model);
		std::vector<uint8_t> bytes;
		if (view.findings.assets->read(served_file(model), bytes) && !bytes.empty()) {
			threedi::Threedi3di3 parsed{};
			MissionModelOutline outline;
			if (threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &parsed) == 0 &&
					mission_model_outline(parsed, outline, MissionOutlineView::Side)) {
				picture->read = true;
				bool any = false;
				for (size_t i = 0; i + 5 < outline.edges.size(); i += 6) {
					// Seen from the side: forward across, up up.
					const float line[4] = { outline.edges[i], outline.edges[i + 2], outline.edges[i + 3],
						outline.edges[i + 5] };
					for (int end = 0; end < 2; ++end)
						for (int axis = 0; axis < 2; ++axis) {
							const float v = line[2 * end + axis];
							picture->lo[axis] = any ? std::min(picture->lo[axis], v) : v;
							picture->hi[axis] = any ? std::max(picture->hi[axis], v) : v;
							any = any || axis == 1;
						}
					picture->lines.insert(picture->lines.end(), line, line + 4);
				}
			}
			threedi::threedi_3di3_free(&parsed);
		}
		pictures_[model] = std::move(picture);
		++made_;
		made = true;
	}
	return made;
}

void MissionPalettePictures::clear() {
	pictures_.clear();
	queue_.clear();
}

} // namespace opennova::editor
