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

const DocumentType kTypes[] = {
	{"catalog", is_catalog_kind, make_catalog, validate_catalogs},
	{"strings", is_strings_kind, make_strings, validate_strings},
	{"menu", is_menu_kind, make_menu, validate_menus},
	{"styles", is_style_kind, make_styles, validate_styles},
	{"model", is_model_kind, make_model, validate_models},
	{"animation", is_animation_kind, make_animation, validate_animations},
	{"animation_map", is_animation_map_kind, make_animation_map, validate_animation_maps},
};

} // namespace

const DocumentType *document_type_for(AssetKind kind) {
	for (const DocumentType &type : kTypes)
		if (type.handles(kind)) return &type;
	return nullptr;
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
