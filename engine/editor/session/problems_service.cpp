#include <editor/session/problems_service.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <editor/graph/project_validation.h>
#include <editor/preview/menu_render_check.h>
#include <editor/project/project_findings.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

namespace {

// What a validation reads of an open document: it starts again when one of these moved.
struct DocumentState {
	uint64_t identity = 0, load_generation = 0, revision = 0;
	bool dirty = false, wrote_file = false;
	bool operator==(const DocumentState &o) const {
		return identity == o.identity && load_generation == o.load_generation && revision == o.revision &&
		       dirty == o.dirty && wrote_file == o.wrote_file;
	}
};

DocumentState state_of(const DocumentBase &document) {
	return {document.identity(), document.load_generation(), document.revision(), document.dirty(), document.wrote_file()};
}

} // namespace

// The validation under way: the project's paths, its document and scan and the open documents as
// they were when it started (held, so a scan the session replaced meanwhile is still the one it
// reads), their states, and the cursor over the session's graph and cache.
struct ProblemsService::Pass {
	Pass(const ProjectPaths &p, std::shared_ptr<const ProjectDocument> d, std::shared_ptr<const AssetScan> s,
			std::vector<std::shared_ptr<const DocumentBase>> o, AssetGraph &graph, ValidationCache &cache) :
			paths(p), project(std::move(d)), scan(std::move(s)), open(std::move(o)), validation(graph, cache) {
		for (const auto &document : open) states.push_back(document ? state_of(*document) : DocumentState());
	}
	ValidationInput input() const { return {paths, *project, *scan, open}; }

	ProjectPaths paths;
	std::shared_ptr<const ProjectDocument> project;
	std::shared_ptr<const AssetScan> scan;
	std::vector<std::shared_ptr<const DocumentBase>> open;
	std::vector<DocumentState> states;
	ProjectValidation validation;
	bool graph_shown = false; // Graph moved for its update
};

ProblemsService::ProblemsService(SessionCore &core) :
		core_(core), view_(core.view()), render_check_(std::make_shared<MenuRenderCheck>()) {
	view_.findings.graph = graph_;
	view_.findings.assets = assets_;
	view_.findings.render_check = render_check_;
}

ProblemsService::~ProblemsService() = default;

// The project's findings now, composed as `opennova-project validate` composes them
// (project/project_findings): the scan's, the requirements', the files the last Play's game
// reported missing and its nonzero exit, each file's own (the open documents standing in for
// theirs, every file's kept in the cache until it changes) with the use checks', the graph's and
// the open documents' own (a file changed outside the editor that was not read again), the menu
// render check's notes and the last build's own findings (after the gate the build reads: a
// note never blocks a build, nor does the last Play's report, which only the next Play can
// clear, nor the last build's, which the next build replaces). They replace the Problems rows:
// Findings moves only when they differ, and Graph only when the graph's update changed it.
void ProblemsService::validate_documents() {
	compose(false);
}

// The validation an edit left due: the findings reported while it was due, and nothing changed
// since, stay after the composed rows (reporting one ran this validation first, before S13 A2).
void ProblemsService::validate_pending_now() {
	compose(true);
}

void ProblemsService::compose(bool keep_reported) {
	while (!step_pass(UINT64_MAX)) {
	}
	compose_rows(keep_reported);
}

bool ProblemsService::pass_current() const {
	if (pass_->project != view_.project.document || pass_->scan != view_.project.scan ||
			pass_->open.size() != view_.documents.open.size())
		return false;
	for (size_t i = 0; i < pass_->open.size(); ++i)
		if (pass_->open[i] != view_.documents.open[i] ||
				!(pass_->states[i] == (pass_->open[i] ? state_of(*pass_->open[i]) : DocumentState())))
			return false;
	return true;
}

bool ProblemsService::step_pass(uint64_t bytes) {
	// A new one when one is due (a change since the one under way started, or none is), or when
	// what the one under way reads moved without one being asked (a gesture's edits): the graph
	// and the cache keep what still holds, so starting again costs what changed.
	if (validation_due_ || !pass_ || !pass_current()) {
		validation_due_ = false;
		pass_ = std::make_unique<Pass>(core_.paths(), view_.project.document, view_.project.scan, view_.documents.open,
				*graph_, validation_cache_);
	}
	const bool done = pass_->validation.step(pass_->input(), bytes);
	if (!pass_->graph_shown && pass_->validation.graph_moved()) {
		pass_->graph_shown = true;
		core_.touch(ViewConcern::Graph);
	}
	return done;
}

void ProblemsService::step_validation(const PollBudget &budget, const OperationClock &clock) {
	if (!validating() || core_.documents().gesture_open()) return;
	const int64_t start = budget.ms > 0 ? clock() : 0;
	bool done = false;
	do {
		done = step_pass(budget.step_bytes);
	} while (!done && budget.ms > 0 && clock() - start < budget.ms);
	if (done) compose_rows(true);
	else show_validation();
}

void ProblemsService::show_validation() {
	ValidationStatus status;
	status.running = validating();
	if (pass_) {
		status.done = pass_->validation.files_done();
		status.total = pass_->validation.files_total();
	}
	if (status == view_.activity.validation) return;
	view_.activity.validation = status;
	core_.touch(ViewConcern::Operation);
}

void ProblemsService::compose_rows(bool keep_reported) {
	const bool files_moved = pass_->validation.moved();
	pass_.reset();
	const std::vector<Diagnostic> open = core_.documents().findings();
	const ProjectFindingsInput input{core_.paths(),        *view_.project.document,    *view_.project.scan,
	                                 *view_.project.requirements, view_.documents.open, view_.activity.boot_missing,
	                                 play_findings_,              open,                 build_findings_};
	// The graph, each file's own findings (the pass that ended) and the render check first: when none
	// of them moved, no other input the rows are made of did, no reported finding waits on this
	// validation and the rows are as it left them, they stand. No row is composed, copied or compared
	// then; the small inputs are compared with their copies (Composed::same), and the open documents'
	// own findings are made again for it.
	const ValidationInput validation{input.paths, input.project, input.scan, input.open};
	const bool notes_moved = render_check_->update(validation, *assets_);
	const bool moved = files_moved || notes_moved;
	if (!moved && reported_.empty() && trailing_ == 0 && composed_.same(input, view_.findings.diagnostics.size())) {
		show_validation();
		return;
	}
	ProjectFindings findings = collect_project_findings(input, *graph_, validation_cache_, *render_check_);
	++compositions_;
	gate_size_ = findings.gate_end - findings.gate_begin;
	gate_tail_ = findings.rows.size() - findings.gate_end;
	// The findings kept after the composed rows, each as often as it was reported (two refusals of
	// the same edit are two rows, as they were); one the composition makes too is its row alone.
	const size_t composed = findings.rows.size();
	if (keep_reported)
		for (const Reported &reported : reported_) {
			const auto composed_end = findings.rows.begin() + static_cast<std::ptrdiff_t>(composed);
			if (reported.kept && std::find(findings.rows.begin(), composed_end, reported.finding) == composed_end)
				findings.rows.push_back(reported.finding);
		}
	trailing_ = findings.rows.size() - composed;
	reported_.clear();
	if (findings.rows != view_.findings.diagnostics) {
		view_.findings.diagnostics = std::move(findings.rows);
		core_.touch(ViewConcern::Findings);
	}
	composed_.keep(input, view_.findings.diagnostics.size());
	show_validation();
}

bool ProblemsService::Composed::same(const ProjectFindingsInput &input, size_t rows_now) const {
	return made && rows == rows_now && scan == input.scan.diagnostics &&
	       requirements == input.requirements.diagnostics && boot_missing == input.boot_missing &&
	       play == input.play && open == input.open_findings && build == input.build;
}

void ProblemsService::Composed::keep(const ProjectFindingsInput &input, size_t rows_now) {
	made = true;
	rows = rows_now;
	scan = input.scan.diagnostics;
	requirements = input.requirements.diagnostics;
	boot_missing = input.boot_missing;
	play = input.play;
	open = input.open_findings;
	build = input.build;
}

void ProblemsService::add_reported(const Diagnostic &d) {
	view_.findings.diagnostics.push_back(d);
	++trailing_;
	reported_.push_back({d, validating()});
	core_.touch(ViewConcern::Findings);
}

std::vector<Diagnostic> ProblemsService::gate_findings() {
	// Every writer of the rows keeps the range where it is (compose, add_reported, and the Play's
	// drop of its own rows, which sit before it); rows that no longer hold it are composed again,
	// never a build planned on another gate.
	if (gate_size_ + gate_tail_ + trailing_ > view_.findings.diagnostics.size())
		compose(false);
	const auto end = view_.findings.diagnostics.end() - static_cast<std::ptrdiff_t>(gate_tail_ + trailing_);
	return std::vector<Diagnostic>(end - static_cast<std::ptrdiff_t>(gate_size_), end);
}

void ProblemsService::clear() {
	graph_->clear();
	assets_->clear();
	render_check_->clear();
	validation_cache_ = ValidationCache();
	composed_ = Composed();
	gate_size_ = gate_tail_ = trailing_ = 0;
	reported_.clear();
	validation_due_ = false;
	pass_.reset();
	show_validation();
}

} // namespace opennova::editor
