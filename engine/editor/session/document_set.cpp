#include <editor/session/document_set.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <editor/session/problems_service.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>

namespace fs = std::filesystem;

namespace opennova::editor {

DocumentSet::DocumentSet(SessionCore &core) : core_(core), view_(core.view()), paths_(core.paths()) {}

// --- what is open ----------------------------------------------------------------------------

DocumentBase *DocumentSet::document_for(const std::string &path) {
	const std::string &wanted = path.empty() ? view_.documents.active : path;
	for (auto &document : documents_)
		if (document->path() == wanted || normalized_logical_name(fs::path(document->path()).filename().string()) == normalized_logical_name(wanted))
			return document.get();
	return nullptr;
}

Document *DocumentSet::records_for(const std::string &path) {
	DocumentBase *document = document_for(path);
	return document ? records_of(*document) : nullptr;
}

void DocumentSet::refuse_records(const std::string &path, const char *not_open) {
	if (!view_.project.open) return;
	if (const DocumentBase *document = document_for(path))
		return core_.refuse_now(CoreFinding::DocumentNoRecords, "This document holds no records.",
		                        document->path());
	core_.refuse_now(CoreFinding::DocumentNotOpen, not_open, path);
}

bool DocumentSet::documents_dirty() const {
	for (const auto &document : documents_) if (document->dirty()) return true;
	return false;
}

std::vector<std::string> DocumentSet::dirty_files() const {
	std::vector<std::string> files;
	for (const auto &document : documents_)
		if (document->dirty()) files.push_back(document->path());
	return files;
}

void DocumentSet::update_view() {
	view_.documents.open.clear();
	for (const auto &document : documents_) view_.documents.open.push_back(document);
	core_.problems().set_open(view_.documents.open);
	core_.touch(ViewConcern::Documents);
	// Which documents are open, each as read and whether it has unsaved edits: an edit that
	// leaves its document as unsaved as it was moves Documents alone.
	std::vector<std::pair<uint64_t, bool>> set;
	for (const auto &document : documents_)
		set.emplace_back(document->identity(), document->dirty());
	if (set != document_set_) {
		document_set_ = std::move(set);
		core_.touch(ViewConcern::DocumentSet);
	}
}

const DocumentBase *DocumentSet::open_at(const std::string &path) const {
	for (const auto &document : documents_)
		if (document->path() == path) return document.get();
	return nullptr;
}

// The one place the active document changes. The document it replaces keeps its selection
// for when it is active again; `path` takes back the one it kept (none when it never had
// one, or was read again since: a menu then shows its first screen), repaired against its
// records as they are now. A caller with a record to show selects it after.
void DocumentSet::activate(const std::string &path) {
	if (path == view_.documents.active) return;
	if (open_at(view_.documents.active)) remembered_[view_.documents.active] = {view_.documents.selection, view_.documents.selected};
	view_.documents.active = path;
	view_.documents.select_only({});
	const auto kept = remembered_.find(path);
	if (kept != remembered_.end()) {
		view_.documents.selection = kept->second.primary;
		view_.documents.selected = kept->second.selected;
		remembered_.erase(kept);
		if (const DocumentBase *document = open_at(path))
			if (const Document *records = records_of(*document))
				view_.documents.repair_selection(*records, NodeAddress());
	}
	select_first_screen();
	core_.touch(ViewConcern::ActiveDocument); // the caller touches Selection
}

// A menu made the active document, or read again, with nothing selected shows its first
// screen: the menu view lists the selected screen's windows and the preview draws it.
void DocumentSet::select_first_screen() {
	if (view_.documents.selection.row) return;
	const Document *document = records_for();
	if (!document || document->kind() != AssetKind::Menu || document->rows().empty()) return;
	const Node &screen = *document->rows().front();
	view_.documents.select_only({screen.id, screen.kind, 0});
}

// --- the files -------------------------------------------------------------------------------

std::shared_ptr<DocumentBase> DocumentSet::load(const std::string &relative, AssetKind kind,
                                                Diagnostic &error) const {
	const DocumentType *type = document_type_for(kind);
	if (!type) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This kind of file has no editor yet.", relative);
		return nullptr;
	}
	std::shared_ptr<DocumentBase> document = type->make();
	if (!document->load((fs::path(paths_.root) / relative).generic_string(), relative, kind, view_.project.document->target_game,
	                    error))
		return nullptr;
	return document;
}

// The open documents against their files (Rescan, an import, Build and Play: the build
// packs the files on disk, and its gate is the validation in which an open document stands
// in for its file). A document whose file holds what it was read from or last saved keeps
// its records, its history and its selection. A clean one whose file changed outside the
// editor is read again (its selection goes with its old records); one whose file no longer
// reads (or is gone) stays open as it was, and why is its Problems row (document.stale, an
// error: the build would pack the file that does not read). One with unsaved edits whose
// file changed keeps them, and a warning says so (document.conflict: its Save is refused)
// with a Reload fix, which asks about the edits first.
void DocumentSet::reload_changed() {
	bool changed = false;
	for (auto &document : documents_) {
		const std::string path = document->path();
		if (document->matches_file()) {
			forget_file_state(path);
			continue;
		}
		if (document->dirty()) {
			stale_.erase(path);
			conflicts_.insert(path);
			continue;
		}
		Diagnostic error;
		std::shared_ptr<DocumentBase> loaded = load(path, document->kind(), error);
		if (!loaded) {
			if (!stale_.count(path))
				core_.note("Kept " + path + " as it was: it changed outside the editor and could not be read again.");
			stale_[path] = error;
			continue;
		}
		forget_file_state(path);
		remembered_.erase(path); // read again: its records' identities are gone
		if (path == view_.documents.active) view_.documents.select_only({});
		document = loaded;
		changed = true;
		core_.note("Reloaded " + path + ": it changed outside the editor.");
	}
	if (!changed) return;
	select_first_screen(); // the active menu read again shows its first screen
	core_.touch(ViewConcern::Selection);
	update_view();
}

// The Problems rows of the open documents whose file changed outside the editor and was not
// read again (stale_, conflicts_).
std::vector<Diagnostic> DocumentSet::findings() const {
	std::vector<Diagnostic> findings;
	for (const auto &[path, reason] : stale_) {
		Diagnostic d = make_finding(CoreFinding::DocumentStale, DiagnosticSeverity::Error,
		                            "This file changed outside the editor and could not be read again, so the editor "
		                            "shows it as it was: " + reason.message + " Correct the file and Refresh, or close it.",
		                            path);
		d.line = reason.line;
		findings.push_back(std::move(d));
	}
	for (const std::string &path : conflicts_)
		findings.push_back(make_finding(CoreFinding::DocumentConflict, DiagnosticSeverity::Warning,
		                                "This file changed outside the editor while it has unsaved edits: Save is "
		                                "refused until it is read again, which discards the edits.",
		                                path));
	return findings;
}

void DocumentSet::forget_file_state(const std::string &path) {
	stale_.erase(path);
	conflicts_.erase(path);
}

// --- the requests ------------------------------------------------------------------------------

void DocumentSet::create_file(const EditorRequest &request) {
	if (!view_.project.open) return;
	// A name alone cannot say what a new `.bin` is; the request's file_kind may name the kind.
	const AssetKind kind = request.file_kind.empty() ? classify_asset(request.path, nullptr)
	                                                 : asset_kind_from_token(request.file_kind);
	// A required name gets its requirement's blank (main.mnu, the STARTUP screen); any
	// other name the kind's free-form one (blank_factory.h). A kind with neither cannot
	// be made; one the editor does not edit (a font) is made and not opened.
	BlankRequest blank;
	blank.logical_name = request.path;
	blank.project_title = view_.project.document->title;
	for (const RequirementRow &row : view_.project.requirements->rows)
		if (row.expected_kind == kind && normalized_logical_name(row.name) == normalized_logical_name(request.path))
			blank.role = row.role;
	if (!find_blank_factory_for_role(blank.role) && !find_blank_factory_for_kind(kind)) {
		core_.report(make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "The editor cannot create this kind of file.", request.path));
		return;
	}
	// A plain name the archives can carry, whose extension is the kind's, landing
	// inside the project.
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	const char *const folder = asset_kind_row(kind).folder;
	if (!check_project_file_name(paths_.root, folder, request.path, kind, problem, message)) {
		const CoreFinding code = problem == FileNameProblem::Kind   ? CoreFinding::DocumentKind
		                         : problem == FileNameProblem::Path ? CoreFinding::DocumentPath
		                                                            : CoreFinding::DocumentName;
		core_.report(make_finding(code, DiagnosticSeverity::Error, message, request.path));
		return;
	}
	const auto *existing = view_.project.scan->find(request.path);
	const std::string relative = (fs::path(folder) / request.path).generic_string();
	if (!existing) {
		const auto target = fs::path(paths_.root) / relative;
		std::error_code ec;
		if (fs::exists(target, ec) || ec) {
			core_.report(make_finding(CoreFinding::DocumentConflict, DiagnosticSeverity::Error, "Refresh before creating this file.", request.path));
			return;
		}
		std::vector<uint8_t> bytes; Diagnostic error;
		if (!make_blank(blank, kind, bytes, error)) { core_.report(error); return; }
		if (!ensure_directory(target.parent_path().generic_string(), message) ||
			!write_file_atomic(target.generic_string(), bytes.data(), bytes.size(), message)) {
			core_.report(make_finding(CoreFinding::DocumentWrite, DiagnosticSeverity::Error, message, request.path));
			return;
		}
		core_.update_files({relative}); // the file made, read into the scan alone
		core_.note("Created " + relative);
	}
	if (is_editable_kind(kind)) {
		open_document(request::open_document(request.path));
	} else {
		view_.activity.status = existing ? existing->relative_path + " is in the project already." : "Created " + relative + ".";
		core_.touch(ViewConcern::Output);
	}
}

void DocumentSet::open_document(const EditorRequest &request) {
	if (!view_.project.open) return;
	const std::string path = request.path.empty() ? view_.documents.active : request.path;
	// The record a request names (by its address, or by its locator: a Go to) is selected,
	// and its field (a Problems row's, the defining field a Go to shows) shown: a RevealRecord
	// event for the document's view and the Inspector, one per ask (the same row clicked again
	// shows it again).
	const auto select_named = [this, &request](const DocumentBase &document) {
		const Document *records = records_of(document);
		if (!records) return; // a document of another kind holds no records to select
		const NodeAddress record =
		        request.locator.empty() ? request.address : records->address_at(request.locator);
		view_.documents.select_only(record);
		if (!record.row || request.field.empty()) return;
		ViewEvent reveal;
		reveal.kind = ViewEventKind::RevealRecord;
		reveal.path = document.path();
		reveal.address = record;
		reveal.field = request.field;
		view_.events.post(std::move(reveal));
	};
	if (request.kind == EditorRequestKind::OpenDocument && document_for(path)) {
		// An open document comes back with the selection it had, unless the request names
		// a record (a Problems row, a Go to).
		const DocumentBase &document = *document_for(path);
		activate(document.path());
		if (request.address.row || !request.locator.empty()) select_named(document);
		core_.touch(ViewConcern::Selection);
		return;
	}
	for (const auto &asset : view_.project.scan->entries) {
		if (asset.relative_path != path && normalized_logical_name(asset.logical_name) != normalized_logical_name(path)) continue;
		const DocumentType *type = document_type_for(asset.kind);
		if (!type) {
			core_.report(make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This kind of file has no editor yet.", path));
			return;
		}
		std::shared_ptr<DocumentBase> document = type->make(); Diagnostic error;
		if (!document->load((fs::path(paths_.root) / asset.relative_path).generic_string(),
					asset.relative_path, asset.kind, view_.project.document->target_game, error)) {
			core_.report(error);
			return;
		}
		for (auto it = documents_.begin(); it != documents_.end(); ++it)
			if ((*it)->path() == asset.relative_path) { documents_.erase(it); break; }
		remembered_.erase(asset.relative_path); // read again: its records have new identities
		forget_file_state(asset.relative_path);
		documents_.push_back(document);
		activate(document->path());
		select_named(*document);
		select_first_screen(); // no record named: a menu shows its first screen
		core_.touch(ViewConcern::Selection);
		update_view(); core_.problems().validate_later(); return;
	}
	core_.report(make_finding(CoreFinding::DocumentMissing, DiagnosticSeverity::Error, "The file was not found.", path));
}

// Files shows the file (and asks its new name when the request says so): a RevealFile event,
// one per ask, so the same file asked again is shown again.
void DocumentSet::show_in_files(const EditorRequest &request) {
	if (!view_.project.open) return;
	const AssetEntry *asset = core_.project_file(request.path);
	if (!asset) {
		core_.report(make_finding(CoreFinding::DocumentMissing, DiagnosticSeverity::Error, "The file was not found.", request.path));
		return;
	}
	ViewEvent reveal;
	reveal.kind = ViewEventKind::RevealFile;
	reveal.path = asset->relative_path;
	reveal.flag = request.ask_name;
	view_.events.post(std::move(reveal));
	core_.touch(ViewConcern::Selection);
}

void DocumentSet::close_document(const std::string &requested) {
	const std::string path = requested.empty() ? view_.documents.active : requested;
	for (auto it = documents_.begin(); it != documents_.end(); ++it)
		if ((*it)->path() == path) { documents_.erase(it); break; }
	remembered_.erase(path);
	forget_file_state(path);
	if (view_.documents.active == path) {
		activate(documents_.empty() ? "" : documents_.back()->path());
		core_.touch(ViewConcern::Selection);
	}
	update_view(); core_.problems().validate_later();
}

// The document's own path, however the request named it (a logical name included), so a
// selection joined by path stays in one document; a record of another document makes it
// the active one, the record alone selected.
void DocumentSet::select_record(const EditorRequest &request) {
	const DocumentBase *document = document_for(request.path);
	const std::string path = document ? document->path() : request.path.empty() ? view_.documents.active : request.path;
	if (path != view_.documents.active) {
		activate(path);
		view_.documents.select_only(request.address);
	} else {
		view_.documents.select(path, request.address, request.mode);
	}
	core_.touch(ViewConcern::Selection);
}

void DocumentSet::edit_record(const EditorRequest &request) {
	// A fix's edit opens its document first (a Problems row about a file not open).
	if (!document_for(request.path) && request.open_first && view_.project.open &&
	    !request.path.empty())
		open_document(request::open_document(request.path));
	auto *document = document_for(request.path);
	if (!document) {
		if (view_.project.open) core_.refuse_now(CoreFinding::DocumentNotOpen, "Open the file before editing it.", request.path);
		return;
	}
	// A batch that asks nothing (a replace_list of an empty list by none, S13 A5) is done.
	if (request.edits.empty()) return;
	apply_edits(*document, request.edits);
}

void DocumentSet::revert_to_saved(const EditorRequest &request) {
	auto *document = records_for(request.path);
	if (!document) {
		refuse_records(request.path, "Open the file before reverting in it.");
		return;
	}
	std::vector<Edit> batch;
	for (const Edit &target : request.edits)
		for (Edit &edit : document->revert_edits(target.address, target.field)) batch.push_back(std::move(edit));
	if (batch.empty()) {
		last_edit_ok_ = false;
		core_.refuse_now(CoreFinding::DocumentRevertNothing,
		                 "Nothing to revert: the field is as the saved file holds it, or the saved file does not have "
		                 "it to go back to.",
		                 document->path());
		return;
	}
	apply_edits(*document, batch);
}

void DocumentSet::copy(const EditorRequest &request) {
	auto *document = records_for(request.path);
	if (!document) {
		refuse_records(request.path, "Open the file before copying from it.");
		return;
	}
	copy_records(*document, request.kind == EditorRequestKind::Cut);
}

void DocumentSet::paste(const EditorRequest &request) {
	auto *document = records_for(request.path);
	if (!document) {
		refuse_records(request.path, "Open the file before pasting into it.");
		return;
	}
	paste_records(*document, request.paste_at);
}

void DocumentSet::duplicate(const EditorRequest &request) {
	auto *document = records_for(request.path);
	if (!document) {
		refuse_records(request.path, "Open the file before duplicating in it.");
		return;
	}
	duplicate_records(*document);
}

void DocumentSet::undo_redo(const EditorRequest &request) {
	auto *document = document_for(request.path);
	if (!document) {
		if (view_.project.open) core_.refuse_now(CoreFinding::DocumentNotOpen, "Open the file before undoing or redoing in it.", request.path);
		return;
	}
	const uint64_t before = document->revision();
	const NodeAddress primary = view_.documents.selection;
	const std::vector<NodeAddress> selected = view_.documents.selected;
	if (request.kind == EditorRequestKind::Undo) document->undo(); else document->redo();
	if (const Document *records = records_of(*document))
		view_.documents.repair_selection(*records, NodeAddress());
	if (view_.documents.selection != primary || view_.documents.selected != selected) core_.touch(ViewConcern::Selection);
	update_view();
	// An undo or a redo ends a gesture: the validation its edits left waiting runs now.
	if (document->revision() != before || gesture_validation_due_) core_.problems().validate_later();
	gesture_validation_due_ = false;
}

void DocumentSet::end_edit(const std::string &path) {
	if (auto *document = document_for(path)) document->end_edit_group();
	if (gesture_validation_due_) core_.problems().validate_later();
	gesture_validation_due_ = false;
}

void DocumentSet::save(const std::string &path) {
	if (DocumentBase *document = document_for(path)) {
		save_documents({document->path()}, true);
	} else if (view_.project.open) {
		// A file that is not open (a Rewrite fix names one) is rewritten closed.
		if (path.empty()) core_.refuse_now(CoreFinding::DocumentNotOpen, "Open a file before saving it.");
		else rewrite_file(path);
	}
}

void DocumentSet::save_all() {
	save_documents(dirty_files(), false);
}

// Writes each open document at `paths` that has unsaved edits (`rewrite`: an explicit Save,
// which also writes one with none whose file holds other bytes than it would write, and
// refuses one that does not serialize), past a failure: an Output line per file written, the
// files written read into the scan alone (SessionCore::update_files, S13 A3: no import pass, no
// other file read), then one finding per file that could not be, the status counting both. False
// when one could not be. (A save while a build packs never reaches here: the busy gate refused
// it.)
bool DocumentSet::save_documents(const std::vector<std::string> &paths, bool rewrite) {
	std::vector<DocumentBase *> writes;
	for (const std::string &path : paths)
		for (const auto &document : documents_)
			if (document->path() == path &&
			    (document->dirty() || (rewrite && document->rewrite_need() != DocumentBase::RewriteNeed::None)))
				writes.push_back(document.get());
	if (writes.empty()) {
		view_.activity.status = paths.size() == 1 ? paths.front() + " has no changes to save." : "No file has unsaved changes.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	size_t saved = 0;
	std::vector<Diagnostic> failures;
	std::vector<std::string> written;
	for (DocumentBase *document : writes) {
		Diagnostic error;
		if (!document->save(error)) {
			// A file changed outside the editor under unsaved edits: the conflict is a row
			// until the document is read again (its Reload fix).
			if (error.row() == &finding_code(CoreFinding::DocumentConflict) && document->dirty())
				conflicts_.insert(document->path());
			failures.push_back(error);
			continue;
		}
		forget_file_state(document->path());
		++saved;
		written.push_back(document->path());
		core_.note("Saved " + document->path());
	}
	gesture_validation_due_ = false;
	update_view();
	core_.update_files(written);
	// Reported after the scan's update, which leaves the validation that rebuilds the rows due.
	for (const Diagnostic &d : failures) core_.report(d);
	view_.activity.status = "Saved " + std::to_string(saved) + " file(s)" +
	               (failures.empty() ? "." : "; " + std::to_string(failures.size()) + " could not be saved: see Problems.");
	core_.touch(ViewConcern::Output);
	return failures.empty();
}

// A Save of a file that is not open: read as its document type, written when it would
// write other bytes than the file holds (the canonical rewrite: the lines the game ignores
// dropped, the line ends fixed, a table regrouped), and left closed; the scan then reads the
// file as written (alone), so the findings the rewrite fixed leave Problems. One that does not
// serialize is refused with the reason (document.unserializable), the file untouched. Of two
// files of one name, the one project_file picks: the path named, else the first of the name.
void DocumentSet::rewrite_file(const std::string &path) {
	const AssetEntry *asset = core_.project_file(path);
	if (!asset) return core_.report(make_finding(CoreFinding::DocumentMissing, DiagnosticSeverity::Error, "The file was not found.", path));
	const std::string relative = asset->relative_path;
	Diagnostic error;
	const std::shared_ptr<DocumentBase> document = load(relative, asset->kind, error);
	if (!document) return core_.report(error);
	if (document->rewrite_need() == DocumentBase::RewriteNeed::None) {
		view_.activity.status = relative + " has no changes to save.";
		core_.touch(ViewConcern::Output);
		return;
	}
	if (!document->save(error)) { // one that does not serialize says why here
		core_.report(error);
		view_.activity.status = relative + " could not be saved: see Problems.";
		core_.touch(ViewConcern::Output);
		return;
	}
	core_.note("Saved " + relative);
	core_.update_files({relative});
	view_.activity.status = "Saved 1 file(s).";
	core_.touch(ViewConcern::Output);
}

// EndEdit on every open document: the coalesced group (typing) and the gesture (a drag) end,
// and the validation a gesture's edits left waiting is due.
void DocumentSet::end_edit_groups() {
	for (const auto &document : documents_) document->end_edit_group();
	if (gesture_validation_due_) core_.problems().validate_later();
	gesture_validation_due_ = false;
}

void DocumentSet::discard(const std::string &path) {
	for (auto it = documents_.begin(); it != documents_.end(); ++it)
		if ((*it)->path() == path) { documents_.erase(it); break; }
	remembered_.erase(path);
	forget_file_state(path);
	update_view();
}

void DocumentSet::discard_all() {
	close_all();
	update_view();
}

void DocumentSet::close_all() {
	documents_.clear();
	remembered_.clear();
	stale_.clear();
	conflicts_.clear();
}

bool DocumentSet::position_after(const Document &document, const NodeAddress &record, NodeId &parent, size_t &position) {
	if (!record.child) {
		for (size_t i = 0; i < document.rows().size(); ++i)
			if (document.rows()[i]->id == record.row) {
				parent = 0;
				position = i + 1;
				return true;
			}
		return false;
	}
	Document::Placement at;
	if (!document.placement(record, at)) return false;
	parent = at.owner.child;
	position = at.index + 1;
	return true;
}

// --- the edits, the clipboard ------------------------------------------------------------------

// One EditRecord: a single edit or a batch on one row, then the selection follows (a
// new record selected, a removed one's owner) and the validation is left due (or, for a
// gesture, until it ends).
bool DocumentSet::apply_edits(DocumentBase &document, const std::vector<Edit> &edits) {
	last_edit_ok_ = false;
	// The selection follows a record document's records (a document of another kind holds none).
	const Document *records = records_of(document);
	NodeAddress owner;
	Document::Placement at;
	if (records && document.path() == view_.documents.active &&
	    records->placement(view_.documents.selection, at))
		owner = at.owner;
	Diagnostic error;
	const uint64_t before = document.revision();
	const std::string active = view_.documents.active;
	const NodeAddress primary = view_.documents.selection;
	const std::vector<NodeAddress> selected = view_.documents.selected;
	// A record's new name is its own edit: its uses keep the old name until Rename everywhere
	// (RenameSymbol) rewrites them.
	if (!document.apply(edits, error)) {
		core_.report(error);
		return false;
	}
	last_edit_ok_ = true;
	bool adds = false, gesture = false;
	for (const Edit &edit : edits) {
		adds = adds || edit.operation == EditOperation::Add || edit.operation == EditOperation::Duplicate ||
		       edit.operation == EditOperation::Paste;
		gesture = gesture || edit.gesture != 0;
	}
	if (adds && document.revision() != before) {
		if (records) core_.outcome().added = records->last_added_records();
		activate(document.path()); // what an edit adds is selected, in its own document
		if (records) view_.documents.select_added(*records);
	} else if (records) {
		view_.documents.repair_selection(*records, owner);
	}
	if (view_.documents.active != active || view_.documents.selection != primary ||
			view_.documents.selected != selected)
		core_.touch(ViewConcern::Selection);
	update_view();
	// A Move that leaves a record where it is changes nothing to validate; a gesture's
	// edits validate once it ends (EndEdit, Undo, Redo, Save).
	if (document.revision() != before) {
		if (gesture) gesture_validation_due_ = true;
		else core_.problems().validate_later();
	}
	view_.activity.status = "Edited " + document.path() + ".";
	core_.touch(ViewConcern::Output);
	return true;
}

void DocumentSet::copy_records(Document &document, bool cut) {
	last_edit_ok_ = false;
	const std::vector<NodeAddress> records =
	        document.path() == view_.documents.active ? document.outermost(view_.documents.selected) : std::vector<NodeAddress>();
	if (records.empty())
		return core_.refuse_now(CoreFinding::DocumentCopy, "Select the records to copy first.", document.path());
	std::string payload = document.copy(records);
	if (payload.empty())
		return core_.refuse_now(CoreFinding::DocumentCopy, "These records cannot be copied.", document.path());
	view_.documents.clipboard = std::move(payload);
	core_.touch(ViewConcern::Selection);
	if (!cut) {
		last_edit_ok_ = true;
		view_.activity.status = "Copied " + std::to_string(records.size()) + " record(s).";
		core_.touch(ViewConcern::Output);
		return;
	}
	std::vector<Edit> removes;
	for (const NodeAddress &record : records) {
		Edit edit;
		edit.operation = EditOperation::Remove;
		edit.address = record;
		removes.push_back(edit);
	}
	if (apply_edits(document, removes)) view_.activity.status = "Cut " + std::to_string(records.size()) + " record(s).";
}

void DocumentSet::paste_records(Document &document, const PasteAt &target) {
	last_edit_ok_ = false;
	if (view_.documents.clipboard.empty())
		return core_.refuse_now(CoreFinding::DocumentPaste, "The clipboard is empty: copy records first.", document.path());
	Edit edit;
	edit.operation = EditOperation::Paste;
	edit.address.row = target.row;
	edit.parent = target.parent;
	edit.position = target.position;
	edit.value = view_.documents.clipboard;
	if (!target.named()) {
		// No target named: beside the selected record (the one position rule, position_after), or
		// into the selected row, at its end.
		if (document.path() != view_.documents.active || !view_.documents.selection.row)
			return core_.refuse_now(CoreFinding::DocumentPaste, "Select where to paste.", document.path());
		edit.address.row = view_.documents.selection.row;
		if (!view_.documents.selection.child || !position_after(document, view_.documents.selection, edit.parent, edit.position)) {
			edit.parent = 0;
			edit.position = SIZE_MAX;
		}
	}
	apply_edits(document, {edit});
}

// Each selected record (a record inside another selected one goes with it) copied right
// after itself (the one position rule, position_after), one step: a row on its own, nested
// records as one batch.
void DocumentSet::duplicate_records(Document &document) {
	last_edit_ok_ = false;
	const std::vector<NodeAddress> records =
	        document.path() == view_.documents.active ? document.outermost(view_.documents.selected) : std::vector<NodeAddress>();
	if (records.empty())
		return core_.refuse_now(CoreFinding::DocumentDuplicate, "Select the records to duplicate first.", document.path());
	if (!records.front().child) {
		// A row: after itself among the rows (the selection stays inside one row, so it is alone).
		Edit edit;
		edit.operation = EditOperation::Duplicate;
		edit.address = records.front();
		NodeId rows = 0;
		position_after(document, edit.address, rows, edit.position);
		if (apply_edits(document, {edit})) view_.activity.status = "Duplicated a record.";
		return;
	}
	// In document order; a copy lands after its original, so the originals after it in the
	// same collection shift by the copies made before them.
	struct Item {
		NodeAddress record;
		Document::Placement at;
	};
	std::vector<Item> items;
	for (const NodeAddress &record : records) {
		Item item{record, {}};
		if (!document.placement(record, item.at))
			return core_.refuse_now(CoreFinding::DocumentSelection, "The selected record no longer exists.", document.path());
		items.push_back(item);
	}
	std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
		if (a.at.owner != b.at.owner) return a.at.owner.child < b.at.owner.child;
		if (a.at.spec.kind != b.at.spec.kind) return a.at.spec.kind < b.at.spec.kind;
		return a.at.index < b.at.index;
	});
	std::vector<Edit> edits;
	for (size_t i = 0; i < items.size(); ++i) {
		size_t before = 0; // copies already made in this record's collection, ahead of it
		for (size_t j = 0; j < i; ++j)
			before += items[j].at.owner == items[i].at.owner && items[j].at.spec.kind == items[i].at.spec.kind;
		Edit edit;
		edit.operation = EditOperation::Duplicate;
		edit.address = items[i].record;
		NodeId owner = 0;
		position_after(document, items[i].record, owner, edit.position);
		edit.position += before;
		edits.push_back(edit);
	}
	if (apply_edits(document, edits)) view_.activity.status = "Duplicated " + std::to_string(edits.size()) + " record(s).";
}

} // namespace opennova::editor
