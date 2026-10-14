#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project/play_mode.h>
#include <editor/project_build/build_run.h>
#include <editor/project_build/export_build.h>
#include <editor/run/launch_plan.h>
#include <editor/run/play_start.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// The Play that waits on a build (ADR 0046 d8, S14): whether one does, and the mission it starts
// the game in ("" the game's menu), by its logical name.
struct PlayIntent {
	bool wanted = false;
	std::string mission;
	// The game's window started behind the others (play's behind, the MCP gaps lane: LaunchPlan::behind).
	bool behind = false;
	// The run directory emptied first of what the runs before wrote there (play's fresh: RunTake::fresh).
	bool fresh = false;
	// Where the game's player starts (play's start, DI-26: Play from here).
	mission::PlayerStart start;
	// How it runs, resolved as the Play was asked (play's play_mode, else the project's own then:
	// PlayController::intent_of), so a setting changed while the build packs does not change the Play.
	PlayMode mode = PlayMode::Runtime;
};

// The Export that waits on a build (ADR 0046 S16): whether one does, and the folder it lands in (""
// the project's export folder; a relative one from the project's folder).
struct ExportIntent {
	bool wanted = false;
	std::string to;
};

// What an Export that waits on a build copies, once the build has landed: its request (true), or
// what refuses it (false, `refused` holding the findings: a folder inside the project, a runtime to ship
// that Play does not run). The session makes it (SessionCore::export_request).
using ExportResolver = std::function<bool(const ExportIntent &intent, const BuildReport &built, ExportRequest &request,
                                          ExportReport &refused)>;

// What a Play that waits on a build copies into the copy cache before it stages, once the build has landed
// (S26 C16: an expansion's base game where the run directory's volume cannot link the install's files): the fill,
// or null for none. The session makes it (PlayController::copy_fill).
using PlayFillResolver = std::function<std::unique_ptr<InstallCopyFill>(const PlayIntent &intent, const BuildReport &built)>;

// The build as the session's operation (ADR 0046 d8, S13 A1): a BuildRun stepped by bytes within
// each poll's budget, reading the project's files (a save, an import, a rename waits for it; an
// edit, an open or an import's preview does not). It keeps the Problems rows it was gated on, to
// tell its own findings from theirs, and the Play that waits on it (PlayIntent), set by the Play
// that started it or joined it (the last one's mission), so the game starts on the build the poll
// it lands, and a build a project switch cancels starts nothing. An Export that waits on it
// (ExportIntent) copies the build once it has landed, stepped by the same budget (ExportRun: the
// operation's progress its bytes, a cancel leaving the folder as it was). A Play that waits on it has the
// install's files it needs copied into the copy cache next, stepped by the same budget (InstallCopyFill, S26 C16):
// the Play then stages its run directory at once, linking them, where it once copied them in one frame; a fill
// that failed starts no game (a cancel, the whole operation's, finishes nothing), the whole copies kept. finish() hands the reports to the session; nothing reaches the view before.
class BuildOperation : public SessionOperation {
public:
	BuildOperation(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs,
			std::vector<Diagnostic> gate, PlayIntent play, ExportIntent exported = ExportIntent(),
			ExportResolver resolve_export = ExportResolver(), PlayFillResolver resolve_fill = PlayFillResolver());

	OperationKind kind() const override { return OperationKind::Build; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	void cancel() override;
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
	std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
	ExportIntent export_;
	ExportResolver resolve_export_;
	std::unique_ptr<ExportRun> export_run_;
	std::unique_ptr<ExportReport> export_refused_; // what refused the export, when something did
	PlayFillResolver resolve_fill_;
	std::unique_ptr<InstallCopyFill> fill_; // the Play's copies, once planned
	bool fill_planned_ = false;
	bool cancelled_ = false;
};

} // namespace opennova::editor
