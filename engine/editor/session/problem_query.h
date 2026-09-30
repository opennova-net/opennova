#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/session/session_view.h>

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
// project's own findings; a code's family, its first dotted segment, or the code itself for
// one grouped apart: an optional file's note, requirement.optional_missing), a title in
// plain words, its rows and how many of each severity they are.
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

// The title of a finding code's family in plain words ("reference.missing": "Missing
// references", "requirement.optional_missing": "Optional files"); the family itself for
// one the table does not name.
std::string problem_family_title(const std::string &code);

// The answer kept while neither the view's revision nor the query moves: the window asks
// every frame, and the revision moves with every change an answer reads (the findings, the
// active and open documents, the files and the game install's a fix looks for).
class ProblemQueryCache {
public:
	const ProblemAnswer &answer(const ProblemQuery &query, const SessionView &view);
	// How many answers it has made: what a reader made of the answer (the Problems list's
	// lines, which index its groups and rows) is made again when this moves.
	uint64_t generation() const { return generation_; }

private:
	const SessionView *view_ = nullptr;
	uint64_t revision_ = 0;
	ProblemQuery query_;
	ProblemAnswer answer_;
	uint64_t generation_ = 0;
};

// Where a finding takes Problems: a project file the scan lists. One the editor opens is
// opened, the record the finding names selected and its field shown; one the editor does not
// open (a font, an environment, an archive), or a finding about the file itself rather than
// what it holds (its name: asset.name.*, build.name_unstorable; its place:
// build.archive_in_project), is shown in Files (`in_files`). Empty when the finding names no
// file of the project (a required file the project lacks): Problems then only selects its row.
struct ProblemLocation {
	std::string path;
	NodeAddress record;
	std::string field;
	bool in_files = false;
	bool empty() const { return path.empty(); }
	// The request that goes there: OpenDocument with the record and field, or ShowInFiles.
	EditorRequest request() const;
};
ProblemLocation problem_location(const Diagnostic &diagnostic, const SessionView &view);

} // namespace opennova::editor
