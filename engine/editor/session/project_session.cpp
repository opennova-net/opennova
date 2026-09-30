#include <editor/session/project_session.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <editor/session/document_set.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/import_controller.h>
#include <editor/session/play_controller.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problems_service.h>
#include <editor/session/rename_controller.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_core.h>
#include <editor/session/session_operation.h>
#include <editor/session/session_view.h>
#include <editor/session/unsaved_guard.h>

namespace opennova::editor {

// The session's parts, each holding the core and reaching the others through it (SessionCore::
// Parts). Made in this order and destroyed in the reverse: the core outlives every part.
struct ProjectSession::Impl {
	Impl(ProcessPlatform &platform, PreferencesStore &store) :
			preferences(store),
			core(platform, preferences),
			problems(core),
			documents(core),
			play(core),
			imports(core),
			renames(core),
			guard(core) {
		core.bind({&documents, &problems, &play, &imports, &renames, &guard});
	}

	// The request, served by the part its kind names: the busy gate and the unsaved-changes prompt
	// first, then the part. The prompt's answer runs what waited on it through here again, never
	// through handle(): a request from outside is one entry and one outcome.
	bool dispatch(const EditorRequest &request);
	// The busy gate (gate_answer): true when the request was refused or joined the running
	// operation; false when it goes on (nothing it conflicts with runs, it superseded the
	// operation, or it cancels the operation itself when it commits).
	bool gate_busy(const EditorRequest &request);
	// A Build or a Play onto the running build.
	void join_operation(const EditorRequest &request);

	EditorPreferences preferences;
	SessionCore core;
	ProblemsService problems;
	DocumentSet documents;
	PlayController play;
	ImportController imports;
	RenameController renames;
	UnsavedGuard guard;
	uint64_t handle_entries = 0;
};

ProjectSession::ProjectSession(ProcessPlatform &platform, PreferencesStore &preferences) :
		impl_(std::make_unique<Impl>(platform, preferences)) {
	impl_->core.start();
}

ProjectSession::~ProjectSession() = default;

const SessionView &ProjectSession::view() const {
	return impl_->core.view();
}

void ProjectSession::set_launcher_source(PlayLauncherSource source) {
	impl_->play.set_launcher_source(std::move(source));
}

bool ProjectSession::handle(const EditorRequest &request) {
	++impl_->handle_entries;
	impl_->core.begin_request();
	const bool served = impl_->dispatch(request);
	impl_->core.end_request();
	// A request from outside returns validated, unless a pump holds validation for its poll.
	if (!impl_->problems.held()) impl_->problems.validate_pending();
	return served;
}

void ProjectSession::hold_validation() {
	impl_->problems.hold();
}

const ActionOutcome &ProjectSession::outcome() const {
	return impl_->core.outcome();
}

bool ProjectSession::last_edit_ok() const {
	return impl_->documents.last_edit_ok();
}

uint64_t ProjectSession::handle_entries() const {
	return impl_->handle_entries;
}

bool ProjectSession::Impl::dispatch(const EditorRequest &request) {
	if (request.kind == EditorRequestKind::ResolveUnsaved) {
		if (const std::optional<EditorRequest> waited = guard.resolve(request.unsaved_choice)) dispatch(*waited);
		return true;
	}
	// Build and Play pack the files as saved: every edit group ends first, as EndEdit ends
	// one, so a keystroke after them is a step of its own.
	if (request.kind == EditorRequestKind::Build || request.kind == EditorRequestKind::Play) documents.end_edit_groups();
	if (gate_busy(request) || guard.holds(request)) return true;
	SessionView &view = core.view();
	switch (request.kind) {
	case EditorRequestKind::NewProject: core.new_project(request.path, request.text); return true;
	case EditorRequestKind::OpenProject: core.open_project(request.path); return true;
	case EditorRequestKind::CloseProject: core.close_project(); return true;
	case EditorRequestKind::ForgetRecent: core.forget_recent(request.path); return true;
	case EditorRequestKind::Rescan:
		// Only what changed outside the editor is read again: an open document whose file
		// holds what it was read from keeps its records, its history and its selection.
		if (view.project_open) {
			documents.reload_changed();
			core.refresh();
		}
		return true;
	case EditorRequestKind::ApplyProjectSettings: core.apply_project_settings(request.settings); return true;
	case EditorRequestKind::PreviewImport: imports.preview_files(request); return true;
	case EditorRequestKind::PlanImport: imports.plan(request); return true;
	case EditorRequestKind::SetImportDependencies: imports.set_dependencies(request.flag); return true;
	case EditorRequestKind::CancelImport: imports.cancel(); return true;
	case EditorRequestKind::ImportFiles: imports.import_files(request); return true;
	case EditorRequestKind::PreviewRetailImport: imports.preview_retail(request); return true;
	case EditorRequestKind::Reimport: imports.reimport(request.path, request.flag); return true;
	case EditorRequestKind::CreateMissing:
		if (view.project_open) core.create_missing(request.names);
		return true;
	case EditorRequestKind::Build:
		if (view.project_open) core.start_build(false);
		return true;
	case EditorRequestKind::Play:
		if (view.project_open) core.start_build(true);
		return true;
	case EditorRequestKind::StopPlay: play.stop(); return true;
	case EditorRequestKind::CancelOperation: core.cancel_operation(true); return true;
	case EditorRequestKind::CreateFile: documents.create_file(request); return true;
	case EditorRequestKind::OpenDocument:
	case EditorRequestKind::ReloadDocument: documents.open_document(request); return true;
	case EditorRequestKind::ShowInFiles: documents.show_in_files(request); return true;
	case EditorRequestKind::CloseDocument: documents.close_document(request.path); return true;
	case EditorRequestKind::SelectRecord: documents.select_record(request); return true;
	case EditorRequestKind::EditRecord: documents.edit_record(request); return true;
	case EditorRequestKind::RevertToSaved: documents.revert_to_saved(request); return true;
	case EditorRequestKind::EndEdit: documents.end_edit(request.path); return true;
	case EditorRequestKind::Copy:
	case EditorRequestKind::Cut: documents.copy(request); return true;
	case EditorRequestKind::Paste: documents.paste(request); return true;
	case EditorRequestKind::Duplicate: documents.duplicate(request); return true;
	case EditorRequestKind::Save: documents.save(request.path); return true;
	case EditorRequestKind::SaveAll: documents.save_all(); return true;
	case EditorRequestKind::Undo:
	case EditorRequestKind::Redo: documents.undo_redo(request); return true;
	case EditorRequestKind::RenameAsset: renames.rename_asset(request.path, request.text); return true;
	case EditorRequestKind::AssignRequirement: renames.assign_requirement(request.text, request.path); return true;
	case EditorRequestKind::PreviewRename: renames.preview(request); return true;
	case EditorRequestKind::RenameSymbol: renames.rename_symbol(request); return true;
	case EditorRequestKind::ClearOutput:
		view.output.clear();
		core.touch(ViewConcern::Output);
		return true;
	case EditorRequestKind::PickDirectory:
	case EditorRequestKind::PickFile:
	case EditorRequestKind::RevealPath: return false;
	case EditorRequestKind::Quit:
		// The running operation goes first (a build's staging directory with it); one that cannot
		// be cancelled keeps the editor open.
		if (!core.cancel_operation(false)) {
			core.refuse_busy(std::string());
			return true;
		}
		view.quit_requested = true;
		core.touch(ViewConcern::Project);
		return true;
	default: return false;
	}
	return false;
}

// --- the busy gate -----------------------------------------------------------------------------

// The busy gate (request_kinds.h): the request's row and the running operation's weighed by
// gate_answer, the one answer the windows disable by (busy_refuses). Refused: an operation.busy
// warning, the outcome not done, nothing changed. Joined: the running operation serves it (a
// Build or a Play onto a build). Superseded: the running operation is cancelled for it (a new
// import plan over a running one). CancelRunning: the request goes on, and its own flow cancels
// the operation when it commits (close_project, Quit), once its own checks pass: a NewProject
// that fails keeps the build. A request that conflicts with nothing it reads or writes (an edit,
// an open, an import's preview while a build packs) goes on. One the unsaved-changes prompt
// would hold, and which the gate does not refuse, asks first and meets the gate once it goes
// ahead: a Close the prompt then drops cancels nothing, and a Build with unsaved edits never
// joins a build that packs the files without them.
bool ProjectSession::Impl::gate_busy(const EditorRequest &request) {
	const GateAnswer answer = gate_answer(request.kind, core.operations().status());
	if (answer == GateAnswer::Proceed) return false;
	if (answer != GateAnswer::Refuse) {
		std::vector<std::string> unsaved;
		if (guard.files(request, unsaved) && !unsaved.empty()) return false;
	}
	switch (answer) {
	case GateAnswer::CancelRunning: return false;
	case GateAnswer::Join: join_operation(request); return true;
	case GateAnswer::Supersede:
		if (core.cancel_operation(false)) return false;
		break;
	default: break;
	}
	core.refuse_busy(request.path);
	return true;
}

// A Build or a Play onto the running build: the build serves it (a Play refused before it could,
// as it would be before any build: no spawn here, or a game running). The outcome names the
// operation joined.
void ProjectSession::Impl::join_operation(const EditorRequest &request) {
	if (request.kind == EditorRequestKind::Play && play.refused()) return;
	core.operations().running()->join(request);
	core.outcome().operation = core.operations().status().id;
	if (request.kind == EditorRequestKind::Play) {
		core.view().status = "Building, then playing...";
		core.note("Play starts the game when the build lands.");
	}
}

// --- the poll and the operation slot -------------------------------------------------------------

void ProjectSession::poll() {
	Impl &session = *impl_;
	session.problems.release();
	session.problems.validate_pending();
	session.core.step_operation();
	session.play.poll();
	// Last, the operation found done finishes: the view learns what it came to (a build lands,
	// and the game a Play waits on starts on it).
	if (session.core.operations().done()) session.core.finish_operation();
}

void ProjectSession::set_poll_budget(const PollBudget &budget) {
	impl_->core.set_poll_budget(budget);
}

void ProjectSession::run_operations() {
	while (impl_->core.operations().running()) {
		impl_->core.operations().run_to_end();
		poll();
	}
}

uint64_t ProjectSession::start_operation(std::unique_ptr<SessionOperation> operation) {
	return impl_->core.start_operation(std::move(operation));
}

// --- what is asked without a request -------------------------------------------------------------

std::string ProjectSession::problems_json(const std::string &query) {
	return impl_->problems.problems_json(query);
}

Document *ProjectSession::document_for(const std::string &path) {
	return impl_->documents.document_for(path);
}

bool ProjectSession::documents_dirty() const {
	return impl_->documents.documents_dirty();
}

bool ProjectSession::project_open() const {
	return impl_->core.view().project_open;
}

const ValidationStats &ProjectSession::validation_stats() const {
	return impl_->problems.validation_stats();
}

std::string ProjectSession::running_build_dir() const {
	return impl_->play.running_build_dir();
}

} // namespace opennova::editor
