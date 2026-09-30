#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/import/import_run.h>
#include <editor/model/diagnostic.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_run.h>
#include <editor/run/process_platform.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/view_revisions.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

class DocumentSet;
class EditorPreferences;
class ImportController;
class PlayController;
class ProblemsService;
class RenameController;
class UnsavedGuard;

// The project session's core (ADR 0046 S13 A2): what every part of the session shares. The open
// project (its paths, its local settings, its document) and what opens, refreshes, sets up and
// closes it; the one operation slot, its poll budget and the build it runs; the view (the Output
// log and the revisions in it), which only touch() moves a counter of; the outcome of the request
// being served; and the editor's preferences. The parts (DocumentSet, ProblemsService,
// PlayController, ImportController, RenameController, UnsavedGuard) each hold the core and reach
// one another through it, so a composition calls the part that serves it (a rename's close and
// reload call DocumentSet), never the session's handle(): handle() is entered once per request
// from outside, and that request's outcome is the one every part adds to. An operation's finish
// absorbs its work through the core (SessionOperation::finish). Single-threaded: the session's
// owner calls it between two frames' requests.
class SessionCore {
public:
	SessionCore(ProcessPlatform &platform, EditorPreferences &preferences);
	SessionCore(const SessionCore &) = delete;
	SessionCore &operator=(const SessionCore &) = delete;

	// The parts, wired once by the session that owns them all (ProjectSession), before anything
	// else is asked of the core.
	struct Parts {
		DocumentSet *documents = nullptr;
		ProblemsService *problems = nullptr;
		PlayController *play = nullptr;
		ImportController *imports = nullptr;
		RenameController *renames = nullptr;
		UnsavedGuard *guard = nullptr;
	};
	void bind(const Parts &parts) { parts_ = parts; }
	DocumentSet &documents() const { return *parts_.documents; }
	ProblemsService &problems() const { return *parts_.problems; }
	PlayController &play() const { return *parts_.play; }
	ImportController &imports() const { return *parts_.imports; }
	RenameController &renames() const { return *parts_.renames; }
	UnsavedGuard &guard() const { return *parts_.guard; }

	// Once, when the session starts: the preferences read (a store that cannot be read is a finding,
	// the defaults in effect) and shown, no project open.
	void start();

	SessionView &view() { return view_; }
	const SessionView &view() const { return view_; }
	ProcessPlatform &platform() const { return platform_; }
	EditorPreferences &preferences() const { return preferences_; }
	const ProjectPaths &paths() const { return paths_; }
	const LocalSettings &local() const { return local_; }

	// --- the request being served ----------------------------------------------------------

	// The request from outside being served, for as long as the scope lives (handle(), before its
	// validation): its outcome starts empty, and what is reported meanwhile is its outcome's. No
	// part enters handle(); a request that reaches it while one is served (a device's or a
	// source's callback) asserts in a debug build and is served inside the outer one, its findings
	// the outer outcome's, which it neither empties nor ends. Leaving the scope, however it is
	// left, ends only the request it began.
	class RequestScope {
	public:
		explicit RequestScope(SessionCore &core);
		~RequestScope();
		RequestScope(const RequestScope &) = delete;
		RequestScope &operator=(const RequestScope &) = delete;
		// True for the request from outside, false for one served inside it.
		bool outermost() const { return outermost_; }

	private:
		SessionCore &core_;
		bool outermost_;
	};
	const ActionOutcome &outcome() const { return outcome_; }
	ActionOutcome &outcome() { return outcome_; }

	// A change of `concern` in the view: its counter and `any` move (view_revisions.h), and the
	// previews follow the active document and the selection. The only way the session moves a
	// counter.
	void touch(ViewConcern concern) {
		view_.documents.update_previews();
		view_.revisions.touch(concern);
	}
	// A line in Output.
	void note(std::string line);
	// A finding: a Problems row (ProblemsService::add_reported, which never validates), the
	// request's outcome when one is served, and a line in Output.
	void report(const Diagnostic &d);
	// A finding raised while serving a request is that request's outcome; one raised by a poll (the
	// build finishing, the game's log) belongs to no request.
	void record_outcome(const Diagnostic &d);
	// A request that cannot run now: a warning, and the request did nothing.
	void refuse_now(CoreFinding code, const std::string &message, const std::string &asset = std::string());

	// --- the operation slot ----------------------------------------------------------------

	const OperationSlot &operations() const { return operations_; }
	OperationSlot &operations() { return operations_; }
	void set_poll_budget(const PollBudget &budget) { poll_budget_ = budget; }
	// `operation` started in the slot: its id, 0 while another runs.
	uint64_t start_operation(std::unique_ptr<SessionOperation> operation);
	// The running operation's steps within the poll's budget (the poll's).
	void step_operation();
	// True when what reads `reads` and writes `writes` conflicts with the running operation.
	bool busy_for(Holds reads, Holds writes) const;
	// "Wait for the build to finish, or cancel it, first.": `until` ends it.
	std::string busy_message(const std::string &until) const;
	void refuse_busy(const std::string &asset);
	// The running operation cancelled between two steps (`asked`: CancelOperation's own words);
	// true when none runs now (none ran, or it was cancelled), false when it cannot be cancelled.
	bool cancel_operation(bool asked);
	// The operation the poll found done, finished: the view learns what it came to.
	void finish_operation();
	void show_operation();

	// --- the project ------------------------------------------------------------------------

	bool new_project(const std::string &dir, const std::string &title);
	bool open_project(const std::string &dir);
	// The open project closed, its operation cancelled first; false (refused, said why, nothing
	// closed) when that operation cannot be cancelled.
	bool close_project();
	ImportRunResult refresh(bool force_import = false, const std::string &only = std::string());
	void apply_project_settings(const ProjectSettingsChange &change);
	void create_missing(const std::vector<std::string> &roles);
	// The recent project `root` dropped from the preferences (ForgetRecent).
	void forget_recent(const std::string &root);
	// Output's Clear: the output lines emptied.
	void clear_output();
	// Quit: the running operation cancelled first (one that cannot be keeps the editor open,
	// refused), then the view's quit_requested set, which the shell acts on.
	void quit();
	// The preferences kept by their store, and shown.
	void save_preferences();
	// The game install the editor imports from and plays in: the open project's (its local.json),
	// else the one the editor last chose.
	std::string game_install() const;
	// The requirement row of `role`, or null.
	const RequirementRow *requirement_row(const std::string &role) const;
	// The project file `file` names: the one at that project-relative path, else the first of that
	// logical name; null when the project has none.
	const AssetEntry *project_file(const std::string &file) const;

	// --- the build ---------------------------------------------------------------------------

	// The build as an operation (BuildOperation), `then_play` when a Play waits on it.
	void start_build(bool then_play);
	// A build's finish (BuildOperation): its report into the view, the findings its gate lacked,
	// the game started on it when a Play waits and it is good.
	OperationOutcome absorb_build(const BuildReport &result, const std::vector<Diagnostic> &gate, bool then_play);

private:
	ProcessPlatform &platform_;
	EditorPreferences &preferences_;
	Parts parts_;
	ProjectPaths paths_;
	LocalSettings local_;
	OperationSlot operations_;
	PollBudget poll_budget_ = kDefaultPollBudget;
	SessionView view_;
	ActionOutcome outcome_;
	bool in_request_ = false; // a request from outside is being served: what is reported is its outcome's
};

} // namespace opennova::editor
