#pragma once

#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project_build/build_run.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// The Play that waits on a build (ADR 0046 d8, S14): whether one does, and the mission it starts
// the game in ("" the game's menu), by its logical name.
struct PlayIntent {
	bool wanted = false;
	std::string mission;
};

// The Export that waits on a build (ADR 0046 S16): whether one does, and the folder it lands in (""
// the project's export folder; a relative one from the project's folder).
struct ExportIntent {
	bool wanted = false;
	std::string to;
};

// The build as the session's operation (ADR 0046 d8, S13 A1): a BuildRun stepped by bytes within
// each poll's budget, reading the project's files (a save, an import, a rename waits for it; an
// edit, an open or an import's preview does not). It keeps the Problems rows it was gated on, to
// tell its own findings from theirs, and the Play that waits on it (PlayIntent), set by the Play
// that started it or joined it (the last one's mission), so the game starts on the build the poll
// it lands, and a build a project switch cancels starts nothing. finish() hands the report to the
// session; nothing reaches the view before.
class BuildOperation : public SessionOperation {
public:
	BuildOperation(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs,
			std::vector<Diagnostic> gate, PlayIntent play, ExportIntent exported = ExportIntent());

	OperationKind kind() const override { return OperationKind::Build; }
	bool step(const StepBudget &budget) override { return run_.step(budget.bytes); }
	OperationProgress progress() const override;
	void cancel() override { run_.cancel(); }
	// A Play joining starts the game when the build lands, in the mission it names; an Export joining
	// copies the build where it names (a Build adds nothing).
	void join(const EditorRequest &request) override;
	OperationOutcome finish(SessionCore &core) override;

	const PlayIntent &play() const { return play_; }
	const ExportIntent &exported() const { return export_; }
	const BuildRun &run() const { return run_; }

private:
	BuildRun run_;
	std::vector<Diagnostic> gate_;
	PlayIntent play_;
	ExportIntent export_;
};

} // namespace opennova::editor
