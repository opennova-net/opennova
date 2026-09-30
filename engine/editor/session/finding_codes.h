#pragma once

#include <string>
#include <vector>

#include <editor/model/finding_code_row.h>

namespace opennova::editor {

// The finding codes as the session asks about them (ADR 0046 S13 A6). The rows, the tables'
// shape and the editor's own table are model/finding_code_row.h's, each document type's table
// its own (DocumentType::findings); a finding keeps its row (Diagnostic::row), so nothing that
// holds a finding looks its code up. Here: the lookup by a token over every table, every table
// with whose it is, and the columns' wire forms (the editor MCP's catalog, the Problems groups).

// The row of a token: the editor's own, else a registered document type's (the registry's rows as
// registered, registered_document_type: a test's stand-in never hides a type's codes); null for a
// token no table declares. One map over every table, made once.
const FindingCodeRow *finding_row(const std::string &token);

// A table and whose it is: "core", or its document type's name.
struct NamedFindingTable {
	const char *owner = "";
	FindingTable rows;
};
// Every table, the editor's own first, then each registered document type's in the registry's
// order: every code the editor and the types know, as the editor MCP's catalog lists them.
std::vector<NamedFindingTable> finding_tables();
// Whose table holds a row (by address): "core" or the type's name; null for a row no table holds.
const char *finding_owner(const FindingCodeRow *row);

// The columns' wire forms: the fixes ("none", "requirement", "wrong_kind", "rename",
// "reset_row", "reference", "unimported_texture", "reload", "reimport", "rewrite"); the place
// ("content", "file"); the group's key, what the Problems groups are keyed by ("requirement",
// "requirement.optional_missing", "reference"...: the family every code of the group starts
// with), and its title ("Required files"); where a finding of the row comes from, a menu's rows
// by source ("graph", "render", else its group's key).
const char *finding_fix_token(FindingFix fixes);
const char *finding_place_token(FindingPlace place);
const char *finding_group_key(FindingGroup group);
const char *finding_group_title(FindingGroup group);
const char *finding_source_token(const FindingCodeRow &row);

} // namespace opennova::editor
