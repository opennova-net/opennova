#include <editor/session/problems_service.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <editor/assets/asset_import.h>
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
	uint64_t identity = 0, load_generation = 0, revision = 0, companion = 0;
	bool dirty = false, wrote_file = false;
	bool operator==(const DocumentState &o) const {
		return identity == o.identity && load_generation == o.load_generation && revision == o.revision &&
		       companion == o.companion && dirty == o.dirty && wrote_file == o.wrote_file;
	}
};

DocumentState state_of(const DocumentBase &document) {
	return {document.identity(), document.load_generation(), document.revision(), document.companion_state(),
	        document.dirty(), document.wrote_file()};
}

} // namespace

// The validation under way: the project's paths, its document and scan and the open documents as
// they were when it started (held, so a scan the session replaced meanwhile is still the one it
// reads), their states, the cursor over the session's graph, cache and readings ahead, then the
// project checks it has brought up (ProjectChecks' slots, each stepped) and whether one said its
// findings moved.
struct ProblemsService::Pass {
	Pass(const ProjectPaths &p, std::shared_ptr<const ProjectDocument> d, std::shared_ptr<const AssetScan> s,
			std::vector<std::shared_ptr<const DocumentBase>> o, std::shared_ptr<const InstallFileSource> b,
			AssetGraph &graph, ValidationCache &cache, GraphReadings &readings) :
			paths(p), project(std::move(d)), scan(std::move(s)), open(std::move(o)), base(std::move(b)),
			validation(graph, cache, readings) {
		for (const auto &document : open) states.push_back(document ? state_of(*document) : DocumentState());
	}
	ValidationInput input() const { return {paths, *project, *scan, open, nullptr, base.get()}; }

	ProjectPaths paths;
	std::shared_ptr<const ProjectDocument> project;
	std::shared_ptr<const AssetScan> scan;
	std::vector<std::shared_ptr<const DocumentBase>> open;
	std::shared_ptr<const InstallFileSource> base; // the base game's files as the pass began (ValidationInput::base)
	std::vector<DocumentState> states;
	ProjectValidation validation;
	size_t checks_run = 0;    // the project checks' slots brought up (ProjectChecks::slot_count)
	bool check_begun = false; // the slot at checks_run started (ProjectChecks::begin_slot)
	bool checks_moved = false;
	bool graph_shown = false; // Graph moved for its update
};

// What the rows' marks were last planned over (mark_rows): the scan's serial and the requirements made with it,
// the gate's rows, the build's target (its NovaWorld lobby file among it, content-addressed by its path), the
// base game's names and the files packed as shipped. While each stands the plan's refusals do (plan_blockers
// reads nothing else but the expansion's table, which a save of it rescans).
struct ProblemsService::BlockerMemo {
	bool made = false;
	uint64_t serial = 0;
	std::shared_ptr<const RequirementReport> requirements;
	std::vector<Diagnostic> gate;
	BuildTarget target;
	std::vector<std::string> base_files;
	ShippedFiles shipped;

	bool same(uint64_t serial_now, const std::shared_ptr<const RequirementReport> &requirements_now,
	          const BuildTarget &target_now, const std::vector<std::string> &base_now,
	          const ShippedFiles &shipped_now) const {
		return made && serial_now != 0 && serial == serial_now && requirements == requirements_now &&
		       target.expansion == target_now.expansion && target.install == target_now.install &&
		       target.game == target_now.game && target.base_project == target_now.base_project &&
		       target.gate_tag_file == target_now.gate_tag_file && base_files == base_now &&
		       shipped.original == shipped_now.original && shipped.unsaved == shipped_now.unsaved;
	}
};

ProblemsService::ProblemsService(SessionCore &core) :
		core_(core), view_(core.view()), checks_(std::make_shared<ProjectChecks>()),
		originals_(std::make_unique<OriginalFiles>()), blocker_memo_(std::make_unique<BlockerMemo>()) {
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
	if (held_) return compose_held();
	while (!step_pass(UINT64_MAX)) {
	}
	compose_rows(keep_reported);
}

void ProblemsService::hold_validation(bool held) {
	if (held == held_) return;
	held_ = held;
	if (!held) {
		// The rows are made again whatever the counts say: the held ones were made of none of it.
		moved_since_composed_ = true;
		validate_later();
		return;
	}
	pass_.reset();
	compose_held();
}

// The scan's and the requirements' findings, as every composition leads with them (show_requirements
// keeps them as files land), then every finding reported since the last validation; the gate empty
// (gate_findings), the marks made again.
void ProblemsService::compose_held() {
	if (!view_.project.scan || !view_.project.requirements) return;
	std::vector<Diagnostic> rows = view_.project.scan->diagnostics;
	const std::vector<Diagnostic> &requirements = view_.project.requirements->diagnostics;
	rows.insert(rows.end(), requirements.begin(), requirements.end());
	const size_t composed = rows.size();
	for (const Reported &reported : reported_) rows.push_back(reported.finding);
	gate_size_ = 0;
	gate_tail_ = composed;
	trailing_ = rows.size() - composed;
	composed_ = Composed();
	composed_.made = true;
	composed_.rows = rows.size();
	composed_.scan = view_.project.scan->diagnostics;
	composed_.requirements = requirements;
	lead_scan_ = composed_.scan;
	lead_requirements_ = composed_.requirements;
	if (rows != view_.findings.diagnostics) {
		view_.findings.diagnostics = std::move(rows);
		core_.touch(ViewConcern::Findings);
	}
	mark_rows();
	show_validation();
}

void ProblemsService::release_base_files() {
	base_files_.reset();
	assets_->set_base(nullptr);
	originals_->clear();
	originals_needed_ = false;
	show_originals();
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
				base_files_, *graph_, validation_cache_, readings_);
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
	// The install is validated for the files a row may be the game's own in (problem_query.h's
	// original_candidate): a finding on a file of a name the install serves, each name validated once (the
	// install's files of those names and what their findings read, session/original_files.h). The install is
	// SessionCore::original_install's: the one the project imports from, whose names the session lists
	// (retail_files), or for a stock expansion the game as it ships, its names listed here once while that view
	// stands. Its names normalized once while its listing stands (a composition asks every row's).
	const InstallSpec install = core_.original_install();
	// A view mounting an expansion the project does not build on is a stock expansion's (it builds on a base
	// project, mounting none); any other is the one the project imports from.
	const bool imported_from = install.root.empty() || install.expansion == view_.project.document->expansion.builds_on;
	if (!imported_from && install != original_listed_) {
		original_listed_ = install;
		original_names_ = list_install_file_names(install);
	}
	const std::vector<std::string> &listing = imported_from ? view_.project.retail_files : original_names_;
	if (served_listing_ != listing) {
		served_listing_ = listing;
		served_.clear();
		for (const std::string &name : served_listing_) served_.insert(pff::normalized_logical_name(name));
	}
	std::vector<std::string> names;
	for (const Diagnostic &d : view_.findings.diagnostics)
		if (original_candidate(d))
			if (std::string name = served_name(d); served_.count(name)) names.push_back(std::move(name));
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	originals_needed_ = !names.empty();
	// Another install or game forgets what was found (the view says so at once); a new scan looks at the
	// install's folder again.
	originals_->want(install, view_.project.document, view_.project.scan.get(), names);
	show_originals();
}

bool ProblemsService::step_originals(uint64_t bytes) {
	if (!originals_needed_ || originals_->settled()) return true;
	// The install validated as a project of its own, nothing of the session's read (the polls step this
	// only once no validation of the project is due or under way).
	const int64_t started = steady_clock_us();
	const bool done = originals_->step(bytes);
	show_originals();
	spent_.originals_us += steady_clock_us() - started;
	if (done) show_validation(); // the counts stand from here
	return done;
}

void ProblemsService::show_originals() {
	if (view_.findings.originals == originals_->data()) return;
	view_.findings.originals = originals_->data();
	mark_rows();
	core_.touch(ViewConcern::Findings);
}

void ProblemsService::mark_rows() {
	BlockerMemo &memo = *blocker_memo_;
	if (view_.project.open && view_.project.scan && view_.project.requirements && view_.project.document) {
		// The plan a build would make (ADR 0046 S16): the project's target, an expansion's gate over its base
		// game's names, and the game's own bytes packed as stored; its refusals alone (plan_blockers), planned
		// again only when what they are made of moved. The gate is compared where it stands among the rows
		// (gate_findings' range, none when the rows no longer hold it), never composing, and copied only when
		// it moved.
		const std::vector<Diagnostic> &rows = view_.findings.diagnostics;
		const bool held = gate_size_ + gate_tail_ + trailing_ <= rows.size();
		const auto end = held ? rows.end() - static_cast<std::ptrdiff_t>(gate_tail_ + trailing_) : rows.end();
		const auto begin = held ? end - static_cast<std::ptrdiff_t>(gate_size_) : rows.end();
		if (size_t(end - begin) != memo.gate.size() || !std::equal(begin, end, memo.gate.begin())) {
			memo.gate.assign(begin, end);
			memo.made = false;
		}
		const BuildTarget target = core_.build_target();
		const ShippedFiles shipped = core_.shipped_files(memo.gate);
		const uint64_t serial = view_.project.scan->serial();
		if (!memo.same(serial, view_.project.requirements, target, view_.project.base_files, shipped)) {
			const BaseNames base{&view_.project.base_files};
			++blocker_plans_;
			blockers_ = plan_blockers(core_.paths(), *view_.project.scan, *view_.project.requirements, memo.gate, target,
			                          &base, &shipped);
			memo.made = true;
			memo.serial = serial;
			memo.requirements = view_.project.requirements;
			memo.target = target;
			memo.base_files = view_.project.base_files;
			memo.shipped = shipped;
		}
	} else {
		blockers_.clear();
		*blocker_memo_ = BlockerMemo();
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
	const int64_t started = steady_clock_us();
	compose_rows_now(keep_reported);
	spent_.compose_us += steady_clock_us() - started;
}

void ProblemsService::compose_rows_now(bool keep_reported) {
	read_once_ = true;
	const bool moved = pass_->validation.moved() || pass_->checks_moved || moved_since_composed_;
	moved_since_composed_ = false;
	pass_.reset();
	const std::vector<Diagnostic> open = core_.documents().findings();
	const ProjectFindingsInput input{core_.paths(),        *view_.project.document,    *view_.project.scan,
	                                 *view_.project.requirements, view_.documents.open, view_.activity.boot_missing,
	                                 play_findings_,              open,                 build_findings_,
	                                 base_files_.get()};
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
	return made && rows == rows_now && base == input.base && scan == input.scan.diagnostics &&
	       requirements == input.requirements.diagnostics && boot_missing == input.boot_missing &&
	       play == input.play && open == input.open_findings && build == input.build;
}

void ProblemsService::Composed::keep(const ProjectFindingsInput &input, size_t rows_now) {
	made = true;
	rows = rows_now;
	base = input.base;
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
	base_files_.reset();
	checks_->clear();
	validation_cache_ = ValidationCache();
	composed_ = Composed();
	lead_scan_.clear();
	lead_requirements_.clear();
	gate_size_ = gate_tail_ = trailing_ = 0;
	reported_.clear();
	validation_due_ = false;
	held_ = false;
	read_once_ = false;
	graph_scan_.reset();
	pass_.reset();
	moved_since_composed_ = false;
	readings_.clear();
	originals_->clear();
	originals_needed_ = false;
	original_listed_ = InstallSpec();
	original_names_.clear();
	blockers_.clear();
	*blocker_memo_ = BlockerMemo();
	view_.findings.marks.reset();
	show_originals();
	show_validation();
}

} // namespace opennova::editor
