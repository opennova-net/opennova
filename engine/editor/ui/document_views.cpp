#include "document_views.h"

#include <editor/assets/asset_kinds.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_table.h>
#include <editor/model/document_base.h>
#include <editor/ui/animation_inspector.h>
#include <editor/ui/main_viewport_view.h>
#include <editor/ui/menu_view.h>
#include <editor/ui/mission_logic_view.h>
#include <editor/ui/model_inspector_view.h>
#include <editor/ui/outline_view.h>
#include <editor/ui/script_view.h>
#include <editor/ui/styles_view.h>

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

// A mission's 128 waypoint paths are the file's own, most of them empty: one with no stop is not
// listed until the outline's switch lists them.
bool mission_row_listed(const Document &, const Node &row) {
	return row.kind != node_kind(MissionKind::WaypointPath) || (!row.collections.empty() && !row.collections[0].empty());
}

constexpr OutlineSpec kCatalogOutline{OutlineMode::List, "", catalog_file_values};
// A mission's rows as a tree (an event holding its triggers and its actions), a chip per kind of
// row (the four pools, the paths, the areas, the events), the empty paths left out; an event's
// triggers and actions added by type (S15: the "+" offers the types by name); under headings that
// read like the mission (S15: each pool, then its teams and groups where they tell rows apart; the
// paths, the areas, the events).
constexpr OutlineSpec kMissionOutline{OutlineMode::Tree, "", nullptr, true, mission_row_listed, "Empty paths",
                                      mission_adds_by_menu, draw_mission_add_menu, mission_row_headings,
                                      mission_row_reads_others};
constexpr OutlineSpec kStringsOutline{OutlineMode::MasterDetail, "Sections", nullptr};
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
	{DocumentTypeId::Catalog, DocumentViewRole::Records, &kCatalogOutline, nullptr},
	{DocumentTypeId::Strings, DocumentViewRole::Records, &kStringsOutline, nullptr},
	{DocumentTypeId::Menu, DocumentViewRole::Records, nullptr, make_menu_view},
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
