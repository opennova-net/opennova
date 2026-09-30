#include <editor/session/project_session.h>

#include <string>
#include <utility>

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
	bool served = false, outermost = false;
	{
		const SessionCore::RequestScope scope(impl_->core);
		outermost = scope.outermost();
		served = serve_request(impl_->core, request);
	}
	// A request from outside returns validated, unless a pump holds validation for its poll.
	if (outermost && !impl_->problems.held()) impl_->problems.validate_pending();
	return served;
}

void ProjectSession::hold_validation() {
	impl_->problems.hold();
}

const ActionOutcome &ProjectSession::outcome() const {
	return impl_->core.outcome();
}

bool ProjectSession::last_edit_ok() const {
	// A request the gate refused, or one whose document was not open, never reached its edit: the
	// flag its edit would have set is the last edit's, so the outcome answers first.
	return impl_->core.outcome().done() && impl_->documents.last_edit_ok();
}

uint64_t ProjectSession::handle_entries() const {
	return impl_->handle_entries;
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
	return impl_->documents.records_for(path);
}

DocumentBase *ProjectSession::document_base_for(const std::string &path) {
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
