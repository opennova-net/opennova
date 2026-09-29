#pragma once

#include <memory>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// The registry of editable file kinds (ADR 0046 d9): one row per document type,
// naming the asset kinds it opens, how to make a document and how to validate the
// project's files of those kinds. The session, the windows and the shell reach a
// document type only through this table.
struct DocumentType {
	const char *name = "";
	bool (*handles)(AssetKind kind) = nullptr;
	std::unique_ptr<Document> (*make)() = nullptr;
	// The type's findings over the project's files of its kinds, with the project's asset graph
	// (updated first: a type reads what its records are used for).
	std::vector<Diagnostic> (*validate)(const ValidationInput &input, const AssetGraph &graph) = nullptr;
};

const DocumentType *document_type_for(AssetKind kind);
bool is_editable_kind(AssetKind kind);

// Every type's validation over the project, with the open documents standing in for
// their files, then the asset graph's findings (a missing reference). `graph`, when
// given, is updated in place and kept (the session's); else one is built for the call.
// `cache`, when given, keeps the closed files it read for the next call (the
// session's); else they are read for this call alone (the command line, once).
std::vector<Diagnostic> validate_open_documents(const ProjectPaths &paths, const ProjectDocument &project,
                                                const AssetScan &scan,
                                                const std::vector<std::shared_ptr<const Document>> &open,
                                                AssetGraph *graph = nullptr, ValidationCache *cache = nullptr);

} // namespace opennova::editor
