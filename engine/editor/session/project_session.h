#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <base/io/json.h>
#include <editor/run/process_platform.h>
#include <editor/session/editor_request.h>
#include <editor/session/view/workspace_view.h>

namespace opennova::editor {

class Document;
class DocumentBase;
class OriginalFiles;
class PreferencesStore;
class SessionOperation;
class Viewports;
struct PollBudget;
struct SessionView;
struct ValidationStats;

// The one open project and everything the editor does to it (ADR 0046 d10): open and
// create, scan and evaluate, create-missing, build, play. Portable: the embedder hands it
// the process seam and the store its preferences live in and drains its view; a test drives
// it the same way. Requests come in typed (EditorRequest), the view goes out (SessionView).
// A long job is an operation (session_operation.h, S13 A1, A3): opening a project, a Rescan or
// a Reimport, an import's plan and its write, a rename's commit, a build; one at a time, stepped
// within each poll's budget so the window that hosts the session keeps drawing, and the request
// that starts one returns at once, its outcome naming it (an embedder that waits polls, or runs
// it to its end: run_operations). A request that conflicts with what an operation reads or
// writes is refused, joins it, supersedes it or cancels it as it commits, as its row says
// (request_kinds.h), in handle(). An edit (a Set, an Add, an Undo) leaves the project's
// validation due rather than running it, and no request runs it (S13 A3): the polls step it a
// file at a time within their budget however many edits the requests made (the first validation of
// a large project spreads over polls), an operation that reads the graph joins it, and a caller
// that waits runs it to its end with the operations (run_operations).
//
// A facade (S13 A2): the session is its parts (session_core.h, document_set.h,
// problems_service.h, play_controller.h, import_controller.h, rename_controller.h,
// unsaved_guard.h, disk_watch.h, editor_preferences.h), which call one another directly, so
// handle() is entered once per request from outside and the request's outcome is the one every
// part adds to. This header names none of them.
class ProjectSession {
public:
	ProjectSession(ProcessPlatform &platform, PreferencesStore &preferences);
	~ProjectSession();
	ProjectSession(const ProjectSession &) = delete;
	ProjectSession &operator=(const ProjectSession &) = delete;

	const SessionView &view() const;
	// What Play launches: asked now for what the view shows of the runtime, and again with a port
	// for the game's MCP endpoint each time the game is spawned, once its build lands.
	void set_launcher_source(PlayLauncherSource source);

	// True when the request was served here; false for the shell-only kinds. The view is
	// validated when it returns, unless a pump holds validation.
	bool handle(const EditorRequest &request);
	// A request in its wire form (S13 A5; the editor MCP's editor_request, the Shell's
	// request_json): read by session_json's editor_request_from_json, its edits named in the
	// record document it acts on (the one its path names, else the active one; a document of
	// another kind there holds no records to name, S13 D6, and the request is refused as it is
	// read), then handled. A kind that takes open_first, asking it with nothing open at its path,
	// is read once before the document opens (RequestNames::unresolved), so a request refused as it
	// is read opens nothing, and again in the document once open. The answer: {ok (it read),
	// served, error?, outcome (action_outcome_to_json: what it came to, with the records its edits
	// made, `added`, for an edit_record `made`, each label its batch gave to the record it named,
	// and for an edit_in_viewport's drag `gesture`, the gesture its batch carried), status,
	// view_revision (the view's clock after it)}. A shell row is not served here:
	// `shell`, when given, receives it (served false) for the shell to serve; the pickers need a
	// person and are refused by their kind before their fields are read.
	io::JsonValue handle_json(const io::JsonValue &json, EditorRequest *shell = nullptr);
	// What is asked of the session without a request (S13 A5, editor_queries.h): the query row
	// `name` answers `args` (an object of its params, or null for none), stamped with its
	// view_revision (run_query); null with `error` for a name no row has, args the row refuses (a
	// member it does not take, one it needs left out, a wrongly typed one, an offset or a limit out
	// of range), or a question it cannot answer (no such document or record).
	io::JsonValue query(std::string_view name, const io::JsonValue &args, std::string &error);
	// What the last request handled from outside came to (reset by the next one).
	const ActionOutcome &outcome() const;
	// True when the last request, an EditRecord, Copy, Cut, Paste or Duplicate, went through: its
	// outcome done and its edit applied (a Move that left a record where it is included: it changed
	// nothing, and nothing was wrong). False for one refused before its edit ran (an operation
	// holding the documents, a document not open).
	bool last_edit_ok() const;
	// How many requests handle() has taken: a test's count that the parts' compositions (a
	// rename's close and reload, the unsaved prompt's answer) never enter it.
	uint64_t handle_entries() const;

	// Once per frame, within one budget: the validation left due, stepped a file at a time first,
	// the running operation's steps within what is left (at least one), the child's state and the
	// game's log tail, then the operation that is done finished (a project opens, a build lands,
	// and the Play waiting on it starts).
	void poll();
	// How much a poll steps (kDefaultPollBudget; a test's ms 0 is one step of the validation and one
	// of the running operation per poll).
	void set_poll_budget(const PollBudget &budget);
	// Whether an Open reopens the documents a project was left with and a close or a quit keeps them in its
	// local settings (the editor's, true by default); a command line's run sets it false, its verbs then
	// neither loading the documents local.json lists nor writing them.
	void set_workspace_kept(bool kept);
	// The running operation, and those its finish starts, run to their end and finished, then the
	// validation they left due run to its end, and the check of which files its rows are about are the
	// game's own data (S15) (a test, a command line).
	void run_operations();
	// `operation` started in the slot as a request starts one: its id, 0 while another runs (a
	// test's, for an operation no request starts yet: one that cannot be cancelled, one that
	// reads or writes what a build does not).
	uint64_t start_operation(std::unique_ptr<SessionOperation> operation);

	// The open record document at `path` (project-relative, or a logical name), "" the active one:
	// null when none is open there, or the one open is not a record document (as_records). What
	// asks only the lifecycle (whether a file is open or unsaved, its JSON, the end of its edit
	// group) takes document_base_for, the open document of any kind (S13 D6).
	Document *document_for(const std::string &path = {});
	DocumentBase *document_base_for(const std::string &path = {});
	bool documents_dirty() const;
	bool project_open() const;
	// What the last validation read: the closed files it loaded and reused.
	const ValidationStats &validation_stats() const;
	// How many times the Problems rows were composed: a validation that finds nothing they are
	// made of moved composes none (for the tests).
	size_t problems_compositions() const;
	// The game's own data's baseline (S15, session/original_files.h): the install validated once (for the
	// tests and the measure).
	const OriginalFiles &originals() const;
	// How many project files the last scan read: every one by a refresh (an Open, a Rescan), those
	// a Save, a create or a rename's commit touched by its update (for the tests).
	size_t files_scanned() const;
	// The directory the running game uses ("" when none): the build never prunes it.
	std::string running_build_dir() const;

	// The viewports (S13 V5, preview/viewports.h), as the Shell's devices drive them
	// (ViewportDeviceCache: attached, followed, their actions taken, their reports given). The view
	// shares them const (DocumentsView::viewports); their state changes only by SetViewport.
	Viewports &viewports();
	// `seconds` of the Shell's frames pass: the preview clock runs while it plays.
	void advance(double seconds);
	// What the Shell's player made of the sound play `serial` asked (the workspace's sound, play_sound): it
	// plays, it played through, it failed (`error` why). A report of a play stopped or replaced since is passed
	// over; one that moves the sound moves the Workspace concern.
	void report_sound(uint64_t serial, WorkspaceView::SoundState state, const std::string &error = std::string());

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::editor
