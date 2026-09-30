#pragma once

#include <memory>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>

namespace opennova::editor {

// The registry of editable file kinds (ADR 0046 d9): one row per document type, one per
// DocumentTypeId past None in its order (a static_assert checks it), saying how to make a
// document and how to validate one file of the kinds it opens, which are the asset kinds whose
// row names its id (AssetKindRow::document). The session, the windows and the shell reach a
// document type only through this table.
struct DocumentType {
	DocumentTypeId id = DocumentTypeId::None;
	const char *name = "";
	std::unique_ptr<Document> (*make)() = nullptr;
	// One file's own findings (ADR 0046 S13 D4), from its document alone, an open one standing in
	// for its file: what the game makes of what the file holds, on the records and fields that
	// cause it. What other files make of what it defines is a use check's (graph/use_checks.h) and
	// what it references is the asset graph's; graph/project_validation.h runs the three over the
	// project, the validation cache keeping each file's findings until the file changes.
	std::vector<Diagnostic> (*validate_file)(const Document &document) = nullptr;
};

// The type its row names (null for DocumentTypeId::None); the type that opens a kind (null for a
// kind the build packs as it is).
const DocumentType *document_type(DocumentTypeId id);
const DocumentType *document_type_for(AssetKind kind);
bool is_editable_kind(AssetKind kind);

} // namespace opennova::editor
