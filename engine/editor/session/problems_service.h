#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class OriginalFiles;
class ProjectChecks;
class SessionCore;
struct ProjectFindingsInput;
struct SessionView;

// The Problems rows of the project session (ADR 0046 S13 A2): when the project validates (an edit
// leaves it due rather than running it, and the polls step it, S13 A3) and what the one findings
// composer reads
// (compose_project_findings, project/project_findings: the asset graph, the validation cache, the
// document types' project checks (S13 V9: the state each keeps between validations lives here,
// one per type by its DocumentTypeId) and the project's files as the open documents stand in for
// theirs; the last Play's own findings; the last build's own; the open documents' own). The
// findings a request reports are an input of their own (add_reported), shown after the composed
// rows, so reporting a finding never validates: the validation an edit had left due when it was
// reported keeps it (it ran first, when report() validated), and a later one drops it. A kept
// finding is compared with the composed rows alone: one the composition makes too is shown once,
// and a finding reported twice is two rows. It keeps the Problems query and the fixes the query
// seam's problems row asks for (answer, fixes), each until what it reads moves.
//
// A validation is stepped (S13 A3): the graph's files read ahead and its update, then each file's
// own findings, a file at a time (graph/project_validation.h's ProjectValidation), then the project
// checks, each a step at a time (documents/project_check.h: the render check a menu a step), then
// the rows. The poll steps the one left due first within its budget (step_validation), so the first
// validation of a large project spreads over frames while the editor draws, and an operation that
// reads the graph (an import's plan, a rename) joins it: its first steps are the validation's
// remaining ones (advance). No request runs it: a test, the command line and the build's gate run
// it to its end (validate_pending). What it reads is held as it was when it started (the scan, the
// project, the open documents and their states), and it starts again when that moved, keeping the
// files it read ahead that are still as the scan lists them; a gesture's edits hold it until they
// end.
class ProblemsService {
public:
	explicit ProblemsService(SessionCore &core);
	~ProblemsService();
	ProblemsService(const ProblemsService &) = delete;
	ProblemsService &operator=(const ProblemsService &) = delete;

	// An edit's validation, left for the polls to step (an operation that reads the graph joins
	// it; a caller that waits runs it, validate_pending). The rows stand until it ends; the view's
	// validation status says at once that one is due (a client waits on it).
	void validate_later() {
		validation_due_ = true;
		for (Reported &reported : reported_) reported.kept = false; // a change after them
		show_validation();
	}
	// The validation left due, or the one under way, run to its end and the rows composed (a test's,
	// the command line's, the build's gate's, a query's that reads the gate).
	void validate_pending() {
		if (validating()) validate_pending_now();
	}
	// The poll's validation steps (S13 A3): the validation left due, or the one under way, stepped
	// within `budget` (at least one step, none while a gesture's edits wait for it to end), its rows
	// composed on the step that ends it; with none left, within what is left of the budget, the check
	// of which of the rows are about the game's own data (S15, OriginalFiles, per finding), the view's
	// originals, its marks and Findings moving when it finds otherwise.
	void step_validation(const PollBudget &budget, const OperationClock &clock);
	// The rows' marks made again (FindingsView::marks: the game's own data's, and what blocks the build,
	// the build's plan over the gate as it stands among them): after every change of the rows, of what
	// the game's own data's check found and of the open documents' unsaved state (a Play's drop of its own
	// rows calls it). Findings moves when they changed.
	void mark_rows();
	// That check run to its end (a test's, after its operations: ProjectSession::run_operations).
	void settle_originals();
	// What it found forgotten (a whole refresh: the install may have changed under the same folder); the
	// files the rows are about are checked again after the next validation.
	void forget_originals();
	// One step of the validation due or under way within `bytes` (an operation that joins it: its
	// first steps are the validation's remaining ones), its rows composed on the step that ends it;
	// none while a gesture's edits are open. True when none is due or under way.
	bool advance(uint64_t bytes);
	// True while a validation is due or under way: the Problems rows are the last composed.
	bool validating() const { return validation_due_ || pass_ != nullptr; }
	// The view's validation status (ActivityView::validation) as it stands: running while one is
	// due or under way, the files asked of those it asks (a validation started again showing where
	// the one before stood until it passes it); Operation moves when it changed.
	void show_validation();

	// A finding a request or a poll reported: a Problems row now, and after the composed rows until
	// the validation for a change made after it (one due or under way when it was reported keeps it).
	void add_reported(const Diagnostic &d);

	// The project's files as the scan lists them, the open documents standing in (set_open).
	void set_scan(const std::string &root, const AssetScan &scan, const std::string &game) {
		assets_->set_scan(root, scan, game);
	}
	void set_open(const std::vector<std::shared_ptr<const DocumentBase>> &documents) {
		assets_->set_open(documents);
	}
	// The last build's own findings, Problems rows until the next build starts or the project closes.
	void set_build_findings(std::vector<Diagnostic> findings) { build_findings_ = std::move(findings); }
	void clear_build_findings() { build_findings_.clear(); }
	// The last Play's own (a nonzero exit), rows until Play starts again or the project closes.
	void set_play_findings(std::vector<Diagnostic> findings) { play_findings_ = std::move(findings); }
	// What the open project held goes (close_project): the graph emptied under a new generation,
	// what the project checks held, the cache and the last validation's findings; nothing left due
	// or under way.
	void clear();

	const AssetGraph &graph() const { return *graph_; }
	// The last validation's gate, which the build plan gates on: every document type's findings
	// over the files, the use checks' and the graph's, then the open documents' own (the project
	// checks' findings are not: they never block a build). A copy of those Problems rows.
	std::vector<Diagnostic> gate_findings();
	// What the last validation did: the files whose own findings it made and kept, the closed
	// files it read.
	const ValidationStats &validation_stats() const { return validation_cache_.stats(); }
	// How many times the rows were composed: a validation that finds nothing they are made of
	// moved composes none (for the tests).
	size_t compositions() const { return compositions_; }

	// The Problems query as the query seam's problems row asks it (editor_queries.h, S13 A5): the
	// rows the query shows, kept while the view and the query stand, and their fixes, kept while
	// what they read stands.
	const ProblemAnswer &answer(const ProblemQuery &query) { return query_cache_.answer(query, view_); }
	ProblemFixCache &fixes() { return fix_cache_; }

private:
	// A reported finding, and whether the validation due when it was reported keeps it (none was
	// due, or a change came after it: the next validation drops it).
	struct Reported {
		Diagnostic finding;
		bool kept = false;
	};
	// The validation under way (problems_service.cpp): what it reads, held as it started, and its
	// cursor over the graph and the cache.
	struct Pass;

	// What the last composition read beside the graph, the files' own findings and the project
	// checks (whose refresh says whether they moved): the rows stand while these do and the rows
	// are as many as it left.
	struct Composed {
		bool made = false;
		size_t rows = 0;
		std::vector<Diagnostic> scan, requirements, play, open, build;
		std::vector<std::string> boot_missing;
		bool same(const ProjectFindingsInput &input, size_t rows_now) const;
		void keep(const ProjectFindingsInput &input, size_t rows_now);
	};

	void validate_pending_now();
	// The validation under way (started when due or none is, and again when what it reads moved)
	// run to its end, then the rows.
	void compose(bool keep_reported);
	// One step of it within `bytes`; true when it is done.
	bool step_pass(uint64_t bytes);
	// Whether what the pass under way reads is the view's still.
	bool pass_current() const;
	// The rows, from the pass that ended (its files' findings moved or not, a pass it took the place
	// of having moved them, and the project checks') and, with `keep_reported`, what was reported.
	void compose_rows(bool keep_reported);
	// The check of the game's own data asked again of the rows as they stand; one step of it within
	// `bytes`, true when none is left.
	void want_originals();
	bool step_originals(uint64_t bytes);
	// The view's originals made the check's where they moved, the rows marked again (Findings then moves).
	void show_originals();
	// The gate as it stands among the rows (gate_findings' range), never composing: none when the rows
	// no longer hold it.
	std::vector<Diagnostic> gate_rows() const;

	SessionCore &core_;
	SessionView &view_;
	std::shared_ptr<AssetGraph> graph_ = std::make_shared<AssetGraph>();
	std::shared_ptr<ProjectAssetSource> assets_ = std::make_shared<ProjectAssetSource>();
	// The document types' project checks, one per type that has one (S13 V9): what each keeps
	// between validations, made when the session is.
	std::shared_ptr<ProjectChecks> checks_;
	ValidationCache validation_cache_;
	// Where the last validation's gate sits among the view's rows, counted from their end, so the
	// rows a Play drops before it (its boot report's, its crash's) leave it where it is: its size,
	// the composed rows after it (the project checks' findings, the last build's own) and the rows
	// after those (the reported findings a validation kept, and those reported since).
	size_t gate_size_ = 0, gate_tail_ = 0, trailing_ = 0;
	Composed composed_;
	size_t compositions_ = 0;
	// The last build's own findings (those its report adds to the Problems rows it was gated on:
	// the plan's own, a step that failed), Problems rows until the next build starts or the
	// project closes.
	std::vector<Diagnostic> build_findings_;
	std::vector<Diagnostic> play_findings_; // the last Play's own (a nonzero exit), that project's too
	std::vector<Reported> reported_;        // reported since the last validation
	bool validation_due_ = false;           // an edit since the last validation
	std::unique_ptr<Pass> pass_;            // the validation under way, stepped by the polls
	// A pass was started again before it ended: what it had brought to the graph, the cache and the
	// project checks may have moved the rows, which its successor's own counts cannot say (S13 A3).
	bool moved_since_composed_ = false;
	// The files the graph's update reads, read ahead a file a step and kept from one pass to the next
	// (ProjectValidation): a pass started again takes those still as the scan lists them.
	GraphReadings readings_;
	ProblemQueryCache query_cache_;         // the problems query's answer, kept while both stand
	ProblemFixCache fix_cache_;             // and its problems' fixes while the view stands
	// Which of the files the rows are about are the game's own data (S15), checked after a validation.
	std::unique_ptr<OriginalFiles> originals_;
};

} // namespace opennova::editor
