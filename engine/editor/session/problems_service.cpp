#include <editor/session/problems_service.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <base/io/json.h>
#include <editor/preview/menu_render_check.h>
#include <editor/project/project_findings.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

ProblemsService::ProblemsService(SessionCore &core) :
		core_(core), view_(core.view()), render_check_(std::make_shared<MenuRenderCheck>()) {
	view_.findings.graph = graph_;
	view_.findings.assets = assets_;
	view_.findings.render_check = render_check_;
}

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
	validation_due_ = false;
	const uint64_t graph_generation = graph_->generation();
	const std::vector<Diagnostic> open = core_.documents().findings();
	const ProjectFindingsInput input{core_.paths(),        *view_.project.document,    *view_.project.scan,
	                                 *view_.project.requirements, view_.documents.open, view_.activity.boot_missing,
	                                 play_findings_,              open,                 build_findings_};
	// The graph, each file's own findings and the render check first: when none of them moved, no
	// other input the rows are made of did, no reported finding waits on this validation and the
	// rows are as it left them, they stand. No row is composed, copied or compared then; the small
	// inputs are compared with their copies (Composed::same), and the open documents' own findings
	// are made again for it.
	const bool moved = refresh_project_findings(input, *graph_, validation_cache_, *render_check_, *assets_);
	if (!moved && reported_.empty() && trailing_ == 0 && composed_.same(input, view_.findings.diagnostics.size())) return;
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
	if (graph_->generation() != graph_generation) core_.touch(ViewConcern::Graph);
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
	reported_.push_back({d, validation_due_});
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
}

std::string ProblemsService::problems_json(const std::string &text) {
	io::JsonValue json;
	std::string error;
	ProblemQuery query;
	size_t offset = 0, limit = 0;
	if (!io::json_parse(text, json, error) || !problem_query_from_json(json, query, offset, limit, error)) {
		io::JsonValue answer = io::JsonValue::make_object();
		answer.set("error", io::JsonValue::make_string(error));
		return io::json_write(answer);
	}
	return io::json_write(problems_to_json(view_, query_cache_.answer(query, view_), offset, limit, fix_cache_));
}

} // namespace opennova::editor
