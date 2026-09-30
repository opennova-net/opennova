#pragma once

#include <memory>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// The registry of editable file kinds (ADR 0046 d9): one row per document type, one per
// DocumentTypeId past None in its order (a static_assert checks it), saying how to make a
// document (a DocumentBase: a record type's is its Document, as_records) and how to validate the
// project's files of the kinds it opens, which are the asset kinds whose row names its id
// (AssetKindRow::document). The session, the windows and the shell reach a document type only
// through this table.
struct DocumentType {
	DocumentTypeId id = DocumentTypeId::None;
	const char *name = "";
	std::unique_ptr<DocumentBase> (*make)() = nullptr;
	// The type's findings over the project's files of its kinds, with the project's asset graph
	// (updated first: a type reads what its records are used for).
	std::vector<Diagnostic> (*validate)(const ValidationInput &input, const AssetGraph &graph) = nullptr;
};

// The type its row names (null for DocumentTypeId::None); the type that opens a kind (null for a
// kind the build packs as it is).
const DocumentType *document_type(DocumentTypeId id);
const DocumentType *document_type_for(AssetKind kind);
bool is_editable_kind(AssetKind kind);

// Every type's validation over the project, with the open documents standing in for
// their files, then the asset graph's findings (a missing reference). `graph`, when
// given, is updated in place and kept (the session's); else one is built for the call.
// `cache`, when given, keeps the closed files it read for the next call (the
// session's); else they are read for this call alone (the command line, once).
std::vector<Diagnostic> validate_open_documents(const ProjectPaths &paths, const ProjectDocument &project,
                                                const AssetScan &scan,
                                                const std::vector<std::shared_ptr<const DocumentBase>> &open,
                                                AssetGraph *graph = nullptr, ValidationCache *cache = nullptr);

} // namespace opennova::editor
