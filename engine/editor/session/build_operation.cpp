#include <editor/session/build_operation.h>

#include <utility>

#include <editor/session/session_core.h>

namespace opennova::editor {

BuildOperation::BuildOperation(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs,
		std::vector<Diagnostic> gate, PlayIntent play) :
		run_(std::move(plan), std::move(output_root), std::move(protected_dirs)),
		gate_(std::move(gate)),
		play_(std::move(play)) {}

OperationProgress BuildOperation::progress() const {
	return {run_.bytes_done(), run_.bytes_total(), OperationUnit::Bytes, run_.label()};
}

void BuildOperation::join(const EditorRequest &request) {
	if (request.kind != EditorRequestKind::Play) return;
	play_.wanted = true;
	play_.mission = request.mission;
}

OperationOutcome BuildOperation::finish(SessionCore &core) {
	return core.absorb_build(run_.report(), gate_, play_);
}

} // namespace opennova::editor
