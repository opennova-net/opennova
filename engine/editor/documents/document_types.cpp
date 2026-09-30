#include "document_types.h"

#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/strings_document.h>

namespace opennova::editor {
namespace {

std::unique_ptr<Document> make_catalog() { return std::make_unique<DefCatalogDocument>(); }
std::unique_ptr<Document> make_strings() { return std::make_unique<StringsDocument>(); }
std::unique_ptr<Document> make_menu() { return std::make_unique<MnuDocument>(); }
std::unique_ptr<Document> make_styles() { return std::make_unique<MnsDocument>(); }
std::unique_ptr<Document> make_model() { return std::make_unique<ModelDocument>(); }
std::unique_ptr<Document> make_animation() { return std::make_unique<AnimationDocument>(); }
std::unique_ptr<Document> make_animation_map() { return std::make_unique<AnimationMapDocument>(); }

constexpr DocumentType kTypes[] = {
	{ DocumentTypeId::Catalog, "catalog", make_catalog, validate_catalog_file },
	{ DocumentTypeId::Strings, "strings", make_strings, validate_strings_file },
	{ DocumentTypeId::Menu, "menu", make_menu, validate_menu_file },
	{ DocumentTypeId::Styles, "styles", make_styles, validate_styles_file },
	{ DocumentTypeId::Model, "model", make_model, validate_model_file },
	{ DocumentTypeId::Animation, "animation", make_animation, validate_animation_file },
	{ DocumentTypeId::AnimationMap, "animation_map", make_animation_map,
			validate_animation_map_file },
};

// One type per DocumentTypeId past None, in its order, each making its documents and validating
// its files.
constexpr bool types_in_order() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		if (static_cast<size_t>(kTypes[i].id) != i + 1 || !kTypes[i].make ||
				!kTypes[i].validate_file)
			return false;
	return true;
}

static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == kDocumentTypeCount,
              "every DocumentTypeId has exactly one type");
static_assert(types_in_order(),
		"the types follow DocumentTypeId's order, each with its make and validate_file");

} // namespace

const DocumentType *document_type(DocumentTypeId id) {
	const size_t index = static_cast<size_t>(id);
	return index >= 1 && index <= kDocumentTypeCount ? &kTypes[index - 1] : nullptr;
}

const DocumentType *document_type_for(AssetKind kind) {
	return document_type(asset_kind_row(kind).document);
}

bool is_editable_kind(AssetKind kind) { return document_type_for(kind) != nullptr; }

} // namespace opennova::editor
