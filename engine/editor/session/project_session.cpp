#include <editor/session/project_session.h>

#include <string>
#include <utility>
#include <vector>

#include <editor/session/document_set.h>
#include <editor/session/editor_queries.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/import_controller.h>
#include <editor/session/play_controller.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problems_service.h>
#include <editor/session/rename_controller.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>
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

io::JsonValue ProjectSession::handle_json(const io::JsonValue &json, EditorRequest *shell) {
	io::JsonValue answer = io::JsonValue::make_object();
	std::string error;
	EditorRequest request;
	RequestNames names;
	bool ok = json.is_object();
	if (!ok) error = "A request is a JSON object.";
	// A kind the shell serves with a person to answer it (the pickers) never comes through here:
	// refused by its kind, before its fields are read, naming the fields that carry what it picks.
	const io::JsonValue *token = ok ? json.get("kind") : nullptr;
	EditorRequestKind kind = EditorRequestKind::Rescan;
	if (token && token->is_string() && request_kind_from_token(token->string, kind) &&
			request_kind_row(kind).served_by == ServedBy::ShellNeedsPerson) {
		ok = false;
		error = "The pickers need a person: pass what they pick instead, a project's folder as the "
				"dir of new_project or open_project, the game install or the runtime as "
				"settings.game_install or settings.runtime_executable of apply_project_settings, "
				"files to import as the paths of preview_import.";
	}
	// The record document a request's edits are named in: the one its path names, else the active
	// one, opened first when the request asks it to be and nothing is open there (a fix's edit);
	// an open document of another kind holds no records to name (S13 D6).
	if (ok && token && token->is_string() && request_kind_from_token(token->string, kind) &&
			request_kind_row(kind).params.has(RequestFieldId::Edits)) {
		const std::string path = json.get_string("path", "");
		if (!document_base_for(path) && !path.empty() && json.get_bool("open_first", false) &&
				project_open())
			handle(request::open_document(path));
		names.document = document_for(path);
		if (const DocumentBase *open = names.document ? nullptr : document_base_for(path)) {
			ok = false;
			error = open->path() + " holds no records (document.no_records): its edits name none.";
		}
	}
	ok = ok && editor_request_from_json(json, request, error, &names);
	bool served = false;
	if (ok) {
		served = handle(request);
		if (!served && shell) *shell = request;
	}
	answer.set("ok", io::JsonValue::make_bool(ok));
	answer.set("served", io::JsonValue::make_bool(served));
	if (!error.empty()) answer.set("error", io::json_string(error));
	// What the request came to (`ok` only says it read): the session's outcome, or a plain done
	// for a kind the shell serves; an edit_record's outcome names what each label made.
	if (ok) {
		const ActionOutcome none;
		const ActionOutcome &outcome = served ? impl_->core.outcome() : none;
		io::JsonValue came = action_outcome_to_json(outcome);
		if (request.kind == EditorRequestKind::EditRecord) {
			io::JsonValue made = io::JsonValue::make_object();
			if (outcome.done() && outcome.added.size() == names.made_labels.size())
				for (size_t i = 0; i < outcome.added.size(); ++i)
					if (!names.made_labels[i].empty())
						made.set(names.made_labels[i], io::json_number(double(outcome.added[i])));
			came.set("made", std::move(made));
		}
		answer.set("outcome", std::move(came));
	}
	answer.set("status", io::json_string(view().activity.status));
	answer.set("revision", io::json_number(double(view().revisions.any())));
	return answer;
}

io::JsonValue ProjectSession::query(
		std::string_view name, const io::JsonValue &args, std::string &error) {
	return run_query(impl_->core, name, args, error);
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
	return impl_->core.view().project.open;
}

const ValidationStats &ProjectSession::validation_stats() const {
	return impl_->problems.validation_stats();
}

std::string ProjectSession::running_build_dir() const {
	return impl_->play.running_build_dir();
}

} // namespace opennova::editor
