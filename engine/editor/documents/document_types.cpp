#include "document_types.h"

#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/strings_document.h>
// The menu type's project check, by its hook alone: the render check runs the preview's headless
// screen compile (MenuScreenRender), so it sits with it in preview/ (ADR 0046 S13 V9).
#include <editor/preview/make_menu_render_check.h>

#include <array>
#include <atomic>

namespace opennova::editor {
namespace {

std::unique_ptr<DocumentBase> make_catalog() { return std::make_unique<DefCatalogDocument>(); }
std::unique_ptr<DocumentBase> make_strings() { return std::make_unique<StringsDocument>(); }
std::unique_ptr<DocumentBase> make_menu() { return std::make_unique<MnuDocument>(); }
std::unique_ptr<DocumentBase> make_styles() { return std::make_unique<MnsDocument>(); }
std::unique_ptr<DocumentBase> make_model() { return std::make_unique<ModelDocument>(); }
std::unique_ptr<DocumentBase> make_animation() { return std::make_unique<AnimationDocument>(); }
std::unique_ptr<DocumentBase> make_animation_map() {
	return std::make_unique<AnimationMapDocument>();
}

constexpr DocumentType kTypes[] = {
	{ DocumentTypeId::Catalog, "catalog", make_catalog, validate_catalog_file,
			DefCatalogDocument::schema, catalog_finding_codes },
	{ DocumentTypeId::Strings, "strings", make_strings, validate_strings_file,
			StringsDocument::schema, strings_finding_codes },
	{ DocumentTypeId::Menu, "menu", make_menu, validate_menu_file, MnuDocument::schema,
			menu_finding_codes, make_menu_render_check },
	{ DocumentTypeId::Styles, "styles", make_styles, validate_styles_file, MnsDocument::schema,
			style_finding_codes },
	{ DocumentTypeId::Model, "model", make_model, validate_model_file, ModelDocument::schema,
			model_finding_codes },
	{ DocumentTypeId::Animation, "animation", make_animation, validate_animation_file,
			AnimationDocument::schema, animation_finding_codes },
	{ DocumentTypeId::AnimationMap, "animation_map", make_animation_map,
			validate_animation_map_file, AnimationMapDocument::schema,
			animation_map_finding_codes },
};

// One type per DocumentTypeId past None, in its order, each making its documents, validating its
// files, naming its records' fields and declaring its finding codes.
constexpr bool types_in_order() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		if (static_cast<size_t>(kTypes[i].id) != i + 1 || !kTypes[i].make ||
				!kTypes[i].validate_file || !kTypes[i].fields || !kTypes[i].findings)
			return false;
	return true;
}

// A type may have no project check; one it has is its own: two rows naming the same would make
// two instances of it, each over the whole project, and list its findings twice.
constexpr bool project_checks_own() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		for (size_t j = i + 1; j < kDocumentTypeCount; ++j)
			if (kTypes[i].project_check && kTypes[i].project_check == kTypes[j].project_check)
				return false;
	return true;
}

static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == kDocumentTypeCount,
              "every DocumentTypeId has exactly one type");
static_assert(types_in_order(),
		"the types follow DocumentTypeId's order, each with its make, validate_file, fields and "
		"findings");
static_assert(project_checks_own(),
		"a type's project check is its own (a type may have none): no two rows name the same");

// A test's type in a registered one's place (DocumentTypeStandIn), null for none.
std::atomic<const DocumentType *> g_stand_in{nullptr};

// Whether a type's documents are record documents: asked of one it makes.
bool makes_records(const DocumentType &type) {
	return type.make && records_of(type.make()) != nullptr;
}

} // namespace

const DocumentType *document_type(DocumentTypeId id) {
	if (const DocumentType *stand_in = g_stand_in.load(); stand_in && stand_in->id == id)
		return stand_in;
	return registered_document_type(id);
}

const DocumentType *registered_document_type(DocumentTypeId id) {
	const size_t index = static_cast<size_t>(id);
	return index >= 1 && index <= kDocumentTypeCount ? &kTypes[index - 1] : nullptr;
}

const DocumentType *document_type_for(AssetKind kind) {
	return document_type(asset_kind_row(kind).document);
}

bool is_editable_kind(AssetKind kind) { return document_type_for(kind) != nullptr; }

bool holds_records(const DocumentType &type) {
	// A registered type is asked once; a stand-in (a test's) each time.
	const size_t index = static_cast<size_t>(type.id);
	if (index < 1 || index > kDocumentTypeCount || &type != &kTypes[index - 1])
		return makes_records(type);
	static const std::array<bool, kDocumentTypeCount> answers = [] {
		std::array<bool, kDocumentTypeCount> out{};
		for (size_t i = 0; i < kDocumentTypeCount; ++i) out[i] = makes_records(kTypes[i]);
		return out;
	}();
	return answers[index - 1];
}

DocumentTypeStandIn::DocumentTypeStandIn(const DocumentType &type) { g_stand_in.store(&type); }

DocumentTypeStandIn::~DocumentTypeStandIn() { g_stand_in.store(nullptr); }

} // namespace opennova::editor
