#pragma once

// The session's part for NovaWorld (ADR 0069): the account the editor acts as on a
// NovaWorld service, the projects it may read, and the open project as a working copy of the one
// its project file names (ProjectDocument::novaworld). It serves the NovaWorld requests
// (request_kinds.cpp: novaworld_sign_in, a person's alone, and the rest, the editor MCP's too) over
// the host's transport (ProjectSession::set_novaworld_transport: the Shell's HTTP, a test's fake)
// and the host's account store (set_novaworld_accounts), and shows what it knows in the view's
// NovaWorldView (the NovaWorld concern). A sign-in, the project list, the open project's ask and a
// put's are calls it polls itself (poll: they hold nothing of the session's); a check, a pull (a
// get's too) and a push run as the session's operations (NovaWorldOperation: the busy gate weighs
// them as their rows say), and their finish comes back here (finish_job). The folder's working copy
// on the account's service starts by itself: a project naming a key that opens (or an account
// signing in) with no state there is made one at the project's head (the setup, NovaWorldTrack, an
// operation no request starts: it waits for the slot), and so is one a check, a pull or a push
// found the store reset under. A pull that meets files both sides changed stops with its plan kept
// here for the author (novaworld_resolve settles each conflict, novaworld_pull goes on), so no
// operation waits on a person. A pull's files written, the project reads them again as an outside
// change (start_changed_refresh), or opens again when its project file changed; a get's opens its
// folder. A put makes the open project a NovaWorld project's (a new one of the account's, or an
// empty one it may push): the key written to its project file as Project settings' Apply writes it,
// the folder its working copy at revision 0, its files then counted as changes for the first push.
// A release is a call too: the service builds it from the project's revision (ADR 0069 d7). Push
// rights are the service's (its can_push for the project), a release an admin's. Single-threaded,
// like the session.

#include <cstdint>
#include <memory>
#include <string>

#include <editor/novaworld/novaworld_account.h>
#include <editor/novaworld/novaworld_jobs.h>
#include <editor/novaworld/novaworld_transport.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/novaworld_view.h>

namespace opennova::editor {

class SessionCore;

class NovaWorldController {
public:
	explicit NovaWorldController(SessionCore &core);
	NovaWorldController(const NovaWorldController &) = delete;
	NovaWorldController &operator=(const NovaWorldController &) = delete;

	// The host's seams, before the requests they serve: the transport (null: every NovaWorld request
	// refused, the editor reaches no service) and the account's store (null: kept in memory for the
	// session alone). Setting the store reads its account into the view.
	void set_transport(NovaWorldTransport *transport);
	void set_accounts(NovaWorldAccountStore *accounts);

	// The requests' handlers.
	void sign_in(const EditorRequest &request);
	void sign_out();
	void check();
	void get(const EditorRequest &request);
	void put(const EditorRequest &request);
	void pull();
	void resolve(const EditorRequest &request);
	void push(const EditorRequest &request);
	void release(const EditorRequest &request);
	void restore(const EditorRequest &request);
	void history();
	void cancel();

	// The poll's: the calls outside the slot (a sign-in, a project list, the project's ask, a put,
	// a release) stepped, each reply taken in; a put's key written once nothing holds the project;
	// and the setup and the check due started once the slot is free.
	void poll();
	// Whether a call outside the slot is in flight, or work waits for the slot (the Shell's frames
	// keep coming while it is).
	bool busy() const;

	// As a project opens: its working copy on the account's service read into the view, its setup
	// due where it has none; as it closes: the view's standing and a paused pull of it forgotten.
	void project_opened();
	void forget_project();

	// NovaWorldOperation's finish: what the job of the operation `kind` came to, taken in.
	OperationOutcome finish_job(OperationKind kind, std::unique_ptr<NovaWorldJob> job);

private:
	// Whether a request may run now; refused (novaworld.refused, with why) when not: no transport;
	// for `admin`, an admin signed in; for `project`, a project open; for `tracked`, the project on
	// NovaWorld and its folder a working copy of it on the account's service.
	bool ready(bool admin, bool project, bool tracked);
	void refuse(const std::string &why);
	// `job` started as the operation `kind` (its row's holds), the NovaWorld concern touched.
	void start(OperationKind kind, std::unique_ptr<NovaWorldJob> job);
	// The account as the view shows it, and kept by the store.
	void show_account();
	void keep_account(const NovaWorldAccountUpdate &update);
	// The account signed in, out, or to another service: the open project's working copy read again
	// for it, and the project asked again (or its setup due).
	void account_changed();
	// The project list asked of the service (the account's view of it).
	void list_projects();
	// The open project's export folder (ProjectPaths::export_dir), which no scan of it reads.
	std::string export_dir() const;
	// The open project's working copy on the account's service: the state there of the key its
	// project file names; false for none (no project, no key, no service, no state, or the state of
	// another key).
	bool working_copy(NovaWorldWorkingCopy &out) const;
	// The open project's place on NovaWorld read into the view: its key, its working copy, and what
	// a put would make of it.
	void show_copy();
	// What Put on NovaWorld makes of the open project (a new project of the account's): its kind,
	// game, base game's key (found as put() finds it) and expansion code, or why it cannot.
	NovaWorldView::PutPlan put_plan() const;
	// The open project's NovaWorld project asked of the service (a call): its answer in the view,
	// and its setup started where the folder is no working copy of it.
	void ask_project();
	// A job's answer for the project in the view (its head too), or why it gave none.
	void take_remote(const NovaWorldJob &job);
	// The folder made a working copy of its project now, or once the slot is free (`then_check`: a
	// check after it).
	void track(bool then_check);
	// The open project's working copy checked (the slot free); false, nothing started, with none.
	bool start_check();
	// A put's answer: the key written and the folder a working copy at revision 0, once nothing
	// holds the project (put_waiting_), and a check due.
	void finish_put();
	// A pull's paused plan's conflicts into the view.
	void show_conflicts();
	void finish_pull(NovaWorldPull &pull);
	void finish_push(NovaWorldPush &push);
	// A check, a pull or a push that found the store reset (NovaWorldJob::reset): said, and the
	// setup started, a check after it.
	void reset_found(int64_t head);

	SessionCore &core_;
	NovaWorldTransport *transport_ = nullptr;
	NovaWorldAccountStore *accounts_ = nullptr;
	MemoryNovaWorldAccountStore memory_;
	NovaWorldAccount account_;
	NovaWorldCall login_;
	std::string login_server_;
	std::string login_username_;
	NovaWorldCall projects_;
	NovaWorldCall logout_;
	NovaWorldCall release_;
	NovaWorldCall history_;
	std::string release_server_; // the service a release was asked of, for its words
	// The open project's ask (its key), and a put's: the folder it was asked in, the key, whether
	// it makes a new project, and the project once answered (put_waiting_: its key not written
	// yet).
	NovaWorldCall project_;
	std::string project_key_;
	NovaWorldCall put_;
	std::string put_root_;
	std::string put_key_;
	bool put_create_ = false;
	RemoteProject put_project_;
	bool put_waiting_ = false;
	// The setup and a check, each waiting for the slot (and a check once the setup is done).
	bool track_due_ = false;
	bool check_due_ = false;
	bool check_after_track_ = false;
	// A pull stopped on conflicts, for the author to settle; and the folder a get's pull opens once
	// it lands
	// ("" a pull of the open project).
	std::unique_ptr<NovaWorldPull> paused_;
	std::string get_dir_;
	// A get of an expansion's project: its base game's project's key, got next where the expansion's
	// project file names it; and the expansion's folder, opened once its base has landed.
	std::string get_base_key_;
	std::string get_open_after_;
};

} // namespace opennova::editor
