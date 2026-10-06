#include <editor/session/project_session.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/model/text_document.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_files.h>
#include <editor/session/disk_watch.h>
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
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

// How many bytes of texture files a poll reads to make thumbnails (one thumbnail at least).
constexpr size_t kThumbnailPollBytes = size_t(4) << 20;

} // namespace

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
			guard(core),
			disk(core) {
		core.bind({&documents, &problems, &play, &imports, &renames, &guard, &disk});
	}

	EditorPreferences preferences;
	SessionCore core;
	ProblemsService problems;
	DocumentSet documents;
	PlayController play;
	ImportController imports;
	RenameController renames;
	UnsavedGuard guard;
	DiskWatch disk;
	uint64_t handle_entries = 0;
};

ProjectSession::ProjectSession(ProcessPlatform &platform, PreferencesStore &preferences) :
		impl_(std::make_unique<Impl>(platform, preferences)) {
	impl_->core.start();
}

ProjectSession::~ProjectSession() {
	// The recently placed items a last placement left unkept (no poll since).
	impl_->core.save_recent_items();
}

const SessionView &ProjectSession::view() const {
	return impl_->core.view();
}

void ProjectSession::set_launcher_source(PlayLauncherSource source) {
	impl_->play.set_launcher_source(std::move(source));
}

bool ProjectSession::handle(const EditorRequest &request) {
	++impl_->handle_entries;
	const SessionCore::RequestScope scope(impl_->core, request_kind_row(request.kind).background);
	// A gesture of the wire's open in the document the request is on ends first, unless the request is
	// its next sample (S13 V7).
	impl_->core.request_arrives(request);
	return serve_request(impl_->core, request);
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
	// Whether a request's edits are a text document's spans (S13 D9): the document open at the path
	// (the active one for none), else the file the project's scan lists there, by the kind the scan
	// read (a music script's .bin by its content, which its name alone does not say).
	const auto text_at = [this](const std::string &path) {
		if (const DocumentBase *document = document_base_for(path)) return text_of(*document) != nullptr;
		const std::shared_ptr<const AssetScan> &scan = view().project.scan;
		if (path.empty() || !scan) return false;
		const AssetEntry *entry = scan->at_path(path);
		if (!entry) entry = scan->find(basename_of(path));
		const DocumentType *type = entry ? document_type_for(entry->kind) : nullptr;
		return type && document_content(*type) == DocumentContent::Text;
	};
	// The record document a request's edits are named in: the one its path names, else the active
	// one; over a text document they are its spans (S13 D9); an open document of another kind holds
	// nothing to name (S13 D6). A kind that takes open_first, asking it with nothing open at its path
	// (a fix's edit), is read once before the document opens, so one refused as it is read asks
	// nothing of the session; what the edits are is known again once it is open.
	if (ok && token && token->is_string() && request_kind_from_token(token->string, kind) &&
			request_kind_row(kind).params.has(RequestFieldId::Edits)) {
		const std::string path = json.get_string("path", "");
		const DocumentBase *open = document_base_for(path);
		if (open && !records_of(*open) && !text_of(*open)) {
			ok = false;
			error = open->path() + " holds no records (document.no_records): its edits name none.";
		} else if (!open && !path.empty() && project_open() &&
				request_kind_row(kind).params.has(RequestFieldId::OpenFirst) &&
				json.get_bool("open_first", false)) {
			RequestNames first;
			first.unresolved = true;
			first.text = text_at(path);
			EditorRequest unread;
			ok = editor_request_from_json(json, unread, error, &first);
			if (ok)
				handle(request::open_document(path));
		}
		names.document = document_for(path);
		names.text = text_at(path);
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
			if (outcome.done())
				for (size_t i = 0; i < names.labels.size() && i < outcome.made.size(); ++i)
					if (!names.labels[i].empty() && outcome.made[i])
						made.set(names.labels[i], io::json_number(double(outcome.made[i])));
			came.set("made", std::move(made));
		}
		// A drag in a viewport names its gesture, which the gesture's next drag passes back (S13 V7).
		if (request.kind == EditorRequestKind::EditInViewport && request.drag != ViewportDrag())
			came.set("gesture", io::json_number(double(outcome.gesture)));
		answer.set("outcome", std::move(came));
	}
	answer.set("status", io::json_string(view().activity.status));
	answer.set("view_revision", io::json_number(double(view().revisions.any())));
	return answer;
}

io::JsonValue ProjectSession::query(
		std::string_view name, const io::JsonValue &args, std::string &error) {
	return run_query(impl_->core, name, args, error);
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

// The poll's order (S13 A3), one budget a poll: a gesture of the wire's with no sample for
// kWireGestureLapseMs ended (S13 V7: a client that went away holds no validation); the validation
// left due, a step at a time within the budget; the running operation's steps within what is left of
// it, at least one; the child's state and the game's log tail; last, the operation found done
// finishes: the view learns what it came to (a project opens, a build lands and the game a Play waits
// on starts on it), and what it read leaves the validation due, which the next poll steps.
void ProjectSession::poll() {
	Impl &session = *impl_;
	session.core.lapse_wire_gestures();
	const PollBudget budget = session.core.poll_budget();
	const int64_t started = budget.ms > 0 ? steady_clock_ms() : 0;
	session.problems.step_validation(budget, steady_clock_ms);
	PollBudget rest = budget;
	if (budget.ms > 0) rest.ms = std::max<int64_t>(0, budget.ms - (steady_clock_ms() - started));
	session.core.step_operation(rest);
	session.play.poll();
	if (session.core.operations().done()) session.core.finish_operation();
	// The sweep a focus-in began over every file (ADR 0046 DI-01), while no operation runs.
	session.disk.step(rest);
	session.core.save_recent_items();
	// The texture thumbnails the windows asked for and the cache lacks (ADR 0046 S18), at least one a
	// poll, then within what is left of the poll's milliseconds and until kThumbnailPollBytes of files are
	// read: a list of hundreds of textures fills a few at a frame.
	const SessionView &view = session.core.view();
	if (view.project.open && view.documents.thumbnails) {
		const std::function<int64_t()> clock = budget.ms > 0 ? std::function<int64_t()>(steady_clock_ms) : std::function<int64_t()>();
		view.documents.thumbnails->step(view, kThumbnailPollBytes, clock, started + budget.ms);
	}
}

void ProjectSession::set_poll_budget(const PollBudget &budget) {
	impl_->core.set_poll_budget(budget);
}

void ProjectSession::set_workspace_kept(bool kept) {
	impl_->core.set_workspace_kept(kept);
}

void ProjectSession::run_operations() {
	while (impl_->core.operations().running()) {
		impl_->core.operations().run_to_end();
		poll();
	}
	impl_->problems.validate_pending();
	impl_->problems.settle_originals();
}

uint64_t ProjectSession::start_operation(std::unique_ptr<SessionOperation> operation) {
	return impl_->core.start_operation(std::move(operation));
}

Viewports &ProjectSession::viewports() {
	return impl_->core.viewports();
}

void ProjectSession::advance(double seconds) {
	impl_->core.viewports().advance(seconds);
}

void ProjectSession::report_sound(uint64_t serial, WorkspaceView::SoundState state, const std::string &error) {
	if (::opennova::editor::report_sound(impl_->core.view().workspace, serial, state, error))
		impl_->core.touch(ViewConcern::Workspace);
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

size_t ProjectSession::problems_compositions() const {
	return impl_->problems.compositions();
}

const OriginalFiles &ProjectSession::originals() const { return impl_->problems.originals(); }

size_t ProjectSession::files_scanned() const {
	return impl_->core.files_scanned();
}

std::string ProjectSession::running_build_dir() const {
	return impl_->play.running_build_dir();
}

} // namespace opennova::editor
