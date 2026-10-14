#pragma once

#include <string>
#include <vector>

#include <editor/model/finding_code_row.h>

namespace opennova::editor {

// The finding codes as the session asks about them (ADR 0046 S13 A6). The rows, the tables'
// shape and the editor's own table are model/finding_code_row.h's, each document type's table
// its own (DocumentType::findings); a finding keeps its row (Diagnostic::row), so nothing that
// holds a finding looks its code up. Here: every table with whose it is, and the columns' wire
// forms (the editor MCP's catalog, the Problems groups).

// A table and whose it is: "core", or its document type's name.
struct NamedFindingTable {
	const char *owner = "";
	FindingTable rows;
};
// Every table, the editor's own first, then each registered document type's in the registry's
// order (the registry's rows as registered, registered_document_type: a test's stand-in never hides a
// type's codes): every code the editor and the types know, as the editor MCP's catalog lists them.
std::vector<NamedFindingTable> finding_tables();

// The columns' wire forms: the fixes ("none", "requirement", "wrong_kind", "rename",
// "reset_row", "reference", "unimported_texture", "reload", "reimport", "rewrite"); the place
// ("content", "file"); the group's key, what the Problems groups are keyed by ("requirement",
// "requirement.optional_missing", "reference"...: the family every code of the group starts
// with), and its title ("Required files"); where a finding of the row comes from, a menu's rows
// by source ("graph", "render", else its group's key); whether a render check's note is a
// Problems row and at what severity ("none", "info", "warning").
const char *finding_fix_token(FindingFix fixes);
const char *finding_place_token(FindingPlace place);
const char *finding_group_key(FindingGroup group);
const char *finding_group_title(FindingGroup group);
const char *finding_source_token(const FindingCodeRow &row);
const char *finding_problem_token(FindingProblem problem);

} // namespace opennova::editor
