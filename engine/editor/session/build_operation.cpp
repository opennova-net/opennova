#include <editor/session/build_operation.h>

#include <utility>

#include <editor/session/session_core.h>

namespace opennova::editor {

BuildOperation::BuildOperation(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs,
		std::vector<Diagnostic> gate, PlayIntent play, ExportIntent exported, ExportResolver resolve_export) :
		run_(std::move(plan), std::move(output_root), std::move(protected_dirs)),
		gate_(std::move(gate)),
		play_(std::move(play)),
		export_(std::move(exported)),
		resolve_export_(std::move(resolve_export)) {}

bool BuildOperation::step(const StepBudget &budget) {
	if (!run_.done()) {
		if (!run_.step(budget.bytes)) return false;
		return !export_.wanted || !run_.report().ok || cancelled_ ? true : false;
	}
	if (!export_.wanted || !run_.report().ok || cancelled_ || export_refused_) return true;
	if (!export_run_) {
		ExportRequest request;
		ExportReport refused;
		if (!resolve_export_ || !resolve_export_(export_, run_.report(), request, refused)) {
			export_refused_ = std::make_unique<ExportReport>(std::move(refused));
			return true;
		}
		export_run_ = std::make_unique<ExportRun>(std::move(request));
	}
	return export_run_->step(budget.bytes);
}

OperationProgress BuildOperation::progress() const {
	if (export_run_) return {export_run_->bytes_done(), export_run_->bytes_total(), OperationUnit::Bytes, export_run_->label()};
	return {run_.bytes_done(), run_.bytes_total(), OperationUnit::Bytes, run_.label()};
}

void BuildOperation::cancel() {
	cancelled_ = true;
	if (!run_.done()) run_.cancel();
	if (export_run_) export_run_->cancel();
}

void BuildOperation::join(const EditorRequest &request) {
	if (request.kind == EditorRequestKind::Export) {
		export_.wanted = true;
		export_.to = request.export_dir;
		return;
	}
	if (request.kind != EditorRequestKind::Play) return;
	play_.wanted = true;
	play_.mission = request.mission;
	play_.behind = request.behind;
	play_.fresh = request.fresh;
	play_.start = request.start;
	// Resolved by the session as it joined (join_operation: the request's, else the project's).
	play_.mode = request.play_mode.value_or(PlayMode::Runtime);
}

OperationOutcome BuildOperation::finish(SessionCore &core) {
	// How long it took, start to finish: what the build panel says (the UX round's problems lane).
	BuildReport report = run_.report();
	report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
	const ExportReport *shipped = export_run_ ? &export_run_->report() : export_refused_.get();
	return core.absorb_build(report, gate_, play_, export_, shipped);
}

} // namespace opennova::editor
