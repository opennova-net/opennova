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
// document (a DocumentBase: a record type's is its Document, as_records) and how to validate one
// file of the kinds it opens, which are the asset kinds whose row names its id
// (AssetKindRow::document). The session, the windows and the shell reach a document type only
// through this table.
struct DocumentType {
	DocumentTypeId id = DocumentTypeId::None;
	const char *name = "";
	std::unique_ptr<DocumentBase> (*make)() = nullptr;
	// One file's own findings (ADR 0046 S13 D4), from its document alone, an open one standing in
	// for its file: what the game makes of what the file holds, on the records and fields that
	// cause it (a record type reads its records through its own document, as_records). What other
	// files make of what it defines is a use check's (graph/use_checks.h) and what it references
	// is the asset graph's; graph/project_validation.h runs the three over the project, the
	// validation cache keeping each file's findings until the file changes.
	std::vector<Diagnostic> (*validate_file)(const DocumentBase &document) = nullptr;
};

// The type its row names (null for DocumentTypeId::None); the type that opens a kind (null for a
// kind the build packs as it is).
const DocumentType *document_type(DocumentTypeId id);
const DocumentType *document_type_for(AssetKind kind);
bool is_editable_kind(AssetKind kind);
// Whether the documents a type makes are record documents (DocumentBase::as_records): what the
// graph's extraction, the validators and the rename read. A type of another kind (a raster, a
// text) contributes nothing to them until its own hooks (S13 D9). Asked of a document the type
// makes, once per registered type.
bool holds_records(const DocumentType &type);

// A test's document type in a registered one's place (S13 D6: a type of another kind than
// records, before one ships): while it lives, document_type answers it for its id, and so
// document_type_for for every asset kind whose row names that id. The registry's one seam:
// nothing but a test makes one, one at a time.
class DocumentTypeStandIn {
public:
	explicit DocumentTypeStandIn(const DocumentType &type);
	~DocumentTypeStandIn();
	DocumentTypeStandIn(const DocumentTypeStandIn &) = delete;
	DocumentTypeStandIn &operator=(const DocumentTypeStandIn &) = delete;
};

} // namespace opennova::editor
