#include "document_views.h"

#include <editor/assets/asset_kinds.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/model/document_base.h>
#include <editor/ui/menu_view.h>
#include <editor/ui/outline_view.h>
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

std::unique_ptr<DocumentView> make_catalog_view() {
	return std::make_unique<OutlineView>(OutlineSpec{OutlineMode::List, "", catalog_file_values});
}
std::unique_ptr<DocumentView> make_strings_view() {
	return std::make_unique<OutlineView>(OutlineSpec{OutlineMode::MasterDetail, "Sections", nullptr});
}
std::unique_ptr<DocumentView> make_menu_view() { return std::make_unique<MenuView>(); }
std::unique_ptr<DocumentView> make_styles_view() { return std::make_unique<StylesView>(); }
std::unique_ptr<DocumentView> make_tree_view() {
	return std::make_unique<OutlineView>(OutlineSpec{OutlineMode::Tree, "", nullptr});
}

// A view per document type, in DocumentTypeId's order: a catalog's rows as a list with its
// spawn registry after them, a string table's sections and strings as master and detail, a menu's
// screens and windows and a stylesheet's lines each their own view, a model, a clip and an
// animation table their records as a tree.
constexpr DocumentViewRow kViews[] = {
	{DocumentTypeId::Catalog, DocumentViewRole::Records, make_catalog_view},
	{DocumentTypeId::Strings, DocumentViewRole::Records, make_strings_view},
	{DocumentTypeId::Menu, DocumentViewRole::Records, make_menu_view},
	{DocumentTypeId::Styles, DocumentViewRole::Records, make_styles_view},
	{DocumentTypeId::Model, DocumentViewRole::Records, make_tree_view},
	{DocumentTypeId::Animation, DocumentViewRole::Records, make_tree_view},
	{DocumentTypeId::AnimationMap, DocumentViewRole::Records, make_tree_view},
};

// One view per DocumentTypeId past None, in its order, each made by its row.
constexpr bool views_in_order() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		if (static_cast<size_t>(kViews[i].type) != i + 1 || !kViews[i].make) return false;
	return true;
}

static_assert(sizeof(kViews) / sizeof(kViews[0]) == kDocumentTypeCount,
              "every DocumentTypeId has exactly one view");
static_assert(views_in_order(), "the views follow DocumentTypeId's order, each with its make");

} // namespace

const DocumentViewRow *document_view_row(DocumentTypeId type) {
	const size_t index = static_cast<size_t>(type);
	return index >= 1 && index <= kDocumentTypeCount ? &kViews[index - 1] : nullptr;
}

std::unique_ptr<DocumentView> make_view(const DocumentBase &document) {
	const DocumentViewRow *row = document_view_row(asset_kind_row(document.kind()).document);
	return row ? row->make() : nullptr;
}

} // namespace opennova::editor
