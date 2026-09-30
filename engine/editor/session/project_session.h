#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <base/io/json.h>
#include <editor/run/process_platform.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class Document;
class PreferencesStore;
class SessionOperation;
struct PollBudget;
struct SessionView;
struct ValidationStats;

// The one open project and everything the editor does to it (ADR 0046 d10): open and
// create, scan and evaluate, create-missing, build, play. Portable: the embedder hands it
// the process seam and the store its preferences live in and drains its view; a test drives
// it the same way. Requests come in typed (EditorRequest), the view goes out (SessionView).
// A long job is an operation (session_operation.h, S13 A1): one at a time, stepped within
// each poll's budget so the window that hosts the session keeps drawing while a project
// packs; a request that conflicts with what it reads or writes is refused, joins it,
// supersedes it or cancels it as it commits, as its row says (request_kinds.h), in
// handle(). An edit (a Set, an Add, an Undo) leaves the project's validation due rather than
// running it: a request from outside returns validated, and a pump that holds validation
// (hold_validation) validates once, at its poll, however many edits its requests made; Save,
// Build, Rescan and the other project requests validate at once.
//
// A facade (S13 A2): the session is its parts (session_core.h, document_set.h,
// problems_service.h, play_controller.h, import_controller.h, rename_controller.h,
// unsaved_guard.h, editor_preferences.h), which call one another directly, so handle() is
// entered once per request from outside and the request's outcome is the one every part adds
// to. This header names none of them.
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
	// document it acts on (the one its path names, else the active one; opened first when it asks,
	// open_first, and is not open), then handled. The answer: {ok (it read), served, error?,
	// outcome (action_outcome_to_json: what it came to, with the records its edits made, `added`,
	// and for an edit_record `made`, each label its batch gave to the record it named), status,
	// revision (the view's any)}. A shell row is not served here: `shell`, when given, receives it
	// (served false) for the shell to serve; the pickers need a person and are refused by their
	// kind before their fields are read.
	io::JsonValue handle_json(const io::JsonValue &json, EditorRequest *shell = nullptr);
	// What is asked of the session without a request (S13 A5, editor_queries.h): the query row
	// `name` answers `args` (an object of its params, or null for none), stamped with the revision
	// of the concern it reads; null with `error` for a name no row has, args the row refuses (a
	// member it does not take, one it needs left out, a wrongly typed one, an offset or a limit out
	// of range), or a question it cannot answer (no such document or record).
	io::JsonValue query(std::string_view name, const io::JsonValue &args, std::string &error);
	// A pump starts (the shell: the requests its windows raised this frame): the requests
	// handled until the next poll() leave their validation to that poll, so a burst of
	// edits (typing, a drag) validates once.
	void hold_validation();
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

	// Once per frame: the validation the frame's edits left due, the running operation's steps
	// within the poll's budget, the child's state and the game's log tail, then the operation
	// that is done finished (a build lands, and the Play waiting on it starts).
	void poll();
	// How much a poll steps the running operation (kDefaultPollBudget; a test's ms 0 is one
	// step per poll).
	void set_poll_budget(const PollBudget &budget);
	// The running operation, and those its finish starts, run to their end and finished (a
	// test, a command line).
	void run_operations();
	// `operation` started in the slot as a request starts one: its id, 0 while another runs (a
	// test's, for an operation no request starts yet: one that cannot be cancelled, one that
	// reads or writes what a build does not).
	uint64_t start_operation(std::unique_ptr<SessionOperation> operation);

	Document *document_for(const std::string &path = {});
	bool documents_dirty() const;
	bool project_open() const;
	// What the last validation read: the closed files it loaded and reused.
	const ValidationStats &validation_stats() const;
	// The directory the running game uses ("" when none): the build never prunes it.
	std::string running_build_dir() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::editor
