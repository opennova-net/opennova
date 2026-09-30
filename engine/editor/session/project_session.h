#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_run.h>
#include <editor/preview/menu_render_check.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_run.h>
#include <editor/run/launch_plan.h>
#include <editor/run/play_lease.h>
#include <editor/run/play_session.h>
#include <editor/run/process_platform.h>
#include <editor/session/editor_request.h>
#include <editor/session/editor_settings.h>
#include <editor/session/session_operation.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

// The one open project and everything the editor does to it (ADR 0046 d10): open and
// create, scan and evaluate, create-missing, build, play. Portable: the shell hands it
// the process seam and the settings path and drains its view; a test drives it the
// same way. Requests come in typed (EditorRequest), the view goes out (SessionView).
// A long job is an operation (session_operation.h, S13 A1): one at a time, stepped within
// each poll's budget so the window that hosts the session keeps drawing while a project
// packs; a request that conflicts with what it reads or writes is refused, joins it,
// supersedes it or cancels it as it commits, as its row says (request_kinds.h), in
// handle(). An edit (a Set, an Add, an
// Undo) leaves the project's validation due rather than running it: a request from
// outside returns validated, and a pump that holds validation (hold_validation) validates
// once, at its poll, however many edits its requests made; Save, Build, Rescan and the
// other project requests validate at once.
class ProjectSession {
public:
	ProjectSession(ProcessPlatform &platform, std::string editor_settings_path);
	~ProjectSession();

	const SessionView &view() const { return view_; }
	const PlayLauncher &launcher() const { return launcher_; }
	void set_launcher(PlayLauncher launcher);

	// True when the request was served here; false for the shell-only kinds. The view is
	// validated when it returns, unless a pump holds validation.
	bool handle(const EditorRequest &request);
	// A pump starts (the shell: the requests its windows raised this frame): the requests
	// handled until the next poll() leave their validation to that poll, so a burst of
	// edits (typing, a drag) validates once.
	void hold_validation() { validation_held_ = true; }
	// What the last request handled from outside came to (reset by the next one).
	const ActionOutcome &outcome() const { return outcome_; }
	// True when the last EditRecord, Copy, Cut, Paste or Duplicate went through (a Move that left
	// a record where it is included: it changed nothing, and nothing was wrong).
	bool last_edit_ok() const { return last_edit_ok_; }
	// Once per frame: the validation the frame's edits left due, the running operation's steps
	// within the poll's budget, the child's state and the game's log tail, then the operation
	// that is done finished (a build lands, and the Play waiting on it starts).
	void poll();
	// How much a poll steps the running operation (kDefaultPollBudget; a test's ms 0 is one
	// step per poll).
	void set_poll_budget(const PollBudget &budget) { poll_budget_ = budget; }
	// The running operation, and those its finish starts, run to their end and finished (a
	// test, a command line).
	void run_operations();
	// `operation` started in the slot as a request starts one: its id, 0 while another runs (a
	// test's, for an operation no request starts yet: one that cannot be cancelled, one that
	// reads or writes what a build does not).
	uint64_t start_operation(std::unique_ptr<SessionOperation> operation);

	Document *document_for(const std::string &path = {});
	bool documents_dirty() const;
	bool project_open() const { return view_.project_open; }
	// What the last validation read: the closed files it loaded and reused.
	const ValidationStats &validation_stats() const { return validation_cache_.stats(); }
	// The directory the running game uses ("" when none): the build never prunes it.
	std::string running_build_dir() const { return play_.running_build_dir(); }

private:
	friend class BuildOperation; // its finish: absorb_build (S13 A2 gives it SessionCore's)

	bool dispatch(const EditorRequest &request);
	// The busy gate (gate_answer): true when the request was refused or joined the running
	// operation; false when it goes on (nothing it conflicts with runs, it superseded the
	// operation, or it cancels the operation itself when it commits).
	bool gate_busy(const EditorRequest &request);
	// True when what reads `reads` and writes `writes` conflicts with the running operation.
	bool busy_for(Holds reads, Holds writes) const;
	// "Wait for the build to finish, or cancel it, first.": `until` ends it.
	std::string busy_message(const std::string &until) const;
	void refuse_busy(const std::string &asset);
	// A Build or a Play onto the running build.
	void join_operation(const EditorRequest &request);
	// The running operation cancelled between two steps (`asked`: CancelOperation's own words);
	// true when none runs now (none ran, or it was cancelled), false when it cannot be cancelled.
	bool cancel_operation(bool asked);
	// The operation the poll found done, finished: the view learns what it came to.
	void finish_operation();
	void show_operation();
	bool handle_document(const EditorRequest &request);
	bool unsaved_files(const EditorRequest &request, std::vector<std::string> &files);
	void rename_unsaved(const std::string &file, const std::string &new_name, std::vector<std::string> &files);
	bool guard_unsaved(const EditorRequest &request);
	void resolve_unsaved(UnsavedChoice choice);
	// The prompt's answer (Save or Discard) refused against the running operation (true, said
	// why, the prompt kept): what waits as the gate would answer it, then the answer itself.
	bool unsaved_answer_refused(UnsavedChoice choice);
	void close_unsaved_prompt();
	bool apply_edits(Document &document, const std::vector<Edit> &edits);
	void copy_records(Document &document, bool cut);
	void paste_records(Document &document, const Edit &target);
	void duplicate_records(Document &document);
	bool save_documents(const std::vector<std::string> &paths, bool rewrite);
	std::vector<std::string> dirty_files() const;
	void end_edit_groups();
	void activate(const std::string &path);
	void update_document_view();
	void validate_documents();
	void validate_later();
	void validate_pending();
	std::shared_ptr<Document> load_document(const std::string &relative, AssetKind kind, Diagnostic &error) const;
	void reload_changed_documents();
	std::vector<Diagnostic> open_document_findings() const;
	void forget_file_state(const std::string &path);
	bool new_project(const std::string &dir, const std::string &title);
	bool open_project(const std::string &dir);
	// The open project closed, its operation cancelled first; false (refused, said why, nothing
	// closed) when that operation cannot be cancelled.
	bool close_project();
	ImportRunResult refresh(bool force_import = false, const std::string &only = std::string());
	void apply_project_settings(const ProjectSettingsChange &change);
	void select_first_screen();
	void create_missing(const std::vector<std::string> &roles);
	void rewrite_file(const std::string &path);
	// Play refused before any build (no spawn on this platform, a game running): true, said why.
	bool play_refused();
	void start_build(bool then_play);
	// A build's finish (BuildOperation): its report into the view, the findings its gate lacked,
	// the game started on it when a Play waits and it is good.
	OperationOutcome absorb_build(const BuildReport &result, const std::vector<Diagnostic> &gate, bool then_play);
	void start_play();
	void stop_play();
	std::string resolve_runtime_executable() const;
	std::string game_install() const;
	void note(std::string line);
	void report(const Diagnostic &d);
	void record_outcome(const Diagnostic &d);
	void refuse_now(const char *code, const std::string &message, const std::string &asset = std::string());
	void tail_game_log();
	void absorb_boot_report(const std::string &line);
	void absorb_play_exit();
	void save_editor_settings();
	void rename_asset(const std::string &file, const std::string &new_name);
	// A name's rename everywhere planned from a PreviewRename's or a RenameSymbol's fields (the
	// definition found by its file, locator and field in the graph, current first).
	SymbolRenamePlan plan_symbol(const EditorRequest &request);
	bool saved_file_uses(const std::string &path, const SymbolRenamePlan &plan) const;
	void preview_rename(const EditorRequest &request);
	void rename_symbol(const EditorRequest &request);
	const RequirementRow *requirement_row(const std::string &role) const;
	const AssetEntry *project_file(const std::string &file) const;
	void assign_requirement(const std::string &role, const std::string &file);
	void reimport(const std::string &source, bool force);
	void preview_import(std::vector<ImportSource> choices, std::vector<ImportSource> roots, bool with_dependencies);
	void plan_preview();
	void set_import_dependencies(bool flag);
	void import_files(const EditorRequest &request);
	void refresh_retail_files();
	// A change of `concern` in the view: its counter and `any` move (session_revisions.h), and
	// the previews follow the active document and the selection. The only way the session moves
	// a counter.
	void touch(ViewConcern concern) {
		view_.update_previews();
		view_.revisions.touch(concern);
	}

	ProcessPlatform &platform_;
	std::string settings_path_;
	EditorSettings settings_;
	ProjectPaths paths_;
	LocalSettings local_;
	PlaySession play_;
	PlayLease play_lease_; // the running game's lease, as written (pid -1: none)
	PlayLauncher launcher_;
	OperationSlot operations_;
	PollBudget poll_budget_ = kDefaultPollBudget;
	std::string game_log_file_;
	uint64_t game_log_offset_ = 0;
	std::string game_log_partial_;
	std::string boot_project_; // the project the game was started in: its boot report is that project's
	std::vector<Diagnostic> play_findings_; // the last Play's own (a nonzero exit), that project's too
	std::vector<std::shared_ptr<Document>> documents_;
	// Each open document's instance and whether it has unsaved edits, as the view last listed
	// them: DocumentSet moves when they differ (update_document_view).
	std::vector<std::pair<uint64_t, bool>> document_set_;
	// The selection each open document had when another became active (activate), by path.
	struct Selection {
		NodeAddress primary;
		std::vector<NodeAddress> selected;
	};
	std::map<std::string, Selection> remembered_;
	// The open documents whose file changed outside the editor and was not read again, by
	// path: a clean one whose file no longer reads (the reason; it stays open as it was), and
	// one with unsaved edits (reload_changed_documents, or a Save refused as a conflict).
	// Each is a Problems row until the file is read again, saved over or matches again, or
	// the document closes.
	std::map<std::string, Diagnostic> stale_;
	std::set<std::string> conflicts_;
	std::optional<EditorRequest> pending_request_; // what the unsaved prompt holds
	std::shared_ptr<AssetGraph> graph_ = std::make_shared<AssetGraph>();
	std::shared_ptr<ProjectAssetSource> assets_ = std::make_shared<ProjectAssetSource>();
	std::shared_ptr<MenuRenderCheck> render_check_ = std::make_shared<MenuRenderCheck>();
	ValidationCache validation_cache_;
	std::vector<Diagnostic> document_findings_; // the last validation's, which the build plan gates on
	                                            // (the render check's notes are not: they never block a build)
	// The last build's own findings (those its report adds to the Problems rows it was gated on:
	// the plan's own, a step that failed), Problems rows until the next build starts or the
	// project closes.
	std::vector<Diagnostic> build_findings_;
	bool validation_due_ = false;               // an edit since the last validation
	bool validation_held_ = false;              // a pump holds validation until its poll
	bool gesture_validation_due_ = false;       // a gesture's edits wait for it to end
	bool last_edit_ok_ = false;
	uint64_t import_serial_ = 0; // the import plans made: each preview's plan takes the next
	SessionView view_;
	ActionOutcome outcome_;
	int handling_ = 0; // handle() depth: the outermost call owns the outcome
};

} // namespace opennova::editor
