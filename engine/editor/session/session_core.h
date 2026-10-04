#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/import/import_run.h>
#include <editor/model/diagnostic.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_run.h>
#include <editor/project_build/export_build.h>
#include <editor/requirements/requirements.h>
#include <editor/run/process_platform.h>
#include <editor/session/editor_request.h>
#include <editor/session/original_bytes.h>
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
struct PlayIntent;
struct ExportIntent;
class RenameController;
class UnsavedGuard;
class TextureUseIndex;
class Viewports;

// How long a gesture of the wire's stays open with no sample (S13 V7): a client that began a drag
// and went away ends it, its Problems' validation no longer waiting on it.
inline constexpr int64_t kWireGestureLapseMs = 10000;

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
	// left, ends only the request it began. The status line a refused request leaves is that
	// request's (ADR 0046 S15): the next request served without a refusal replaces it with its own
	// line, or, saying nothing, clears it, so a refusal never reads as the outcome of what came after;
	// a `background` request (its row's: one the Shell sends of its own, S18) is no such request.
	class RequestScope {
	public:
		explicit RequestScope(SessionCore &core, bool background = false);
		~RequestScope();
		RequestScope(const RequestScope &) = delete;
		RequestScope &operator=(const RequestScope &) = delete;

	private:
		SessionCore &core_;
		bool outermost_;
		bool background_;
		std::string status_before_; // the line when the request arrived
	};
	const ActionOutcome &outcome() const { return outcome_; }
	ActionOutcome &outcome() { return outcome_; }

	// A change of `concern` in the view: its counter and `any` move (view_revisions.h), the
	// previews follow the active document and the selection, and the viewports keep with them (a
	// target's viewport made, a closed document's gone). The only way the session moves a counter.
	void touch(ViewConcern concern);
	// A line in Output.
	void note(std::string line);
	// A line of Output with `folded` under it (an import's files, the game's log: OutputLog), its absolute
	// index; and more folded under it later, its line made `line` (false when Output no longer holds it).
	uint64_t note_folded(std::string line, std::vector<std::string> folded);
	bool fold_into_note(uint64_t index, std::string line, std::vector<std::string> more);
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
	// else the folder's name) as `expansion` (ADR 0046 S16: its rule and the game install's
	// expansions weighed first; its version text made and, on the base game, its text table), then
	// opened as open_project opens it, with its import pass unless `import_pass` is false.
	bool new_project(const std::string &dir, const std::string &title, const std::string &game = std::string(),
	                 bool import_pass = true, const ProjectExpansion &expansion = ProjectExpansion());
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
	// What a program changed of the watched files (S18's external round trip, ExternalChanges) refreshed
	// alone, as an operation (ChangedSourcesOperation): the sources it names imported (the pass over them
	// alone), then the scan updated for those sources and the files; false, refused, while another runs.
	bool start_changed_refresh(ExternalChanges changes);
	// Its finish: the sources' imports and findings the view's in place of what they were, the scan updated
	// for them and the files that moved, the open documents of those files read again, the validation left
	// due.
	void absorb_changed(ImportRunResult &imports, const std::vector<std::string> &files);
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
	// (Viewports::set); a change of the clock alone with no path, the preview clock whatever document
	// is active (Viewports::set_clock, S13 V7). Refused, nothing changed, with why (viewport.refused).
	void set_viewport(const std::string &path, const std::string &change);
	// EditInViewport (S13 V7): the request's drag or command planned by the viewport over the
	// document its path names (Viewports::resolve, of the kind the drag or the command names:
	// followed first, so it plans over the document as it is now), then each request the plan made
	// served in order through its own row (an EditRecord, its gesture's EndEdit, a SetViewport); a
	// drag's gesture in the outcome, refused or not. A drag that keeps its gesture open (`end` false)
	// opens a gesture of the wire's in the document (its token the session's, whatever the sample
	// plans), which only the document's next drag naming it goes on with, from the point of the
	// picture its samples took the handle to (a canvas's drag goes on from its press); a refused
	// sample that ends it ends it all the same. Refused, nothing changed, with why (viewport.refused):
	// neither or both of a drag and a command, no viewport there, a gesture the document holds no open
	// one of the wire's of (or one of another record's handle), a plan the viewport refuses.
	void edit_in_viewport(const EditorRequest &request);
	// The gesture open in the document at `path` (the view's documents.gestures; its token 0 for
	// none): a drag of the wire's goes on only with one the wire opened.
	const OpenGesture &open_gesture(const std::string &path) const { return view_.documents.gesture_in(path); }
	// A request from outside arrives (ProjectSession::handle, before it is served): a gesture of the
	// wire's open in the document the request is on (its path's, else the active one where its row
	// names it) ends, unless the request is a drag naming it (S13 V7: S13 A3's rule, every edit group
	// ending before a request that starts an operation, widened to the wire's gestures).
	void request_arrives(const EditorRequest &request);
	// The poll's: a gesture of the wire's that had no sample for kWireGestureLapseMs (the platform's
	// clock) ends.
	void lapse_wire_gestures();
	// Quit: the running operation cancelled first (one that cannot be keeps the editor open,
	// refused), then the view's quit_requested set, which the shell acts on.
	void quit();
	// The preferences kept by their store, and shown.
	void save_preferences();
	// `item` first among the items recently placed for the open project's game (ADR 0046 S15; per game
	// since the polish, recent_items_game), shown at once; kept by the next poll's save_recent_items
	// (nothing to keep when it was first already, or with no project open).
	void remember_recent_item(int64_t item);
	// The view's recently placed items: the open project's game's (none with no project open).
	void show_recent_items();
	// The poll's (and a quit's, and the session's end): the recently placed items kept by the store when
	// they changed since; a store that cannot keep them says so in Output, failing no request.
	void save_recent_items();
	// The game install the editor imports from and plays in: the open project's (its local.json),
	// else the one the editor last chose.
	std::string game_install() const;
	// The game install's expansions read again into the view (ADR 0046 S16: the ones it mounts, with
	// the Mods list's name and description of each), when the install may have moved.
	void read_install_expansions();
	// The requirements of `doc` over `scan`, with the game install's expansions weighed
	// (evaluate_requirements: the project's expansion against them, listed).
	RequirementReport requirements_of(const ProjectDocument &doc, const AssetScan &scan) const;
	// The requirement row of `role`, or null.
	const RequirementRow *requirement_row(const std::string &role) const;
	// The project file `file` names, its case aside: the one at that project-relative path in any
	// case, or for a name alone the first of that logical name; null when the project has none, or
	// when two files answer to the path in another case (`ambiguous` then true).
	const AssetEntry *project_file(const std::string &file, bool *ambiguous = nullptr) const;

	// --- the build ---------------------------------------------------------------------------

	// The build as an operation (BuildOperation), with the Play that waits on it (`intent`: whether
	// one does, and the mission it starts the game in; refused before anything is built where Play
	// cannot run, or the mission is no .bms of the project); each build a directory under `out_dir`
	// ("" the project's .opennova/build/play; a relative one taken from the project's folder; one
	// inside the project but in its cache or its export folder refused); `rehash`, every file read
	// again, the build cache set aside (BuildPlan::rehash).
	// With the Export that waits on it (`exported`, ADR 0046 S16: the folder it lands in, refused
	// before anything is built when it lies inside the project but its export folder).
	void start_build(const PlayIntent &intent, const std::string &out_dir, bool rehash, const ExportIntent &exported);
	// A build's finish (BuildOperation): its report into the view, the findings its gate lacked,
	// the game started on it, in the Play's mission, when a Play waits and it is good; what the export
	// that waited on it came to (`shipped`: its ExportRun's report, or what refused it; null when none ran).
	OperationOutcome absorb_build(const BuildReport &result, const std::vector<Diagnostic> &gate, const PlayIntent &intent,
	                              const ExportIntent &exported, const ExportReport *shipped);
	// What an Export that waited on the build `built` copies (BuildOperation's ExportResolver): its folder
	// (export_folder), the runtime beside a standalone game when the project asks for it; false with
	// `refused` holding what refuses it.
	bool export_request(const ExportIntent &intent, const BuildReport &built, ExportRequest &request, ExportReport &refused);
	// The folder an Export lands in: `to` from the project's folder when relative, else the
	// project's export folder; "" with a finding reported when it lies inside the project but the
	// export folder (the next scan would list what it holds as the project's files).
	std::string export_folder(const std::string &to);
	// What the open project's build makes (ADR 0046 S16): its project.opennova's expansion (none: the
	// standalone game), over the project's game install, keyed by its game.
	BuildTarget build_target() const;
	// The files the build packs as the game ships them (ADR 0046 S16, ShippedFiles), its gate reading
	// them: of the files a gate row says do not serialize, those that are the game install's bytes
	// (OriginalBytes, asked here), less the open documents with unsaved edits.
	ShippedFiles shipped_files();
	// The same over the gate rows `gate` (the Problems rows' marking asks it without composing them).
	ShippedFiles shipped_files(const std::vector<Diagnostic> &gate);

private:
	// The path of the document a viewport request names (its path or logical name; "" the active
	// one), the name as it came when none is open there.
	std::string viewport_document(const std::string &path);

	// What a drag of the wire's keeps between its samples (S13 V7), by its document while its gesture
	// is open there: the gesture's token, the viewport's kind, the record and the handle its samples
	// drag, and the point of the picture its samples took the handle to (the viewport's units, before
	// any snap), which the next sample's `by` goes on from.
	struct WireDrag {
		uint64_t token = 0;
		ViewportKind kind = ViewportKind::kCount;
		NodeId id = 0;
		std::string handle;
		float x = 0.0f;
		float y = 0.0f;
	};
	// The wire's drag in the document at `path` while its gesture is open there (null for none; one
	// whose gesture ended another way forgotten).
	WireDrag *wire_drag(const std::string &path);
	// The gesture of the wire's open in the document at `path` ends (its EndEdit, as a client's last
	// sample would raise it).
	void end_wire_gesture(const std::string &path);
	// The project's own expansion files under the name `from`, renamed to the name `to`'s, and the project
	// document `project` (which names `to`) saved, all or nothing (ADR 0046 S16): every rename planned and
	// checked first (a target taken, a file another file names, a file open with unsaved edits refuses the
	// whole change, nothing written), then each file moved, then the document saved; a move or the save
	// that fails puts back every move made before it. True when the change is in, its open documents
	// read again at their new paths and the scan updated; false with `failures`, nothing changed.
	bool rename_expansion_files(const std::string &from, const std::string &to, const ProjectDocument &project,
	                            std::vector<Diagnostic> &failures);

	ProcessPlatform &platform_;
	EditorPreferences &preferences_;
	Parts parts_;
	ProjectPaths paths_;
	LocalSettings local_;
	OperationSlot operations_;
	PollBudget poll_budget_ = kDefaultPollBudget;
	SessionView view_;
	std::shared_ptr<Viewports> viewports_;
	std::shared_ptr<TextureUseIndex> texture_uses_; // the view's texture_uses, cleared with the project
	ActionOutcome outcome_;
	std::map<std::string, WireDrag> wire_drags_;
	OriginalBytes original_bytes_; // which files are the install's bytes (shipped_files)
	std::vector<std::string> install_expansion_names_; // the game install's expansions, by folder name
	size_t files_scanned_ = 0;
	bool in_request_ = false; // a request from outside is being served: what is reported is its outcome's
	std::string refusal_status_; // the status line the last refused request left, until a request is served
	bool recent_items_unsaved_ = false; // the recently placed items changed since the store kept them
};

} // namespace opennova::editor
