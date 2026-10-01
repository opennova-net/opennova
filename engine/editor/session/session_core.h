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
class OpenOperation;
class PlayController;
class ProblemsService;
class ProjectRefresh;
class RenameController;
class UnsavedGuard;
class Viewports;

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
	// The viewports (S13 V5): shared const on the view (DocumentsView::viewports), kept with the
	// view's previews' targets at every touch; the Shell's devices drive their follow.
	Viewports &viewports() { return *viewports_; }
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

	private:
		SessionCore &core_;
		bool outermost_;
	};
	const ActionOutcome &outcome() const { return outcome_; }
	ActionOutcome &outcome() { return outcome_; }

	// A change of `concern` in the view: its counter and `any` move (view_revisions.h), the
	// previews follow the active document and the selection, and the viewports keep with them (a
	// target's viewport made, a closed document's gone). The only way the session moves a counter.
	void touch(ViewConcern concern);
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
	const PollBudget &poll_budget() const { return poll_budget_; }
	// `operation` started in the slot: its id, 0 while another runs.
	uint64_t start_operation(std::unique_ptr<SessionOperation> operation);
	// The running operation's steps within `budget` (the poll's: what its validation steps left of
	// its budget, at least one step).
	void step_operation(const PollBudget &budget);
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

	// A project of `game` (a gameprofile code; "" the default, jo) made in `dir` (titled `title`,
	// else the folder's name), then opened as open_project opens it, with its import pass unless
	// `import_pass` is false.
	bool new_project(const std::string &dir, const std::string &title, const std::string &game = std::string(),
	                 bool import_pass = true);
	// The project in `dir` read (its document, its local settings; `game_install` in place of the
	// install they name, for the session alone, when given), then, the open one closed, opened as an
	// operation (OpenOperation, S13 A3: the game install's names, the import pass unless `import_pass`
	// is false, the scan, the requirements), whose id the request's outcome names: the view holds
	// nothing of it until it finishes (absorb_open). False, the open project kept, for a folder that
	// holds none.
	bool open_project(const std::string &dir, bool import_pass = true, const std::string &game_install = std::string());
	// An Open's finish: the project it read is the open one, with its files as it read them.
	OperationOutcome absorb_open(OpenOperation &open);
	// The open project closed, its operation cancelled first; false (refused, said why, nothing
	// closed) when that operation cannot be cancelled.
	bool close_project();
	// The project's files read again as an operation (RefreshOperation): a Rescan, or a Reimport
	// (`force` over the sources `only` names); false, refused, while another runs.
	bool start_refresh(bool reimport = false, bool force = false, const std::string &only = std::string());
	// A refresh done (an Open's, a Refresh's, an import's, an import source's rename's): its
	// imports, scan and requirements are the view's, an Output line for each source it imported,
	// the validation left due. What its import pass came to.
	ImportRunResult absorb_refresh(ProjectRefresh &refresh);
	// The refresh run to its end now and the project validated (the build's, before it plans).
	void refresh_now();
	// The project files at `paths` read again alone (AssetScan::update: a Save's, a create's, a
	// rename's commit), the requirements evaluated again over the scan, the validation left due.
	void update_files(const std::vector<std::string> &paths);
	// How many files the last scan the view took read: every file of the project by a refresh (an
	// Open's, a Rescan's, an import's), those it named by an update (update_files).
	size_t files_scanned() const { return files_scanned_; }
	void apply_project_settings(const ProjectSettingsChange &change);
	void create_missing(const std::vector<std::string> &roles);
	// The recent project `root` dropped from the preferences (ForgetRecent).
	void forget_recent(const std::string &root);
	// Output's Clear: the output lines emptied.
	void clear_output();
	// SetViewport: the viewport over the document `path` names ("" the active one; by its path or
	// its logical name, as every request names one) changed as `change` (its JSON text) says
	// (Viewports::set); refused, nothing changed, with why (viewport.refused).
	void set_viewport(const std::string &path, const std::string &change);
	// EditInViewport (S13 V7): the request's drag or command planned by the viewport over the
	// document its path names (Viewports::resolve: followed first, so it plans over the document as
	// it is now), then each request the plan made served in order through its own row (an
	// EditRecord, its gesture's EndEdit, a SetViewport); a drag's gesture in the outcome. Refused,
	// nothing changed, with why (viewport.refused): neither or both of a drag and a command, no
	// viewport there, a plan the viewport refuses.
	void edit_in_viewport(const EditorRequest &request);
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

	// The build as an operation (BuildOperation), `then_play` when a Play waits on it; each build a
	// directory under `out_dir` ("" the project's .opennova/build/play; a relative one taken from
	// the project's folder; one inside the project but in its cache or its export folder refused).
	void start_build(bool then_play, const std::string &out_dir = std::string());
	// A build's finish (BuildOperation): its report into the view, the findings its gate lacked,
	// the game started on it when a Play waits and it is good.
	OperationOutcome absorb_build(const BuildReport &result, const std::vector<Diagnostic> &gate, bool then_play);

private:
	// The path of the document a viewport request names (its path or logical name; "" the active
	// one), the name as it came when none is open there.
	std::string viewport_document(const std::string &path);

	ProcessPlatform &platform_;
	EditorPreferences &preferences_;
	Parts parts_;
	ProjectPaths paths_;
	LocalSettings local_;
	OperationSlot operations_;
	PollBudget poll_budget_ = kDefaultPollBudget;
	SessionView view_;
	std::shared_ptr<Viewports> viewports_;
	ActionOutcome outcome_;
	size_t files_scanned_ = 0;
	bool in_request_ = false; // a request from outside is being served: what is reported is its outcome's
};

} // namespace opennova::editor
