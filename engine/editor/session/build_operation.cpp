#include <editor/session/build_operation.h>

#include <utility>

#include <editor/session/session_core.h>

namespace opennova::editor {

BuildOperation::BuildOperation(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs,
		std::vector<Diagnostic> gate, PlayIntent play, ExportIntent exported) :
		run_(std::move(plan), std::move(output_root), std::move(protected_dirs)),
		gate_(std::move(gate)),
		play_(std::move(play)),
		export_(std::move(exported)) {}

OperationProgress BuildOperation::progress() const {
	return {run_.bytes_done(), run_.bytes_total(), OperationUnit::Bytes, run_.label()};
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
}

OperationOutcome BuildOperation::finish(SessionCore &core) {
	// How long it took, start to finish: what the build panel says (the UX round's problems lane).
	BuildReport report = run_.report();
	report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
	return core.absorb_build(report, gate_, play_, export_);
}

} // namespace opennova::editor
