#pragma once

// What the query handlers share beyond the table (session/editor_queries.h): the paging docs every paged
// query's params read, a query's page, and the open record document and record its args name. The table's
// handlers in editor_queries.cpp and those of a domain's own file (session/mission_queries.cpp, S24) read
// these alike.

#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/document.h>
#include <editor/model/edit.h>
#include <editor/session/editor_queries.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

inline constexpr const char *kQueryOffsetDoc = "The page's first entry, by its place in the list (0 the first).";
inline constexpr const char *kQueryLimitDoc = "How many entries the page holds at most, 1 to 200.";

// A paged query's page: from `offset`, `limit` entries at most.
JsonPage page_of(const QueryArgs &args);
// The open record document `args` names by its `path` ("" the active one), or null with `error`: none is
// open there, or the one open is of another kind, which holds no records (S13 D6).
const Document *document_of(const QueryContext &context, const QueryArgs &args, std::string &error);
// A record of `document` by the identity `args` names (`id`), or false with `error`.
bool record_of(const Document &document, const QueryArgs &args, NodeAddress &out, std::string &error);
// A record's identity: a nested record's own, else its row's.
NodeId identity_of(const NodeAddress &address);
// The edits an op planned in the batch form an edit_record takes back ({edits}), or the refusal that says why
// none ({edits: [], refusal}).
io::JsonValue planned_edits(const Document &document, bool ok, const std::vector<Edit> &edits, const std::string &refusal);

} // namespace opennova::editor
