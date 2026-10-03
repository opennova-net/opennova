#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

// Which findings Problems shows (ADR 0046 S11b), one query the window and the editor MCP
// share: the severities, a text matched without case against the message, the file, the
// record, the field and the code, where they sit (the whole project, the active document,
// the open documents), only those with a fix (problem_fixes.h), and how they group.
enum class ProblemScope { Project, ActiveFile, OpenFiles };
enum class ProblemGrouping { None, File, Kind };

struct ProblemQuery {
	bool errors = true;
	bool warnings = true;
	bool infos = true;
	std::string text;
	ProblemScope scope = ProblemScope::Project;
	bool fixable = false;
	ProblemGrouping grouping = ProblemGrouping::None;

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
};

// What the query shows of a view: `rows` indexes the view's diagnostics in display order
// (errors, then warnings, then notes, each in the order they were reported; grouped, group
// after group, the groups in the order their first finding shows); `groups` when grouped;
// and how many findings of each severity the project has in all, shown or not ("37 of 412"
// is rows.size() of total()).
struct ProblemAnswer {
	std::vector<size_t> rows;
	bool grouped = false;
	std::vector<ProblemGroup> groups;
	size_t errors = 0;
	size_t warnings = 0;
	size_t infos = 0;
	size_t total() const { return errors + warnings + infos; }
};

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
// opened, the record the finding names selected and its field shown; one the editor does not
// open (a font, an environment, an archive), or a finding about the file itself rather than
// what it holds (its row's place: its name, asset.name.* and build.name_unstorable; its place,
// build.archive_in_project), is shown in Files (`in_files`). Empty when the finding names no
// file of the project (a required file the project lacks): Problems then only selects its row.
struct ProblemLocation {
	std::string path;
	NodeAddress record;
	std::string field;
	// In a text document (ADR 0046 S13 D9), the finding's place: "line:column" (its column 1 where
	// the finding names its line alone).
	std::string locator;
	bool in_files = false;
	bool empty() const { return path.empty(); }
	// The request that goes there: OpenDocument with the record and field (a text's place by its
	// locator), or ShowInFiles.
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
