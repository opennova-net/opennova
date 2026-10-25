#pragma once

// The session's part for the content store (ADR 0069): the account the editor acts as on a
// NovaWorld service, the projects it may read, and the open project as a working copy of one of
// them. It serves the content requests (request_kinds.cpp: content_sign_in, a person's alone, and
// the rest, the editor MCP's too) over the host's transport (ProjectSession::set_content_transport:
// the Shell's HTTP, a test's fake) and the host's account store (set_content_accounts), and shows
// what it knows in the view's ContentView (the Content concern). A sign-in and the project list are
// calls it polls itself (poll: they hold nothing of the session's); a check, a bind, a pull (a
// get's too) and a push run as the session's operations (ContentOperation: the busy gate weighs
// them as their rows say), and their finish comes back here (finish_job). A pull that meets files
// both sides changed stops with its plan kept here for the author (content_resolve settles each
// conflict, content_pull goes on), so no operation waits on a person. A pull's files written, the
// project reads them again as an outside change (start_changed_refresh), or opens again when its
// project file changed; a get's opens its folder. A release is a call too: the service builds it
// from the project's revision (ADR 0069 d7). Single-threaded, like the session.

#include <cstdint>
#include <memory>
#include <string>

#include <editor/content_store/content_account.h>
#include <editor/content_store/content_jobs.h>
#include <editor/content_store/content_transport.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class SessionCore;

class ContentController {
public:
	explicit ContentController(SessionCore &core);
	ContentController(const ContentController &) = delete;
	ContentController &operator=(const ContentController &) = delete;

	// The host's seams, before the requests they serve: the transport (null: every content request
	// refused, the editor reaches no service) and the account's store (null: kept in memory for the
	// session alone). Setting the store reads its account into the view.
	void set_transport(ContentTransport *transport);
	void set_accounts(ContentAccountStore *accounts);

	// The requests' handlers.
	void sign_in(const EditorRequest &request);
	void sign_out();
	void check();
	void get(const EditorRequest &request);
	void bind(const EditorRequest &request);
	void pull();
	void resolve(const EditorRequest &request);
	void push(const EditorRequest &request);
	void release(const EditorRequest &request);
	void restore(const EditorRequest &request);
	void history();
	void cancel();

	// The poll's: the calls outside the slot (a sign-in, a project list, a release) stepped, each
	// reply taken in.
	void poll();
	// Whether a call outside the slot is in flight (the Shell's frames keep coming while it is).
	bool busy() const;

	// As a project opens: its binding read into the view; as it closes: the view's standing and a
	// paused pull of it forgotten.
	void project_opened();
	void forget_project();

	// ContentOperation's finish: what the job of the operation `kind` came to, taken in.
	OperationOutcome finish_job(OperationKind kind, std::unique_ptr<ContentJob> job);

private:
	// Whether a request may run now; refused (content.refused, with why) when not: no transport;
	// for `admin`, an admin signed in; for `project`, a project open; for `bound`, the project a
	// working copy of a content project on the account's service (its binding's server).
	bool ready(bool admin, bool project, bool bound);
	void refuse(const std::string &why);
	// `job` started as the operation `kind` (its row's holds), the content concern touched.
	void start(OperationKind kind, std::unique_ptr<ContentJob> job);
	// The account as the view shows it, and kept by the store.
	void show_account();
	void keep_account(const ContentAccountUpdate &update);
	// The project list asked of the service (the account's view of it).
	void list_projects();
	// The open project's export folder (ProjectPaths::export_dir), which no scan of it reads.
	std::string export_dir() const;
	// The open project's binding read into the view.
	void show_binding();
	// A pull's paused plan's conflicts into the view.
	void show_conflicts();
	void finish_pull(ContentPull &pull);
	void finish_push(ContentPush &push);

	SessionCore &core_;
	ContentTransport *transport_ = nullptr;
	ContentAccountStore *accounts_ = nullptr;
	MemoryContentAccountStore memory_;
	ContentAccount account_;
	ContentCall login_;
	std::string login_server_;
	std::string login_username_;
	ContentCall projects_;
	ContentCall logout_;
	ContentCall release_;
	ContentCall history_;
	std::string release_server_; // the service a release was asked of, for its words
	// A pull stopped on conflicts, for the author to settle; and the folder a get's pull opens once
	// it lands
	// ("" a pull of the open project).
	std::unique_ptr<ContentPull> paused_;
	std::string get_dir_;
	// A get of an expansion's project: its base game's project's key, got next where the expansion's
	// project file names it; and the expansion's folder, opened once its base has landed.
	std::string get_base_key_;
	std::string get_open_after_;
};

} // namespace opennova::editor
