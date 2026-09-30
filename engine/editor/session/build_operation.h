#pragma once

#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project_build/build_run.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// The build as the session's operation (ADR 0046 d8, S13 A1): a BuildRun stepped by bytes within
// each poll's budget, holding the project's files (a save, an import, a rename waits for it; an
// edit does not). It keeps the Problems rows it was gated on, to tell its own findings from
// theirs, and whether a Play waits on it: `then_play`, set by the Play that started it or joined
// it, so the game starts on the build the poll it lands, and a build a project switch cancels
// starts nothing. finish() hands the report to the session; nothing reaches the view before.
class BuildOperation : public SessionOperation {
public:
	BuildOperation(BuildPlan plan, std::string output_root, std::vector<std::string> protected_dirs,
			std::vector<Diagnostic> gate, bool then_play);

	OperationKind kind() const override { return OperationKind::Build; }
	bool step(const StepBudget &budget) override { return run_.step(budget.bytes); }
	OperationProgress progress() const override;
	void cancel() override { run_.cancel(); }
	// A Build is served as it is; a Play too, the game then starting when the build lands.
	bool join(const EditorRequest &request) override;
	OperationOutcome finish(SessionCore &core) override;

	bool then_play() const { return then_play_; }
	const BuildRun &run() const { return run_; }

private:
	BuildRun run_;
	std::vector<Diagnostic> gate_;
	bool then_play_ = false;
};

} // namespace opennova::editor
