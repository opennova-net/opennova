#include <editor/session/build_operation.h>

#include <utility>

#include <editor/session/project_session.h>

namespace opennova::editor {

BuildOperation::BuildOperation(BuildPlan plan, std::string output_root, std::vector<std::string> protected_dirs,
		std::vector<Diagnostic> gate, bool then_play) :
		run_(std::move(plan), std::move(output_root), std::move(protected_dirs)),
		gate_(std::move(gate)),
		then_play_(then_play) {}

OperationProgress BuildOperation::progress() const {
	return {run_.bytes_done(), run_.bytes_total(), OperationUnit::Bytes, run_.label()};
}

bool BuildOperation::join(const EditorRequest &request) {
	if (request.kind == EditorRequestKind::Play) {
		then_play_ = true;
		return true;
	}
	return request.kind == EditorRequestKind::Build;
}

OperationOutcome BuildOperation::finish(SessionCore &core) {
	return core.absorb_build(run_.report(), gate_, then_play_);
}

} // namespace opennova::editor
