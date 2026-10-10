#include <editor/session/unsaved_guard.h>

#include <algorithm>
#include <utility>

#include <editor/model/diagnostic.h>
#include <editor/model/field_text.h>
#include <editor/project/local_settings.h>
#include <editor/session/document_set.h>
#include <editor/session/file_chores.h>
#include <editor/session/import_controller.h>
#include <editor/session/rename_controller.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

UnsavedGuard::UnsavedGuard(SessionCore &core) : core_(core), view_(core.view()) {}

bool UnsavedGuard::files(const EditorRequest &request, std::vector<std::string> &out) {
	out.clear();
	DocumentSet &documents = core_.documents();
	switch (request_kind_row(request.kind).guard) {
	case GuardScope::None: return false;
	case GuardScope::Document:
		if (const DocumentBase *document = documents.document_for(request.path); document && document->dirty())
			out.push_back(document->path());
		return true;
	case GuardScope::AllDirty: out = documents.dirty_files(); return true;
	case GuardScope::PlannedWrites:
		// What the request's own plan writes over or rewrites, its planner asked.
		if (request.kind == EditorRequestKind::ImportFiles) core_.imports().unsaved_files(request, out);
		else if (request.kind == EditorRequestKind::DeleteAsset || request.kind == EditorRequestKind::DuplicateAsset ||
		         request.kind == EditorRequestKind::DeleteFolder || request.kind == EditorRequestKind::RenameFolder ||
		         request.kind == EditorRequestKind::UndoFile || request.kind == EditorRequestKind::RedoFile ||
		         (request.kind == EditorRequestKind::MoveAsset && !request.paths.empty()))
			core_.chores().unsaved_files(request, out);
		else core_.renames().unsaved_files(request, out);
		return true;
	}
	return false;
}

// A request that would lose, pack or write over unsaved edits waits on the prompt instead of
// running, the prompt listing those files (files()); Build and Play pack the files on disk and
// an import or a rename writes them, so those offer no Discard (their rows' can_discard): their
// Save writes the edits first. One that finds nothing unsaved goes ahead, and a prompt still open
// from an earlier request is dropped: what it waited on was saved another way, and this request
// comes after it. False when the request goes ahead.
bool UnsavedGuard::holds(const EditorRequest &request) {
	std::vector<std::string> unsaved;
	if (!files(request, unsaved)) return false;
	// Play saves first (DI-26), its Save all as the prompt's would write it; what it could not write, the
	// prompt lists.
	if (!unsaved.empty() && saved_first(request, unsaved)) files(request, unsaved);
	if (unsaved.empty()) {
		if (pending_) close_prompt();
		return false;
	}
	// What waits names its document (a Close, a Reload: the one with the edits), the project a
	// switch opens (its dir), or the file it renames.
	const RequestKindRow &row = request_kind_row(request.kind);
	DialogsView::UnsavedPrompt prompt;
	prompt.open = true;
	prompt.action = request.kind;
	prompt.target = row.guard == GuardScope::Document ? unsaved.front()
	                : !request.dir.empty()            ? request.dir
	                                                  : request.path;
	prompt.files = std::move(unsaved);
	prompt.can_discard = row.can_discard;
	pending_ = request;
	if (row.guard == GuardScope::Document) pending_->path = prompt.target;
	view_.dialogs.unsaved_prompt = std::move(prompt);
	core_.outcome().unsaved_prompt = true;
	core_.touch(ViewConcern::Dialogs);
	return true;
}

// Play with save_before_play on (the Play's own, else the project's: on by default; the Play loop, DI-26): the files with unsaved
// edits written, as the prompt's Save would write them, the Play going ahead without asking. Not while an
// operation runs whose gate the Play would meet (a build it would join packs the files: the prompt waits,
// as its answer is weighed against the operation). True when it saved (every file, or some: what was not
// written stays unsaved, its failure reported), and Output says how many.
bool UnsavedGuard::saved_first(const EditorRequest &request, const std::vector<std::string> &unsaved) {
	if (request.kind != EditorRequestKind::Play || !request.save_before_play.value_or(core_.local().save_before_play))
		return false;
	if (gate_answer(request.kind, core_.operations().status()) != GateAnswer::Proceed) return false;
	DocumentSet &documents = core_.documents();
	documents.end_edit_groups();
	const bool all = documents.save_documents(unsaved, false);
	if (all) core_.note("Saved " + counted(unsaved.size(), "file") + " before Play (Build > Save all before Play).");
	return true;
}

// The prompt's answer (UnsavedChoice). Before a Save or a Discard acts, what the request
// would lose or pack is taken again: a file made unsaved since the prompt opened (the
// editor MCP's edit, an undo) renews the prompt with it, and nothing is saved or dropped
// that the prompt did not list. A Save that cannot write every file it lists keeps the
// prompt open over what waits, the failures reported: the modder saves again or cancels.
std::optional<EditorRequest> UnsavedGuard::resolve(UnsavedChoice choice) {
	if (!pending_) {
		core_.refuse_now(CoreFinding::UnsavedNone, "No unsaved-changes prompt is open: nothing waits on an answer.");
		return std::nullopt;
	}
	if (choice == UnsavedChoice::Cancel) {
		close_prompt();
		return std::nullopt;
	}
	if (choice == UnsavedChoice::Discard && !view_.dialogs.unsaved_prompt.can_discard) {
		core_.refuse_now(CoreFinding::UnsavedDiscard, "Build and Play pack the files on disk: save the edited files or cancel.");
		core_.outcome().unsaved_prompt = true;
		return std::nullopt;
	}
	std::vector<std::string> unsaved;
	files(*pending_, unsaved);
	const std::vector<std::string> &listed = view_.dialogs.unsaved_prompt.files;
	for (const std::string &file : unsaved) {
		if (std::find(listed.begin(), listed.end(), file) != listed.end()) continue;
		view_.dialogs.unsaved_prompt.files = unsaved;
		view_.activity.status = file + " has unsaved changes too: the prompt lists it now.";
		core_.outcome().unsaved_prompt = true;
		core_.touch(ViewConcern::Dialogs);
		core_.touch(ViewConcern::Output);
		return std::nullopt;
	}
	if (answer_refused(choice)) return std::nullopt;
	DocumentSet &documents = core_.documents();
	if (choice == UnsavedChoice::Save) {
		documents.end_edit_groups();
		if (!documents.save_documents(view_.dialogs.unsaved_prompt.files, false)) {
			core_.outcome().unsaved_prompt = true;
			return std::nullopt;
		}
	}
	const EditorRequest pending = *pending_;
	close_prompt();
	if (choice == UnsavedChoice::Discard) {
		if (request_kind_row(pending.kind).guard == GuardScope::Document) documents.discard(pending.path);
		else documents.discard_all();
	}
	return pending;
}

// The prompt's answer weighed against the running operation before anything is saved or dropped,
// refused (the prompt kept, said why) as busy_refuses_answer says, the rule the prompt's buttons
// are enabled by: what waits meets the gate first, as it will once answered (refused there,
// nothing is written or dropped: a Discard would lose the edits and still not close), then the
// answer itself (a build packing the files refuses a Save, never a Discard). A project switch or
// Quit, which cancels the operation as it commits, cancels it now, on either answer, and is
// refused when that fails.
bool UnsavedGuard::answer_refused(UnsavedChoice choice) {
	const OperationStatus running = core_.operations().status();
	bool refused = busy_refuses_answer(pending_->kind, choice, running);
	if (!refused && running.running() && gate_answer(pending_->kind, running) == GateAnswer::CancelRunning)
		refused = !core_.cancel_operation(false);
	if (!refused) return false;
	core_.refuse_busy(std::string());
	core_.outcome().unsaved_prompt = true;
	return true;
}

void UnsavedGuard::close_prompt() {
	pending_.reset();
	view_.dialogs.unsaved_prompt = DialogsView::UnsavedPrompt();
	core_.touch(ViewConcern::Dialogs);
}

} // namespace opennova::editor
