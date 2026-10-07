#include <editor/graph/jump_queries.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <editor/graph/reference_kinds.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

// How a hit holds the text (lower ranks first): its name the text itself (a file's name also without its
// extension), a name the text starts, a name holding it anywhere, then a hit found by its words, its record's
// title or a record naming it.
int hit_rank(const GraphSearchHit &hit, const std::string &wanted) {
	const std::string name = strutil::to_upper(hit.name);
	const size_t dot = name.find_last_of('.');
	const std::string stem = hit.symbol || dot == std::string::npos ? name : name.substr(0, dot);
	if (name == wanted || stem == wanted) return 0;
	if (name.rfind(wanted, 0) == 0) return 1;
	if (name.find(wanted) != std::string::npos) return 2;
	return 3;
}

// The symbol a record at `locator` of `file` defines first (a document record by its locator, a native file's
// by its path; else any record by its path, as a finding names it), null for none.
const GraphSymbol *symbol_at_locator(const AssetGraph &graph, const std::string &file, const std::string &locator) {
	const std::vector<AssetGraph::FileSymbol> defined = graph.symbols_in(file);
	const GraphSymbol *by_path = nullptr;
	for (const AssetGraph::FileSymbol &each : defined) {
		const GraphSymbol &symbol = *each.symbol;
		// A record of a record set goes by its index in its own file: no name the other files use.
		if (reference_row(symbol.kind).resolution == ReferenceResolution::Record) continue;
		if ((symbol.locator.empty() ? symbol.record : symbol.locator) == locator) return &symbol;
		if (!by_path && !symbol.record.empty() && symbol.record == locator) by_path = &symbol;
	}
	return by_path;
}

} // namespace

const char *search_scope_token(SearchScope scope) {
	switch (scope) {
	case SearchScope::All: return "all";
	case SearchScope::Files: return "files";
	case SearchScope::Names: return "names";
	}
	return "all";
}

bool search_scope_from_token(const std::string &token, SearchScope &out) {
	for (const SearchScope scope : {SearchScope::All, SearchScope::Files, SearchScope::Names})
		if (token == search_scope_token(scope)) {
			out = scope;
			return true;
		}
	return false;
}

std::vector<GraphSearchHit> search_project(const AssetGraph &graph, const std::string &text, SearchScope scope) {
	std::vector<GraphSearchHit> hits = graph.search(text);
	if (scope == SearchScope::All) return hits;
	const bool files = scope == SearchScope::Files;
	hits.erase(std::remove_if(hits.begin(), hits.end(),
	                          [files](const GraphSearchHit &hit) { return (hit.symbol == nullptr) != files; }),
	           hits.end());
	const std::string wanted = strutil::to_upper(text);
	std::vector<std::pair<int, size_t>> order;
	order.reserve(hits.size());
	for (size_t i = 0; i < hits.size(); ++i) order.emplace_back(hit_rank(hits[i], wanted), i);
	std::stable_sort(order.begin(), order.end(),
	                 [](const auto &a, const auto &b) { return a.first < b.first; });
	std::vector<GraphSearchHit> out;
	out.reserve(hits.size());
	for (const auto &[rank, index] : order) out.push_back(std::move(hits[index]));
	return out;
}

std::vector<const GraphEdge *> usages_at(const AssetGraph &graph, const std::string &file, const std::string &locator) {
	if (locator.empty()) return graph.usages_of(file);
	const GraphSymbol *symbol = symbol_at_locator(graph, file, locator);
	if (!symbol) return {};
	return record_users(graph, file, symbol->record, symbol->address);
}

std::string usages_subject_words(const AssetGraph &graph, const std::string &file, const std::string &locator) {
	const std::string name = basename_of(file);
	if (locator.empty()) return name;
	if (const GraphSymbol *symbol = symbol_at_locator(graph, file, locator))
		return std::string(reference_row(symbol->kind).label) + " " + symbol->display + " in " + name;
	return locator + " in " + name;
}

bool record_definition(const AssetGraph &graph, const AssetScan &scan, const Document &document, const NodeAddress &record,
                       std::vector<ReferenceTarget> &out) {
	out.clear();
	if (!record.row) return false;
	for (const FieldSchema &schema : document.fields(record.kind)) {
		if (schema.optional && !document.present(record, schema.id)) continue;
		Value value;
		if (!document.get(record, schema.id, value)) continue;
		const FieldUse field = document.field_on(record, schema);
		if (field.applies == Applicability::Ignored || value_reference(field, value) == ReferenceKind::None) continue;
		if (reference_status(graph, field, value) != ReferenceStatus::Present) continue;
		out = reference_targets(graph, scan, field, value);
		if (!out.empty()) return true;
	}
	return false;
}

} // namespace opennova::editor
