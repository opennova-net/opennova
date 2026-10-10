#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/session/original_files.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

// Which findings Problems shows (ADR 0046 S11b), one query the window and the editor MCP
// share: the severities, a text matched without case against the message, the file, the
// record, the field and the code, where they sit (the whole project, the active document,
// the open documents: ProblemScope, session/view/workspace_view.h, where the window's part keeps
// them), only those with a fix (problem_fixes.h), and how they group (ProblemGrouping).
struct ProblemQuery {
	bool errors = true;
	bool warnings = true;
	bool infos = true;
	std::string text;
	ProblemScope scope = ProblemScope::Project;
	bool fixable = false;
	ProblemGrouping grouping = ProblemGrouping::None;
	// Only the rows a build is refused for (FindingMarks::blocking: the gate's refusals, ADR 0046 S14).
	bool blocking = false;

	bool shows(DiagnosticSeverity severity) const;
	bool operator==(const ProblemQuery &other) const;
	bool operator!=(const ProblemQuery &other) const { return !(*this == other); }
};

// A group of the findings shown: its key (a file's project-relative path, "" for the
// project's own findings; grouped by kind, the key of the group its row names, its code's family,
// the code's first dotted segment, or requirement.optional_missing for an optional file's note:
// session/finding_codes.h's finding_group_key), a title in plain words, its rows and how many of
// each severity they are.
struct ProblemGroup {
	std::string key;
	std::string title;
	std::vector<size_t> rows;
	size_t errors = 0;
	size_t warnings = 0;
	size_t infos = 0;
	// Shown under a header of its own (a group of the query's grouping, the game's own data's); false
	// for the modder's findings listed as one before the game's own data's (ungrouped).
	bool header = true;
	// The game's own data's (ADR 0046 S15): the findings the game install, as a whole, makes too in the file
	// of the same name (FindingsView::originals, FindingMarks::original), kOriginalGroupKey, last.
	bool original = false;
};

// The key and the title of the group of the game's own data's findings.
inline constexpr const char *kOriginalGroupKey = "original";
inline constexpr const char *kOriginalGroupTitle = "In the game's own data (also in the original)";

// What the query shows of a view: `rows` indexes the view's diagnostics in display order
// (errors, then warnings, then notes, each in the order they were reported; grouped, group
// after group, the groups in the order their first finding shows); `groups` when grouped;
// and how many findings of each severity the project has in all, shown or not ("37 of 412"
// is rows.size() of total()). The findings about the game's own data (S15: those the game install makes
// too) come after the modder's, under a group of their own
// (ProblemGroup::original; the answer grouped then whatever the query's grouping, the modder's
// findings ungrouped as one group with no header), and are counted apart: `errors`, `warnings` and
// `infos` are the modder's, `original_*` the game's own data's.
struct ProblemAnswer {
	std::vector<size_t> rows;
	bool grouped = false;
	std::vector<ProblemGroup> groups;
	size_t errors = 0;
	size_t warnings = 0;
	size_t infos = 0;
	size_t original_errors = 0;
	size_t original_warnings = 0;
	size_t original_infos = 0;
	// How many rows a build is refused for, shown or not.
	size_t blocking = 0;
	size_t original() const { return original_errors + original_warnings + original_infos; }
	size_t total() const { return errors + warnings + infos + original(); }
};

// The marks of `rows` (FindingMarks; the UX round's problems lane). A row is about the game's own data
// when the build does not gate on its code (blocks_build: the gate follows the game's refusals, so no
// count of 0 errors stands beside a refused build) and the game install, as a whole, makes the same
// finding (original_finding_key) in the file of the same logical name (OriginalData::findings, once
// `ready`), each of the install's findings taken by one row, the rows' first: what the modder's edits
// or the project's other files brought is the modder's, wherever it is. A row blocks the build when it is
// one of `blockers` (the plan's refusals, build_blockers; null: when the build gates on its code).
FindingMarks mark_findings(const std::vector<Diagnostic> &rows, const OriginalData *originals,
                           const std::vector<Diagnostic> *blockers);
// The view's marks: the session's while they are the rows' (FindingsView::marks), else made into
// `scratch` from what the view holds (a view no session made).
const FindingMarks &finding_marks(const SessionView &view, FindingMarks &scratch);
// One row's marks, the session's; for a view without them, worked out for that row alone (a finding of a
// file held otherwise is the original's where its copy makes one of its key, none taken).
bool in_original_data(size_t row, const SessionView &view);
bool blocks_the_build(size_t row, const SessionView &view);
// The view's findings by severity, the modder's and the game's own data's apart, and how many a build is
// refused for: what Problems, the menu bar, the view's problem_counts and `opennova-project status` all
// count.
struct ProblemCounts {
	size_t errors = 0, warnings = 0, infos = 0;
	size_t original_errors = 0, original_warnings = 0, original_infos = 0;
	size_t blocking = 0;
};
ProblemCounts count_problems(const SessionView &view);

ProblemAnswer answer_problems(const ProblemQuery &query, const SessionView &view);

// What an answer reads of a view, as a cache's key (view_revisions.h): the findings; which
// document is active for the active file's scope (ActiveDocument), which are open for the open
// files' (DocumentSet); for only the fixable, what their fixes read (problem_fix_key).
RevisionKey problem_query_key(const SessionView &view, const ProblemQuery &query);

// The answer kept while neither what it reads (problem_query_key) nor the query moves: the
// window asks every frame.
class ProblemQueryCache {
public:
	const ProblemAnswer &answer(const ProblemQuery &query, const SessionView &view);
	// How many answers it has made: what a reader made of the answer (the Problems list's
	// lines, which index its groups and rows) is made again when this moves.
	uint64_t generation() const { return generation_; }

private:
	const SessionView *view_ = nullptr;
	RevisionKey key_;
	ProblemQuery query_;
	ProblemAnswer answer_;
	uint64_t generation_ = 0;
};

// Where a finding takes Problems: a project file the scan lists. One the editor opens is
// opened, the record the finding names selected and its field shown; one the editor has no editor
// for (a font, a terrain, a wave) lands on its page as every Go to does (`page`, ADR 0046 DI-17: one
// OpenDocument, always), the record the finding names marked there by its path as the graph names it
// and its field; a finding about the file itself rather than what it holds (its row's place: its
// name, asset.name.* and build.name_unstorable; its place, build.archive_in_project) is shown in
// Files (`in_files`), whose Rename... sets it right. Empty when the finding names no file of the
// project (a required file the project lacks): Problems then only selects its row.
struct ProblemLocation {
	std::string path;
	NodeAddress record;
	std::string field;
	// In a text document (ADR 0046 S13 D9), the finding's place: "line:column" (its column 1 where
	// the finding names its line alone); on a file's page, the record the finding names (its path as
	// the graph names it, Diagnostic::record; "" for the file alone).
	std::string locator;
	bool page = false;
	bool in_files = false;
	bool empty() const { return path.empty(); }
	// The request that goes there: OpenDocument with the record and field (a text's place by its
	// locator, a page's record by its path), or ShowInFiles.
	EditorRequest request() const;
};
ProblemLocation problem_location(const Diagnostic &diagnostic, const SessionView &view);

// What the record a finding is about reads as in the windows (Document::record_title: a mission's
// trigger or action in words, ADR 0046 S15) where its document is open and its type words it
// otherwise than its name; "" where it does not (the finding's record path stands alone).
std::string finding_record_title(const Diagnostic &diagnostic, const SessionView &view);
// What the record a finding is about calls the field it names (FieldUse::label: a trigger's
// parameter by what it reads, "Zone"), where its document is open and the field has a name of its
// own; "" where it does not (the finding's field id stands alone).
std::string finding_field_title(const Diagnostic &diagnostic, const SessionView &view);

} // namespace opennova::editor
