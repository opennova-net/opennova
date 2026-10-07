// Placing a model no item of the project draws yet (model_placement.h, ADR 0046 DI-12).

#include <editor/preview/model_placement.h>

#include <algorithm>
#include <optional>
#include <unordered_set>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/def_table.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

namespace {

// The open document at a project path (null: not open).
const DocumentBase *open_at(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

// The item rows of an open catalog, each by its id and its name.
template <typename Visit> void open_items(const SessionView &view, const std::string &catalog, Visit visit) {
	const DocumentBase *open = open_at(view, catalog);
	const Document *records = open ? records_of(*open) : nullptr;
	if (!records) return;
	const NodeKind item = node_kind(def::DefRecordKind::Item);
	for (const auto &row : records->rows()) {
		if (!row || row->kind != item) continue;
		Value id;
		if (records->get({ row->id, row->kind, 0 }, "id", id))
			if (const auto *number = std::get_if<int64_t>(&id)) visit(*number, row->name());
	}
}

Edit set_made(const char *field, Value value) {
	Edit edit;
	edit.address = { batch_made(0), node_kind(def::DefRecordKind::Item), 0 };
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// The item by its name and its id: "oncrate2 (100020)".
std::string item_label(const ModelItemPlan &plan) {
	return plan.name + " (" + std::to_string(plan.id) + ")";
}

} // namespace

int64_t free_project_item_id(const SessionView &view, const std::string &catalog) {
	std::unordered_set<int64_t> used;
	if (const AssetGraph *graph = view.findings.graph.get()) {
		for (const GraphSymbol *symbol : graph->symbols_of_kind(ReferenceKind::Item))
			if (const std::optional<int> id = strutil::parse_int(symbol->name)) used.insert(*id);
		graph->for_each_edge([&](const GraphEdge &edge) {
			if (edge.kind != ReferenceKind::Item) return;
			if (const std::optional<int> id = strutil::parse_int(edge.value)) used.insert(*id);
		});
	}
	open_items(view, catalog, [&](int64_t id, const std::string &) { used.insert(id); });
	return free_item_id([&](int id) { return used.count(id) != 0; });
}

bool plan_model_item(const SessionView &view, const AssetEntry &model, ModelItemPlan &out, std::string &error) {
	out = ModelItemPlan();
	out.model = model.relative_path;
	// The model as the project holds it now (an open document's edits included), its parts read.
	std::vector<uint8_t> bytes;
	threedi::Threedi3di3 parsed{};
	if (!view.findings.assets || !view.findings.assets->read(model.logical_name, bytes) || bytes.empty() ||
	    threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &parsed) != 0) {
		threedi::threedi_3di3_free(&parsed);
		error = model.logical_name + " does not read as a model, so no item can draw it.";
		return false;
	}
	out.facts = model_item_facts(parsed);
	threedi::threedi_3di3_free(&parsed);
	// The graphic as items.def names a model: its file's name less ".3di" (the game loads "<graphic>.3di").
	out.graphic = model.logical_name;
	if (out.graphic.size() > 4 && strutil::iequals(out.graphic.substr(out.graphic.size() - 4), ".3di"))
		out.graphic.resize(out.graphic.size() - 4);
	if (!out.facts.makes) {
		error = "No item of the project draws " + model.logical_name + ", and it is " + model_item_words(out.facts) +
		        ": its item needs what the model does not say (" +
		        (out.facts.kind == ModelItemKind::Person    ? "a person's animation table, anim_def, and his classes"
		         : out.facts.kind == ModelItemKind::Vehicle ? "a vehicle's class and its physics"
		                                                    : "the mounted gun's class and its weapon") +
		        "). Add its item in items.def (Add record, graphic " + out.graphic + "), then drop the item.";
		return false;
	}
	ReferenceSubject items;
	items.kind = ReferenceKind::Item;
	const AssetEntry *catalog = defining_file(items, view);
	if (!catalog) {
		error = "The project has no item catalog for " + model.logical_name + "'s item: create items.def (Problems' "
		        "Create), then drop the model again.";
		return false;
	}
	out.catalog = catalog->relative_path;
	// The game keeps 15 characters of it: its ItemDef's graphic name is 16 bytes [orig: ItemDef_DumpToFile
	// @ 0x49e250, graphicName +0x60 before huskName +0x70].
	if (out.graphic.empty() || out.graphic.size() > 15) {
		error = "An item's graphic holds 15 characters: rename " + model.logical_name + " shorter to make its item.";
		return false;
	}
	// A name no item has: the game's lookups by name find the first item of a name (itemdef-re.md, "Repeated
	// names and ids").
	std::unordered_set<std::string> names;
	if (const AssetGraph *graph = view.findings.graph.get())
		for (const GraphSymbol *symbol : graph->symbols_of_kind(ReferenceKind::Item)) names.insert(strutil::to_upper(symbol->record));
	open_items(view, out.catalog, [&](int64_t, const std::string &name) { names.insert(strutil::to_upper(name)); });
	out.name = out.graphic;
	for (int n = 2; names.count(strutil::to_upper(out.name)); ++n) out.name = out.graphic + " " + std::to_string(n);
	out.id = free_project_item_id(view, out.catalog);
	// One batch: the item added (a marker of an id of its own, as Add makes one), then its id, TYPE and
	// graphic set on what it made.
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = node_kind(def::DefRecordKind::Item);
	add.field = "display_name";
	add.value = out.name;
	out.edits.push_back(std::move(add));
	out.edits.push_back(set_made("id", out.id));
	out.edits.push_back(set_made("type", int64_t(out.facts.type)));
	out.edits.push_back(set_made("graphic", out.graphic));
	return true;
}

std::string model_item_plan_words(const ModelItemPlan &plan) {
	const size_t slash = plan.catalog.find_last_of('/');
	std::string words = "item " + item_label(plan) + ", " + model_item_words(plan.facts) + ", in " +
	                    (slash == std::string::npos ? plan.catalog : plan.catalog.substr(slash + 1));
	if (plan.facts.doors)
		words += " (its " + std::to_string(plan.facts.doors) + (plan.facts.doors == 1 ? " door stays" : " doors stay") +
		         " shut: the door class is the item's to set)";
	return words;
}

// The mission a Place in mission of `what` arms Place in exists (place_in_mission_target); false with why: no
// mission open, or several and none last active.
static bool mission_to_place_in(const SessionView &view, const std::string &what, std::string &error) {
	if (!place_in_mission_target(view).empty()) return true;
	error = std::none_of(view.documents.open.begin(), view.documents.open.end(),
	                     [](const auto &document) { return document && document->kind() == AssetKind::Mission; })
	                ? "No mission is open: open the mission to place " + what + " in, then Place in mission."
	                : "Several missions are open: make the one to place " + what + " in active, then Place in mission.";
	return false;
}

std::string place_in_mission_target(const SessionView &view) {
	const auto mission_at = [&](const std::string &path) {
		const DocumentBase *open = path.empty() ? nullptr : open_at(view, path);
		return open && open->kind() == AssetKind::Mission;
	};
	if (mission_at(view.documents.active)) return view.documents.active;
	for (const NavigationPlace &place : view.navigation.back)
		if (mission_at(place.path)) return place.path;
	std::string only;
	for (const auto &document : view.documents.open) {
		if (!document || document->kind() != AssetKind::Mission) continue;
		if (!only.empty()) return std::string();
		only = document->path();
	}
	return only;
}

bool plan_place_in_mission(const SessionView &view, const std::string &model, CanvasRequests &out, std::string &error) {
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(model) : nullptr;
	if (!entry) entry = view.project.scan ? view.project.scan->find(model) : nullptr;
	if (!entry || entry->kind != AssetKind::Model) {
		error = "Place in mission places a model: " + model + " is none.";
		return false;
	}
	if (!mission_to_place_in(view, entry->logical_name, error)) return false;
	const std::vector<int64_t> drawers = mission_items_of_model(view, entry->relative_path);
	if (drawers.size() > 1) {
		error = "Several items draw " + entry->logical_name + ":";
		for (size_t i = 0; i < drawers.size(); ++i) {
			MissionItemFacts facts;
			std::string ignored;
			mission_item_facts(view, drawers[i], facts, ignored);
			error += std::string(i ? ", " : " ") + (facts.name.empty() ? std::string() : facts.name + " ") + "(" +
			         std::to_string(drawers[i]) + ")";
		}
		error += ". Pick one in the mission's Place palette.";
		return false;
	}
	int64_t item = 0;
	std::string words;
	if (drawers.empty()) {
		ModelItemPlan plan;
		if (!plan_model_item(view, *entry, plan, error)) return false;
		out.request(request::edit_record(plan.catalog, plan.edits, true));
		item = plan.id;
		words = "Made " + model_item_plan_words(plan) + " for " + entry->logical_name + "; ";
	} else {
		item = drawers.front();
		MissionItemFacts facts;
		std::string ignored;
		mission_item_facts(view, item, facts, ignored);
		words = (facts.name.empty() ? std::string("Item ") : facts.name + " ") + "(" + std::to_string(item) + ") draws " +
		        entry->logical_name + "; ";
	}
	return plan_place_item_in_mission(view, item, words, out, error);
}

bool plan_place_item_in_mission(const SessionView &view, int64_t item, const std::string &lead, CanvasRequests &out,
                                std::string &error) {
	MissionItemFacts facts;
	std::string ignored;
	mission_item_facts(view, item, facts, ignored);
	const std::string named = facts.name.empty() ? "item " + std::to_string(item) : facts.name;
	if (!mission_to_place_in(view, named, error)) return false;
	const std::string mission = place_in_mission_target(view);
	// The mission made active (a jump the history records), then its Place tool armed with the item.
	out.request(request::open_document(mission));
	io::JsonValue options = io::JsonValue::make_object();
	options.set("tool", io::json_string(mission_tool_token(MissionTool::Place)));
	options.set("item", io::json_number(double(item)));
	out.request(request::set_viewport(mission, viewport_change(ViewportKind::Mission, "options", std::move(options))));
	const size_t slash = mission.find_last_of('/');
	std::string words = lead.empty() ? named + " (" + std::to_string(item) + "): " : lead;
	words += "Place is armed in " + (slash == std::string::npos ? mission : mission.substr(slash + 1)) +
	         ": a click on its picture places one on the ground there.";
	out.served(std::move(words), 0);
	return true;
}

} // namespace opennova::editor
