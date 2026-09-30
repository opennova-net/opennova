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
	{DocumentTypeId::Catalog, "catalog", make_catalog, validate_catalogs},
	{DocumentTypeId::Strings, "strings", make_strings, validate_strings},
	{DocumentTypeId::Menu, "menu", make_menu, validate_menus},
	{DocumentTypeId::Styles, "styles", make_styles, validate_styles},
	{DocumentTypeId::Model, "model", make_model, validate_models},
	{DocumentTypeId::Animation, "animation", make_animation, validate_animations},
	{DocumentTypeId::AnimationMap, "animation_map", make_animation_map, validate_animation_maps},
};

// One type per DocumentTypeId past None, in its order.
constexpr bool types_in_order() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		if (static_cast<size_t>(kTypes[i].id) != i + 1) return false;
	return true;
}

static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == kDocumentTypeCount,
              "every DocumentTypeId has exactly one type");
static_assert(types_in_order(), "the types follow DocumentTypeId's order");

} // namespace

const DocumentType *document_type(DocumentTypeId id) {
	const size_t index = static_cast<size_t>(id);
	return index >= 1 && index <= kDocumentTypeCount ? &kTypes[index - 1] : nullptr;
}

const DocumentType *document_type_for(AssetKind kind) {
	return document_type(asset_kind_row(kind).document);
}

bool is_editable_kind(AssetKind kind) { return document_type_for(kind) != nullptr; }

std::vector<Diagnostic> validate_open_documents(const ProjectPaths &paths, const ProjectDocument &project,
                                                const AssetScan &scan,
                                                const std::vector<std::shared_ptr<const Document>> &open,
                                                AssetGraph *graph, ValidationCache *cache) {
	std::vector<Diagnostic> findings;
	// The graph first: a type's validation reads what its records are used for (a
	// stylesheet variable's uses).
	AssetGraph local;
	AssetGraph &resolved = graph ? *graph : local;
	resolved.update(paths, project, scan, open);
	ValidationCache local_cache;
	ValidationCache &files = cache ? *cache : local_cache;
	files.begin();
	const ValidationInput input{paths, project, scan, open, files};
	for (const DocumentType &type : kTypes)
		for (const Diagnostic &d : type.validate(input, resolved)) findings.push_back(d);
	files.end();
	for (const Diagnostic &d : resolved.diagnostics()) findings.push_back(d);
	return findings;
}

} // namespace opennova::editor
