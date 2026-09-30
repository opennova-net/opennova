#include <editor/session/problem_query.h>

#include <initializer_list>
#include <map>
#include <memory>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// The families of the finding codes Problems groups by kind, with their titles: a code's
// first dotted segment, or a whole code the table names apart (an optional file the
// project lacks is a note, not one of the files the game cannot start without).
struct Family {
	const char *key;
	const char *title;
};
constexpr Family kFamilies[] = {
	{"requirement.optional_missing", "Optional files"},
	{"requirement", "Required files"},
	{"reference", "Missing references"},
	{"menu", "Menus"},
	{"style", "Stylesheets"},
	{"catalog", "Catalogs"},
	{"strings", "String tables"},
	{"model", "Models"},
	{"animation", "Animations"},
	{"animation_map", "Animation maps"},
	{"import", "Imports"},
	{"asset", "Project files"},
	{"build", "Build"},
	{"play", "Play"},
	{"document", "Documents"},
	{"graph", "Files not checked"},
	{"project", "Project"},
};

std::string family_of(const std::string &code) {
	for (const Family &row : kFamilies)
		if (code == row.key) return code;
	return code.substr(0, code.find('.'));
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
	for (const std::string *member : {&d.message, &d.asset, &d.record, &d.field, &d.code})
		if (strutil::to_lower(*member).find(needle) != std::string::npos) return true;
	return false;
}

// A finding about a file as a whole in the project, which Files shows and renames: its name
// (it does not fit the archives, another file has it) or its place (an archive the build does
// not pack).
bool about_the_file(const std::string &code) {
	return code.rfind("asset.name.", 0) == 0 || code == "build.name_unstorable" || code == "build.archive_in_project";
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

std::string problem_family_title(const std::string &code) {
	const std::string family = family_of(code);
	for (const Family &row : kFamilies)
		if (family == row.key) return row.title;
	return family;
}

ProblemAnswer answer_problems(const ProblemQuery &query, const SessionView &view) {
	ProblemAnswer answer;
	answer.grouped = query.grouping != ProblemGrouping::None;
	const std::vector<Diagnostic> &findings = view.findings.diagnostics;
	for (const Diagnostic &d : findings) tally(d.severity, answer.errors, answer.warnings, answer.infos);
	const std::string needle = strutil::to_lower(query.text);
	// Only the fixable: the view's findings read once for all of them, not once per finding.
	std::unique_ptr<ProblemFixIndex> index;
	if (query.fixable) index = std::make_unique<ProblemFixIndex>(view);
	for (const DiagnosticSeverity severity :
	     {DiagnosticSeverity::Error, DiagnosticSeverity::Warning, DiagnosticSeverity::Info}) {
		if (!query.shows(severity)) continue;
		for (size_t i = 0; i < findings.size(); ++i) {
			const Diagnostic &d = findings[i];
			if (d.severity != severity || !in_scope(d, query.scope, view) || !matches_text(d, needle)) continue;
			if (index && !has_fixes(d, view, index.get())) continue;
			answer.rows.push_back(i);
		}
	}
	if (!answer.grouped) return answer;
	std::map<std::string, size_t> placed; // a key -> its group
	for (const size_t i : answer.rows) {
		const Diagnostic &d = findings[i];
		const std::string key = query.grouping == ProblemGrouping::File ? d.asset : family_of(d.code);
		auto found = placed.find(key);
		if (found == placed.end()) {
			ProblemGroup group;
			group.key = key;
			group.title = query.grouping == ProblemGrouping::Kind ? problem_family_title(d.code)
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
	location.in_files = !is_editable_kind(entry->kind) || about_the_file(diagnostic.code);
	if (!location.in_files && diagnostic.row_id) {
		location.record = {diagnostic.row_id, diagnostic.record_kind, diagnostic.child_id};
		location.field = diagnostic.field;
	}
	return location;
}

EditorRequest ProblemLocation::request() const {
	if (in_files) return request::show_in_files(path);
	return request::open_record(path, record, field);
}

} // namespace opennova::editor
