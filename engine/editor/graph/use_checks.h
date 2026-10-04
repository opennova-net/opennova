#pragma once

#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

class AssetGraph;
class ValidationCache;
struct ValidationInput;

// What other files make of what a file defines (ADR 0046 S13 D4): the findings about a file that
// its own validation (DocumentType::validate_file, over its document alone) cannot make, since
// they read the whole project through the asset graph: what the menus use a stylesheet's
// variables as; what each texture reference's role asks of the file its loader opens (S18). One row
// per asset kind with such a check, in AssetKind's order (kUseChecks, use_checks.cpp); a new
// cross-file check is one row. Each reads the graph as its last update left it (its symbols carry a
// definition's record, line and value) and, of the files the validation asked, only those whose own
// checks read their records (ValidationCache::records_checked: a file that did not load, or that a
// source error blocks, reports that alone), and the project's files as the validation's input lists
// them (the texture check reads its files' headers, each again only once its stamp moves), and adds
// its findings in the files' order.
using UseCheck = void (*)(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
		std::vector<Diagnostic> &out);

struct UseCheckRow {
	AssetKind kind = AssetKind::Unknown;
	UseCheck check = nullptr;
};

// The row of a kind; null for a kind with no use check.
const UseCheckRow *use_check(AssetKind kind);
// Every row's check, in the table's order.
void run_use_checks(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
		std::vector<Diagnostic> &out);

} // namespace opennova::editor
