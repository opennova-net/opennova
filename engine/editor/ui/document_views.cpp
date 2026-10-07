#include "document_views.h"

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_table.h>
#include <editor/model/document_base.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/animation_inspector.h>
#include <editor/ui/catalog_inspector_view.h>
#include <editor/ui/environment_inspector.h>
#include <editor/ui/terrain_inspector.h>
#include <editor/ui/main_viewport_view.h>
#include <editor/ui/menu_inspector.h>
#include <editor/ui/menu_view.h>
#include <editor/ui/mission_logic_view.h>
#include <editor/ui/model_inspector_view.h>
#include <editor/ui/outline_view.h>
#include <editor/ui/script_view.h>
#include <editor/ui/sound_inspector.h>
#include <editor/ui/styles_view.h>
#include <editor/ui/texture_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>
#include <formats/def/reserved_items.h>

#include <imgui.h>

#include <set>

namespace opennova::editor {

namespace {

// An item table's vehicle spawn registry (items.def's file-wide ids), the catalog's file-wide
// values: a slot's item id each, set by an Edit SetFileValue at its slot.
bool catalog_file_values(const Document &document, OutlineFileValues &out) {
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	if (!catalog || catalog->kind() != AssetKind::ItemDefs) return false;
	out.title = "Vehicle spawn IDs";
	out.label = "Slot";
	out.tip = "The item id the game spawns in this vehicle slot.";
	out.add_label = "Add spawn slot";
	out.add_tip = "Adds a slot at the end (item id 0).";
	out.max = size_t(def::DEF_VEHICLE_SPAWN_SLOTS);
	out.full_tip = "The registry holds " + std::to_string(out.max) + " slots at most.";
	for (const int id : catalog->spawn_ids()) out.values.push_back(id);
	return true;
}

// An items.def's rows the engine looks for by their ids (formats/def/reserved_items.h; ADR 0046 S19,
// "Reserved item ids"): the places and objectives (a start, a waypoint, a flag), the starts and the
// waypoints first, each the file lacks added on its id and of its kind (reserved_item_add_edits: one
// undo step), each it has listed as there.
bool catalog_offers_engine_items(const Document &document) {
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	return catalog && catalog->kind() == AssetKind::ItemDefs;
}

void draw_catalog_engine_items(Workspace &workspace, const Document &document) {
	std::set<int> ids;
	for (const auto &row : document.rows())
		if (def_kind(row->kind) == def::DefRecordKind::Item)
			ids.insert(static_cast<const CatalogRow &>(*row).native.as<def::DefItemDef>().id);
	size_t count = 0;
	const def::ReservedItem *rows = def::reserved_items(&count);
	for (const bool places : {true, false})
		for (size_t i = 0; i < count; ++i) {
			const def::ReservedItem &reserved = rows[i];
			if (reserved.rule != def::ReservedItemRule::Refuse || (reserved.type >= 6000) != places) continue;
			const int id = def::DEF_ITEM_ID_BASE + reserved.type;
			const bool has = ids.count(id) != 0;
			const std::string label = std::string(reserved.label) + " (" + std::to_string(id) + ")" +
			                          (has ? ": in the file" : "") + "###" + std::to_string(id);
			if (ImGui::MenuItem(label.c_str(), nullptr, false, !has))
				workspace.request(request::edit_record(document.path(), reserved_item_add_edits(reserved)));
			ui_kit::tooltip(reserved.use);
		}
}

// A mission's 128 waypoint paths are the file's own, most of them empty: one with no stop is not
// listed until the outline's switch lists them.
bool mission_row_listed(const Document &, const Node &row) {
	return row.kind != node_kind(MissionKind::WaypointPath) || (!row.collections.empty() && !row.collections[0].empty());
}

constexpr OutlineSpec kCatalogOutline{OutlineMode::List,
                                      "",
                                      catalog_file_values,
                                      false,
                                      nullptr,
                                      "",
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      catalog_offers_engine_items,
                                      draw_catalog_engine_items,
                                      "Add engine item...",
                                      "Adds an item the engine looks for by its id (an insertion point, a "
                                      "waypoint, a flag), on that id and of its kind.",
                                      "",
                                      false,
                                      draw_catalog_row_menu};
// A mission's rows as a tree (an event holding its triggers and its actions), a chip per kind of
// row (the four pools, the paths, the areas, the events), the empty paths left out; an event's
// triggers and actions added by type (S15: the "+" offers the types by name); under headings that
// read like the mission (S15: each pool, then its teams and groups where they tell rows apart; the
// paths, the areas, the events).
constexpr OutlineSpec kMissionOutline{OutlineMode::Tree, "", nullptr, true, mission_row_listed, "Empty paths",
                                      mission_adds_by_menu, draw_mission_add_menu, mission_row_headings,
                                      mission_row_reads_others};
// A string table's sections, each added by its name (one the table has selected, as a lookup reads the
// first section of a name), its strings moved to another section.
constexpr OutlineSpec kStringsOutline{OutlineMode::MasterDetail, "Sections", nullptr, false, nullptr, "", nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr, "", "", "name", true};
constexpr OutlineSpec kTreeOutline{OutlineMode::Tree, "", nullptr};
// An animation map's rows under their slots' families (S17: "Walking and running", "Deaths"), each
// row titled by its slot's words and the clips it plays.
constexpr OutlineSpec kAnimationMapOutline{OutlineMode::Tree, "", nullptr, false, nullptr, "", nullptr, nullptr,
                                           animation_map_row_headings};
std::unique_ptr<DocumentView> make_menu_view() { return std::make_unique<MenuView>(); }
std::unique_ptr<DocumentView> make_styles_view() { return std::make_unique<StylesView>(); }
std::unique_ptr<DocumentView> make_script_view() { return std::make_unique<ScriptView>(); }

// A view per document type, in DocumentTypeId's order: a catalog's rows as a list with its
// spawn registry after them, a string table's sections and strings as master and detail, a menu's
// screens and windows and a stylesheet's lines each their own view, a model, a clip and an
// animation table their records as a tree, and every text type its script device, the Main view
// (S13 V10; its lines read only where no device draws, S13 D9).
constexpr DocumentViewRow kViews[] = {
	// A weapon's Show on the HUD heads the Inspector (DI-20, ui/catalog_inspector_view).
	{DocumentTypeId::Catalog, DocumentViewRole::Records, &kCatalogOutline, nullptr, draw_catalog_inspector},
	{DocumentTypeId::Strings, DocumentViewRole::Records, &kStringsOutline, nullptr},
	// What a window plays and when heads its Inspector (DI-34, ui/menu_inspector).
	{DocumentTypeId::Menu, DocumentViewRole::Records, nullptr, make_menu_view, draw_menu_inspector},
	{DocumentTypeId::Styles, DocumentViewRole::Records, nullptr, make_styles_view},
	// A model's material heads the Inspector with its bullet faces' surface and flags, set on every face
	// made from it (S17, ui/model_inspector_view).
	{DocumentTypeId::Model, DocumentViewRole::Records, &kTreeOutline, nullptr, draw_model_inspector},
	// A clip's and a map's words head the Inspector (S17: what a row is for, who plays a map, a clip's
	// length and the rows that play it).
	{DocumentTypeId::Animation, DocumentViewRole::Records, &kTreeOutline, nullptr, draw_clip_inspector},
	{DocumentTypeId::AnimationMap, DocumentViewRole::Records, &kAnimationMapOutline, nullptr,
	 draw_animation_map_inspector},
	// A mission's 3D viewport fills the tab beside its outline (ADR 0046 S14: the Main role,
	// ui/main_viewport_view over the Mission viewport kind); its logic in words heads the Inspector,
	// what names a record closes it (S15, ui/mission_logic_view).
	{DocumentTypeId::Mission, DocumentViewRole::MainViewport, &kMissionOutline, nullptr, draw_mission_inspector,
	 draw_mission_uses, mission_logic_by_name},
	{DocumentTypeId::Script, DocumentViewRole::MainViewport, nullptr, make_script_view},
	{DocumentTypeId::MusicScript, DocumentViewRole::MainViewport, nullptr, make_script_view},
	{DocumentTypeId::Credits, DocumentViewRole::MainViewport, nullptr, make_script_view},
	{DocumentTypeId::Shader, DocumentViewRole::MainViewport, nullptr, make_script_view},
	{DocumentTypeId::Text, DocumentViewRole::MainViewport, nullptr, make_script_view},
	// A texture's picture fills the tab beside what it is and what uses it (S18, ui/texture_view).
	{DocumentTypeId::Texture, DocumentViewRole::MainViewport, nullptr, make_texture_view},
	// A bank's waves and sets as a tree (a set holding its layers, a layer its members), a profile's slots
	// under it; each heads the Inspector with what plays it and a Play (ui/sound_inspector).
	{DocumentTypeId::SoundBank, DocumentViewRole::Records, &kTreeOutline, nullptr, draw_sound_bank_inspector},
	{DocumentTypeId::SoundProfiles, DocumentViewRole::Records, &kTreeOutline, nullptr, draw_sound_profile_inspector},
	// A particle file's text in the script device; the Preview window plays its effect (DI-14).
	{DocumentTypeId::Particles, DocumentViewRole::MainViewport, nullptr, make_script_view},
	// An environment's row and its keyframes as a tree (DI-19a) beside its time-of-day viewport (DI-19b: the
	// Main role, ui/main_viewport_view over the Environment viewport kind); the missions that run on it head
	// the Inspector, each a Go to with its terrain and what its header sets over it (ui/environment_inspector).
	{DocumentTypeId::Environment, DocumentViewRole::MainViewport, &kTreeOutline, nullptr, draw_environment_inspector},
	// The HUD layout's text in its script device (DI-20), its HUD the Preview window's.
	{DocumentTypeId::HudLayout, DocumentViewRole::MainViewport, nullptr, make_script_view},
	// A terrain's row, its grid rows and its foliage definitions as a tree (DI-30); the import that makes it and
	// the missions that run on it head the Inspector (ui/terrain_inspector).
	{DocumentTypeId::Terrain, DocumentViewRole::Records, &kTreeOutline, nullptr, draw_terrain_inspector},
};

// One view per DocumentTypeId past None, in its order, each an outline or a view its make makes.
constexpr bool views_in_order() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		if (static_cast<size_t>(kViews[i].type) != i + 1 || (kViews[i].outline != nullptr) == (kViews[i].make != nullptr))
			return false;
	return true;
}

static_assert(sizeof(kViews) / sizeof(kViews[0]) == kDocumentTypeCount,
              "every DocumentTypeId has exactly one view");
static_assert(views_in_order(), "the views follow DocumentTypeId's order, each an outline or its own make");

} // namespace

const DocumentViewRow *document_view_row(DocumentTypeId type) {
	const size_t index = static_cast<size_t>(type);
	return index >= 1 && index <= kDocumentTypeCount ? &kViews[index - 1] : nullptr;
}

bool made_by_import(const SessionView &view, const DocumentBase &document) {
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(document.path()) : nullptr;
	return entry && !entry->imported_from.empty();
}

std::unique_ptr<DocumentView> make_view(const DocumentBase &document) {
	const DocumentTypeId type = asset_kind_row(document.kind()).document;
	const DocumentViewRow *row = document_view_row(type);
	if (!row) return nullptr;
	// The Main role: the outline beside the viewport of the Main-role kind that shows the type, or a
	// view of its own that draws it (a text's script view).
	const ViewportKind main = main_viewport_kind(type);
	if (row->role == DocumentViewRole::MainViewport && row->outline && main != ViewportKind::kCount)
		return std::make_unique<MainViewportView>(*row->outline, main);
	if (row->outline) return std::make_unique<OutlineView>(*row->outline);
	return row->make();
}

} // namespace opennova::editor
