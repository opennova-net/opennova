#include <editor/session/build_operation.h>

#include <utility>

#include <editor/session/session_core.h>

namespace opennova::editor {

BuildOperation::BuildOperation(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs,
		std::vector<Diagnostic> gate, PlayIntent play, ExportIntent exported, ExportResolver resolve_export,
		PlayFillResolver resolve_fill) :
		run_(std::move(plan), std::move(output_root), std::move(protected_dirs)),
		gate_(std::move(gate)),
		play_(std::move(play)),
		export_(std::move(exported)),
		resolve_export_(std::move(resolve_export)),
		resolve_fill_(std::move(resolve_fill)) {}

bool BuildOperation::step(const StepBudget &budget) {
	if (!run_.done()) {
		if (!run_.step(budget.bytes)) return false;
		// Landed: done unless an Export or a Play's copies follow (each asked at the next step).
		return !run_.report().ok || cancelled_ || (!export_.wanted && !(play_.wanted && resolve_fill_));
	}
	if (!run_.report().ok || cancelled_) return true;
	if (export_.wanted && !export_refused_) {
		if (!export_run_) {
			ExportRequest request;
			ExportReport refused;
			if (!resolve_export_ || !resolve_export_(export_, run_.report(), request, refused)) {
				export_refused_ = std::make_unique<ExportReport>(std::move(refused));
				return !(play_.wanted && resolve_fill_);
			}
			export_run_ = std::make_unique<ExportRun>(std::move(request));
		}
		if (!export_run_->done()) {
			if (!export_run_->step(budget.bytes)) return false;
			return !(play_.wanted && resolve_fill_) || cancelled_;
		}
	}
	// The Play's copies into the copy cache (C16), planned once the build (and its export) landed.
	if (!play_.wanted || !resolve_fill_) return true;
	if (!fill_planned_) {
		fill_planned_ = true;
		fill_ = resolve_fill_(play_, run_.report());
	}
	return !fill_ || fill_->step(budget.bytes);
}

OperationProgress BuildOperation::progress() const {
	if (fill_ && !fill_->cancelled()) return {fill_->bytes_done(), fill_->bytes_total(), OperationUnit::Bytes, fill_->label()};
	if (export_run_) return {export_run_->bytes_done(), export_run_->bytes_total(), OperationUnit::Bytes, export_run_->label()};
	return {run_.bytes_done(), run_.bytes_total(), OperationUnit::Bytes, run_.label()};
}

void BuildOperation::cancel() {
	cancelled_ = true;
	if (!run_.done()) run_.cancel();
	if (export_run_) export_run_->cancel();
	if (fill_) fill_->cancel();
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
	// A Play whose copies failed starts no game: its staging would copy the rest in one frame. (One cancelled is
	// the whole operation's cancel, which finishes nothing.)
	PlayIntent play = play_;
	const bool failed = fill_ && fill_->failed();
	if (failed) play.wanted = false;
	OperationOutcome outcome = core.absorb_build(report, gate_, play, export_, shipped);
	if (failed)
		core.report(make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                         "The game was not started: the game install's files could not be copied for the expansion (" +
		                                 fill_->reason() + ")."));
	return outcome;
}

} // namespace opennova::editor
