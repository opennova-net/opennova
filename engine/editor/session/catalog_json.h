#pragma once

#include <vector>

#include <base/io/json.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The editor's catalog (the catalog query, editor_queries.cpp), the parts of it that are no query's:
// what the editor MCP makes its schemas from.
//
// The request side: each request kind with the fields it takes and needs and who serves it, every
// request field with its JSON type, the batch form of a request's edits (record_batch.h: its forms,
// ops and members) and what the windows show of their own (workspace_parts.h: each part, its members
// and the windows its focus brings forward); set on `out` as requests, fields, batch and workspace.
void request_catalog_json(io::JsonValue &out);

// The view side: the view's sections with their concerns, the concerns, and every finding code the
// session and the types know (S13 A6) with what Problems does with a finding of it and how many of
// `diagnostics` (the Problems rows) carry it; set on `out` as sections, concerns and finding_codes.
void view_catalog_json(io::JsonValue &out, const std::vector<Diagnostic> &diagnostics);

} // namespace opennova::editor
