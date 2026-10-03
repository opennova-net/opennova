#include <editor/session/problem_query.h>

#include <initializer_list>
#include <map>
#include <memory>
#include <optional>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/graph/display_names.h>
#include <editor/model/field_text.h>
#include <editor/session/finding_codes.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// The group a finding shows under when Problems groups by kind: its row's (FindingGroup, its key
// and title session/finding_codes.h's); none for a Diagnostic no finding was made into.
FindingGroup group_of(const Diagnostic &d) {
	return d.row() ? d.row()->group : FindingGroup::None;
}

void tally(DiagnosticSeverity severity, size_t &errors, size_t &warnings, size_t &infos) {
	switch (severity) {
	case DiagnosticSeverity::Error: ++errors; break;
	case DiagnosticSeverity::Warning: ++warnings; break;
	case DiagnosticSeverity::Info: ++infos; break;
	}
}

bool matches_text(const Diagnostic &d, const std::string &needle) {
	if (needle.empty()) return true;
	for (const std::string *member : {&d.message, &d.asset, &d.record, &d.field, &d.code()})
		if (strutil::to_lower(*member).find(needle) != std::string::npos) return true;
	return false;
}

// A finding about a file as a whole in the project, which Files shows and renames: its row places
// it there (FindingPlace::File: its name, which does not fit the archives or another file has; its
// place, an archive the build does not pack).
bool about_the_file(const Diagnostic &d) {
	return d.row() && d.row()->place == FindingPlace::File;
}

bool in_scope(const Diagnostic &d, ProblemScope scope, const SessionView &view) {
	switch (scope) {
	case ProblemScope::Project: return true;
	case ProblemScope::ActiveFile: return !d.asset.empty() && d.asset == view.documents.active;
	case ProblemScope::OpenFiles:
		for (const auto &document : view.documents.open)
			if (document && document->path() == d.asset) return true;
		return false;
	}
	return true;
}

} // namespace

bool ProblemQuery::shows(DiagnosticSeverity severity) const {
	switch (severity) {
	case DiagnosticSeverity::Error: return errors;
	case DiagnosticSeverity::Warning: return warnings;
	case DiagnosticSeverity::Info: return infos;
	}
	return true;
}

bool ProblemQuery::operator==(const ProblemQuery &other) const {
	return errors == other.errors && warnings == other.warnings && infos == other.infos && text == other.text &&
	       scope == other.scope && fixable == other.fixable && grouping == other.grouping;
}

bool in_original_data(const Diagnostic &diagnostic, const SessionView &view) {
	const auto &files = view.findings.original_files;
	return files && !diagnostic.asset.empty() && files->count(diagnostic.asset) != 0;
}

namespace {

// The game's own data's findings after the modder's (S15): their group last, the modder's before it
// as they are grouped or, ungrouped, as one group with no header.
void place_original(ProblemAnswer &answer, std::vector<size_t> original, const SessionView &view) {
	if (original.empty()) return;
	if (!answer.grouped) {
		answer.grouped = true;
		if (!answer.rows.empty()) {
			ProblemGroup mine;
			mine.header = false;
			mine.rows = answer.rows;
			for (const size_t i : mine.rows) tally(view.findings.diagnostics[i].severity, mine.errors, mine.warnings, mine.infos);
			answer.groups.push_back(std::move(mine));
		}
	}
	ProblemGroup group;
	group.key = kOriginalGroupKey;
	group.title = kOriginalGroupTitle;
	group.original = true;
	for (const size_t i : original) tally(view.findings.diagnostics[i].severity, group.errors, group.warnings, group.infos);
	answer.rows.insert(answer.rows.end(), original.begin(), original.end());
	group.rows = std::move(original);
	answer.groups.push_back(std::move(group));
}

} // namespace

ProblemAnswer answer_problems(const ProblemQuery &query, const SessionView &view) {
	ProblemAnswer answer;
	answer.grouped = query.grouping != ProblemGrouping::None;
	const std::vector<Diagnostic> &findings = view.findings.diagnostics;
	for (const Diagnostic &d : findings) {
		if (in_original_data(d, view)) tally(d.severity, answer.original_errors, answer.original_warnings, answer.original_infos);
		else tally(d.severity, answer.errors, answer.warnings, answer.infos);
	}
	const std::string needle = strutil::to_lower(query.text);
	// Only the fixable: the view's findings read once for all of them, not once per finding.
	std::unique_ptr<ProblemFixIndex> index;
	if (query.fixable) index = std::make_unique<ProblemFixIndex>(view);
	std::vector<size_t> original;
	for (const DiagnosticSeverity severity :
	     {DiagnosticSeverity::Error, DiagnosticSeverity::Warning, DiagnosticSeverity::Info}) {
		if (!query.shows(severity)) continue;
		for (size_t i = 0; i < findings.size(); ++i) {
			const Diagnostic &d = findings[i];
			if (d.severity != severity || !in_scope(d, query.scope, view) || !matches_text(d, needle)) continue;
			if (index && !has_fixes(d, view, index.get())) continue;
			(in_original_data(d, view) ? original : answer.rows).push_back(i);
		}
	}
	if (!answer.grouped) {
		place_original(answer, std::move(original), view);
		return answer;
	}
	std::map<std::string, size_t> placed; // a key -> its group
	for (const size_t i : answer.rows) {
		const Diagnostic &d = findings[i];
		const std::string key = query.grouping == ProblemGrouping::File ? d.asset : finding_group_key(group_of(d));
		auto found = placed.find(key);
		if (found == placed.end()) {
			ProblemGroup group;
			group.key = key;
			group.title = query.grouping == ProblemGrouping::Kind ? finding_group_title(group_of(d))
			              : key.empty()                           ? std::string("Project")
			                                                      : key;
			found = placed.emplace(key, answer.groups.size()).first;
			answer.groups.push_back(std::move(group));
		}
		ProblemGroup &group = answer.groups[found->second];
		group.rows.push_back(i);
		tally(d.severity, group.errors, group.warnings, group.infos);
	}
	answer.rows.clear();
	for (const ProblemGroup &group : answer.groups) answer.rows.insert(answer.rows.end(), group.rows.begin(), group.rows.end());
	place_original(answer, std::move(original), view);
	return answer;
}

RevisionKey problem_query_key(const SessionView &view, const ProblemQuery &query) {
	RevisionKey key = revision_key(view.revisions, {ViewConcern::Findings});
	if (query.scope == ProblemScope::ActiveFile)
		key = key | revision_key(view.revisions, {ViewConcern::ActiveDocument});
	if (query.scope == ProblemScope::OpenFiles)
		key = key | revision_key(view.revisions, {ViewConcern::DocumentSet});
	if (query.fixable) key = key | problem_fix_key(view);
	return key;
}

const ProblemAnswer &ProblemQueryCache::answer(const ProblemQuery &query, const SessionView &view) {
	const RevisionKey key = problem_query_key(view, query);
	if (view_ != &view || key_ != key || query_ != query) {
		answer_ = answer_problems(query, view);
		view_ = &view;
		key_ = key;
		query_ = query;
		++generation_;
	}
	return answer_;
}

ProblemLocation problem_location(const Diagnostic &diagnostic, const SessionView &view) {
	ProblemLocation location;
	const AssetEntry *entry = view.project.scan->at_path(diagnostic.asset);
	if (!entry) return location;
	location.path = entry->relative_path;
	location.in_files = !is_editable_kind(entry->kind) || about_the_file(diagnostic);
	if (!location.in_files && diagnostic.row_id) {
		location.record = {diagnostic.row_id, diagnostic.record_kind, diagnostic.child_id};
		location.field = diagnostic.field;
	}
	// A finding inside a text document names its place by its line and column.
	const DocumentType *type = document_type_for(entry->kind);
	if (!location.in_files && diagnostic.line && type &&
			document_content(*type) == DocumentContent::Text)
		location.locator = TextDocument::locator(diagnostic.line, diagnostic.column ? diagnostic.column : 1);
	return location;
}

std::string finding_record_title(const Diagnostic &diagnostic, const SessionView &view) {
	if (!diagnostic.row_id || diagnostic.asset.empty()) return std::string();
	for (const auto &open : view.documents.open) {
		if (!open || open->path() != diagnostic.asset) continue;
		const Document *document = records_of(*open);
		if (!document) return std::string();
		const NodeAddress address{diagnostic.row_id, diagnostic.record_kind, diagnostic.child_id};
		// In the display names' words, the project's names read (S15, Names: an entity by its item's
		// name and its SSN).
		std::optional<GraphNameSource> names;
		if (view.findings.graph) names.emplace(*view.findings.graph);
		const std::string title = record_display(*document, address, names ? &*names : nullptr);
		return title == document->record_name(address) ? std::string() : title;
	}
	return std::string();
}

std::string finding_field_title(const Diagnostic &diagnostic, const SessionView &view) {
	if (!diagnostic.row_id || diagnostic.field.empty() || diagnostic.asset.empty()) return std::string();
	for (const auto &open : view.documents.open) {
		if (!open || open->path() != diagnostic.asset) continue;
		const Document *document = records_of(*open);
		if (!document) return std::string();
		const NodeAddress address{diagnostic.row_id, diagnostic.record_kind, diagnostic.child_id};
		for (const FieldSchema &field : document->fields(address.kind)) {
			if (field.id != diagnostic.field) continue;
			const std::string title = field_title(document->field_on(address, field));
			return title == diagnostic.field ? std::string() : title;
		}
		return std::string();
	}
	return std::string();
}

EditorRequest ProblemLocation::request() const {
	if (in_files) return request::show_in_files(path);
	if (!locator.empty()) return request::open_document(path, locator);
	return request::open_record(path, record, field);
}

} // namespace opennova::editor
