#include <editor/session/problems_service.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <editor/assets/asset_registry.h>
#include <editor/documents/project_checks.h>
#include <editor/graph/project_validation.h>
#include <editor/project/project_files.h>
#include <editor/project/project_findings.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/document_set.h>
#include <editor/session/original_files.h>
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
// reads), their states, the cursor over the session's graph, cache and readings ahead, then the
// project checks it has brought up (ProjectChecks' slots, each stepped) and whether one said its
// findings moved.
struct ProblemsService::Pass {
	Pass(const ProjectPaths &p, std::shared_ptr<const ProjectDocument> d, std::shared_ptr<const AssetScan> s,
			std::vector<std::shared_ptr<const DocumentBase>> o, AssetGraph &graph, ValidationCache &cache,
			GraphReadings &readings) :
			paths(p), project(std::move(d)), scan(std::move(s)), open(std::move(o)), validation(graph, cache, readings) {
		for (const auto &document : open) states.push_back(document ? state_of(*document) : DocumentState());
	}
	ValidationInput input() const { return {paths, *project, *scan, open}; }

	ProjectPaths paths;
	std::shared_ptr<const ProjectDocument> project;
	std::shared_ptr<const AssetScan> scan;
	std::vector<std::shared_ptr<const DocumentBase>> open;
	std::vector<DocumentState> states;
	ProjectValidation validation;
	size_t checks_run = 0;    // the project checks' slots brought up (ProjectChecks::slot_count)
	bool check_begun = false; // the slot at checks_run started (ProjectChecks::begin_slot)
	bool checks_moved = false;
	bool graph_shown = false; // Graph moved for its update
};

ProblemsService::ProblemsService(SessionCore &core) :
		core_(core), view_(core.view()), checks_(std::make_shared<ProjectChecks>()),
		originals_(std::make_unique<OriginalFiles>()) {
	view_.findings.graph = graph_;
	view_.findings.assets = assets_;
	view_.findings.project_checks = checks_;
	view_.findings.originals = originals_->data();
}

ProblemsService::~ProblemsService() = default;

// The validation an edit left due, run to its end: the project's findings now, composed as
// `opennova-project validate` composes them (project/project_findings): the scan's, the
// requirements', the files the last Play's game reported missing and its nonzero exit, each file's
// own (the open documents standing in for theirs, every file's kept in the cache until it changes)
// with the use checks', the graph's and the open documents' own (a file changed outside the editor
// that was not read again), the document types' project checks' findings (the menu render check's
// notes) and the last build's own findings (after the gate the build reads: a project check's
// finding never blocks a build, nor does the last Play's report, which only the next Play can
// clear, nor the last build's, which the next build replaces). They replace the Problems rows:
// Findings moves only when they differ, and Graph only when the graph's update changed it. The
// findings reported while it was due, and nothing changed since, stay after the composed rows.
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
	// what the one under way reads moved without one being asked (a gesture's edits): the graph,
	// the cache and the readings ahead keep what still holds, so starting again costs what changed.
	// One taking the place of a pass that had not ended composes the rows whatever its own counts
	// say: what that pass brought up before it stopped is no longer counted.
	if (validation_due_ || !pass_ || !pass_current()) {
		if (pass_) moved_since_composed_ = true;
		validation_due_ = false;
		pass_ = std::make_unique<Pass>(core_.paths(), view_.project.document, view_.project.scan, view_.documents.open,
				*graph_, validation_cache_, readings_);
	}
	Pass &pass = *pass_;
	if (!pass.validation.done()) {
		pass.validation.step(pass.input(), bytes);
		if (!pass.graph_shown && pass.validation.graph_moved()) {
			pass.graph_shown = true;
			core_.touch(ViewConcern::Graph);
		}
		if (pass.validation.graph_read()) graph_scan_ = pass.scan;
		return false;
	}
	// After the last file's own findings, the project checks in the registry's order (a check reads
	// which files' records their own checks read), each stepped within the budget (the render check
	// renders a menu a step): the step that ends a check ends there, and the slots with no check
	// pass by in the same step.
	const ValidationInput input = pass.input();
	while (pass.checks_run < checks_->slot_count()) {
		const size_t slot = pass.checks_run;
		if (!pass.check_begun) {
			checks_->begin_slot(slot);
			pass.check_begun = true;
		}
		bool moved = false;
		if (!checks_->step_slot(slot, {input, validation_cache_, *assets_}, bytes, moved)) return false;
		pass.checks_moved = pass.checks_moved || moved;
		pass.check_begun = false;
		++pass.checks_run;
		if (checks_->has_check(slot)) return false;
	}
	return true;
}

bool ProblemsService::advance(uint64_t bytes) {
	if (!validating()) return true;
	if (core_.documents().gesture_open()) return false;
	if (step_pass(bytes)) {
		compose_rows(true);
		return true;
	}
	show_validation();
	return false;
}

void ProblemsService::step_validation(const PollBudget &budget, const OperationClock &clock) {
	const int64_t start = budget.ms > 0 ? clock() : 0;
	if (validating()) {
		if (core_.documents().gesture_open()) return;
		while (!advance(budget.step_bytes) && budget.ms > 0 && clock() - start < budget.ms) {
		}
		if (validating()) return;
	}
	// The rows stand: which files they are about are the game's own data, in what is left of the budget.
	while (!step_originals(budget.step_bytes) && budget.ms > 0 && clock() - start < budget.ms) {
	}
}

void ProblemsService::want_originals() {
	if (!view_.project.open || !view_.project.scan) return;
	// Another install or game forgets what was found (the view says so at once); a new scan looks at the
	// install's folder again.
	originals_->want(core_.base_game(), view_.project.document, view_.project.scan.get());
	// The install is validated once a row may be about its data: a finding on a file of a name it serves.
	originals_needed_ = false;
	const std::vector<std::string> &served = view_.project.retail_files;
	const auto by_name = [](const std::string &a, const std::string &b) {
		return pff::normalized_logical_name(a) < pff::normalized_logical_name(b);
	};
	for (const Diagnostic &d : view_.findings.diagnostics) {
		if (d.asset.empty() || blocks_build(d)) continue;
		const std::string name = basename_of(d.asset);
		if (std::binary_search(served.begin(), served.end(), name, by_name)) {
			originals_needed_ = true;
			break;
		}
	}
	show_originals();
}

bool ProblemsService::step_originals(uint64_t bytes) {
	if (!originals_needed_ || originals_->settled()) return true;
	// The install validated as a project of its own, nothing of the session's read (the polls step this
	// only once no validation of the project is due or under way).
	const bool done = originals_->step(bytes);
	show_originals();
	if (done) show_validation(); // the counts stand from here
	return done;
}

void ProblemsService::show_originals() {
	if (view_.findings.originals == originals_->data()) return;
	view_.findings.originals = originals_->data();
	mark_rows();
	core_.touch(ViewConcern::Findings);
}

// The gate as it stands among the rows (gate_findings' range), never composing: empty when the rows no
// longer hold it.
std::vector<Diagnostic> ProblemsService::gate_rows() const {
	const std::vector<Diagnostic> &rows = view_.findings.diagnostics;
	if (gate_size_ + gate_tail_ + trailing_ > rows.size()) return {};
	const auto end = rows.end() - static_cast<std::ptrdiff_t>(gate_tail_ + trailing_);
	return std::vector<Diagnostic>(end - static_cast<std::ptrdiff_t>(gate_size_), end);
}

void ProblemsService::mark_rows() {
	blockers_.clear();
	if (view_.project.open && view_.project.scan && view_.project.requirements && view_.project.document) {
		// The plan a build would make (ADR 0046 S16): the project's target, an expansion's gate over its base
		// game's names, and the game's own bytes packed as stored.
		const std::vector<Diagnostic> gate = gate_rows();
		const BaseNames base{&view_.project.base_files};
		const ShippedFiles shipped = core_.shipped_files(gate);
		blockers_ = build_blockers(plan_build(core_.paths(), *view_.project.scan, *view_.project.requirements, gate,
		                                      core_.build_target(), &base, &shipped));
	}
	auto marks = std::make_shared<const FindingMarks>(
	        mark_findings(view_.findings.diagnostics, view_.findings.originals.get(), &blockers_));
	if (view_.findings.marks && *view_.findings.marks == *marks) return;
	view_.findings.marks = std::move(marks);
	core_.touch(ViewConcern::Findings);
}

void ProblemsService::settle_originals() {
	while (!step_originals(UINT64_MAX)) {
	}
}


void ProblemsService::show_validation() {
	ValidationStatus status;
	// The check of which rows are the game's own data is the validation's last part (the UX round's
	// problems lane): until it settles the counts may still move, so a client waiting on the validation
	// waits for it too, and the menu bar says "Validating" meanwhile.
	status.running = validating() || (view_.project.open && originals_needed_ && !originals_->settled());
	status.read = read_once_;
	status.files_unread = view_.project.open && view_.project.scan && graph_scan_ != view_.project.scan;
	if (pass_) {
		status.done = pass_->validation.files_done();
		status.total = pass_->validation.files_total();
	} else if (status.running) {
		status.done = status.total = view_.activity.validation.total;
	}
	// A validation started again shows where the one before stood until it passes it: the progress
	// shown never falls back while one runs.
	const ValidationStatus &shown = view_.activity.validation;
	if (status.running && shown.running && status.done < shown.done) {
		status.done = shown.done;
		status.total = shown.total;
	}
	if (status == view_.activity.validation) return;
	view_.activity.validation = status;
	core_.touch(ViewConcern::Operation);
}

void ProblemsService::compose_rows(bool keep_reported) {
	read_once_ = true;
	const bool moved = pass_->validation.moved() || pass_->checks_moved || moved_since_composed_;
	moved_since_composed_ = false;
	pass_.reset();
	const std::vector<Diagnostic> open = core_.documents().findings();
	const ProjectFindingsInput input{core_.paths(),        *view_.project.document,    *view_.project.scan,
	                                 *view_.project.requirements, view_.documents.open, view_.activity.boot_missing,
	                                 play_findings_,              open,                 build_findings_};
	// The graph, each file's own findings and the project checks (the pass that ended) first: when
	// none of them moved, no other input the rows are made of did, no reported finding waits on this
	// validation and the rows are as it left them, they stand. No row is composed, copied or compared
	// then; the small inputs are compared with their copies (Composed::same), and the open documents'
	// own findings are made again for it.
	if (!moved && reported_.empty() && trailing_ == 0 && composed_.same(input, view_.findings.diagnostics.size())) {
		want_originals();
		mark_rows();
		show_validation();
		return;
	}
	ProjectFindings findings = collect_project_findings(input, *graph_, validation_cache_, *checks_);
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
	lead_scan_ = composed_.scan;
	lead_requirements_ = composed_.requirements;
	mark_rows();
	want_originals();
	show_validation();
}

// The rows lead with the scan's findings, then the requirements' (project_findings's order): those the
// files' landing changed take their place at once, the rest standing until the validation the files
// left due composes them (the demo round's bug 10: an import's files landed, yet its 27 "required file
// missing" rows stood until the validation after it ended). The composition is left as it was, so the
// validation's own composes the rows whatever its counts say.
void ProblemsService::show_requirements() {
	if (!composed_.made || !view_.project.scan || !view_.project.requirements) return;
	std::vector<Diagnostic> &rows = view_.findings.diagnostics;
	const size_t lead = lead_scan_.size() + lead_requirements_.size();
	if (lead > rows.size() || !std::equal(lead_scan_.begin(), lead_scan_.end(), rows.begin()) ||
	    !std::equal(lead_requirements_.begin(), lead_requirements_.end(), rows.begin() + std::ptrdiff_t(lead_scan_.size())))
		return; // the rows no longer lead with them: the validation composes them
	const std::vector<Diagnostic> &scan = view_.project.scan->diagnostics;
	const std::vector<Diagnostic> &requirements = view_.project.requirements->diagnostics;
	if (scan == lead_scan_ && requirements == lead_requirements_) return;
	rows.erase(rows.begin(), rows.begin() + std::ptrdiff_t(lead));
	rows.insert(rows.begin(), requirements.begin(), requirements.end());
	rows.insert(rows.begin(), scan.begin(), scan.end());
	lead_scan_ = scan;
	lead_requirements_ = requirements;
	core_.touch(ViewConcern::Findings);
	mark_rows();
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
	// A reported finding comes after the gate and is the request's (a refusal, an import's word), never the
	// game's own: its marks follow the rows' over the plan the rows were last marked against, not planned
	// again (an import may report hundreds).
	if (view_.findings.marks && view_.findings.marks->rows + 1 == view_.findings.diagnostics.size()) {
		auto marks = std::make_shared<FindingMarks>(*view_.findings.marks);
		const bool blocks = blocks_build(d) && std::find(blockers_.begin(), blockers_.end(), d) != blockers_.end();
		++marks->rows;
		marks->original.push_back(0);
		marks->blocking.push_back(blocks ? 1 : 0);
		marks->blocking_count += blocks ? 1 : 0;
		view_.findings.marks = std::move(marks);
	} else {
		mark_rows();
	}
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

void ProblemsService::set_base_layer(std::shared_ptr<const GraphLayer> layer) {
	if (!graph_->set_base(std::move(layer)).changed) return;
	pass_.reset();
	moved_since_composed_ = true;
	validate_later();
}

void ProblemsService::clear() {
	graph_->clear();
	assets_->clear();
	checks_->clear();
	validation_cache_ = ValidationCache();
	composed_ = Composed();
	lead_scan_.clear();
	lead_requirements_.clear();
	gate_size_ = gate_tail_ = trailing_ = 0;
	reported_.clear();
	validation_due_ = false;
	read_once_ = false;
	graph_scan_.reset();
	pass_.reset();
	moved_since_composed_ = false;
	readings_.clear();
	originals_->clear();
	originals_needed_ = false;
	blockers_.clear();
	view_.findings.marks.reset();
	show_originals();
	show_validation();
}

} // namespace opennova::editor
