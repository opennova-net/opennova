#include <editor/session/document_set.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>

#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
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
		return core_.refuse_now("document.no_records", "This document holds no records.",
		                        document->path());
	core_.refuse_now("document.not_open", not_open, path);
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
	Selection &selection = view_.documents.selection;
	if (open_at(view_.documents.active)) remembered_[view_.documents.active] = selection;
	view_.documents.active = path;
	selection.select_only(path, NodeAddress());
	const auto kept = remembered_.find(path);
	if (kept != remembered_.end()) {
		selection.restore(kept->second);
		remembered_.erase(kept);
		if (const DocumentBase *document = open_at(path))
			if (const Document *records = records_of(*document))
				selection.repair(*records, nullptr, NodeAddress());
	}
	select_first_screen();
	core_.touch(ViewConcern::ActiveDocument); // the caller touches Selection
}

// A menu made the active document, or read again, with nothing selected shows its first
// screen: the menu view lists the selected screen's windows and the preview draws it.
void DocumentSet::select_first_screen() {
	if (view_.documents.selection.primary.row) return;
	const Document *document = records_for();
	if (!document || document->kind() != AssetKind::Menu || document->rows().empty()) return;
	const Node &screen = *document->rows().front();
	view_.documents.selection.select_only(view_.documents.active, {screen.id, screen.kind, 0});
}

// --- the files -------------------------------------------------------------------------------

std::shared_ptr<DocumentBase> DocumentSet::load(const std::string &relative, AssetKind kind,
                                                Diagnostic &error) const {
	const DocumentType *type = document_type_for(kind);
	if (!type) {
		error = make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This kind of file has no editor yet.", relative);
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
		if (path == view_.documents.active)
			view_.documents.selection.select_only(path, NodeAddress());
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
		Diagnostic d = make_diagnostic(DiagnosticSeverity::Error, "document.stale",
		                               "This file changed outside the editor and could not be read again, so the editor "
		                               "shows it as it was: " + reason.message + " Correct the file and Refresh, or close it.",
		                               path);
		d.line = reason.line;
		findings.push_back(std::move(d));
	}
	for (const std::string &path : conflicts_)
		findings.push_back(make_diagnostic(DiagnosticSeverity::Warning, "document.conflict",
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
		core_.report(make_diagnostic(DiagnosticSeverity::Error, "document.kind", "The editor cannot create this kind of file.", request.path));
		return;
	}
	// A plain name the archives can carry, whose extension is the kind's, landing
	// inside the project.
	std::string problem, message;
	if (!check_project_file_name(paths_.root, blank_placement_dir(kind), request.path, kind, problem, message)) {
		core_.report(make_diagnostic(DiagnosticSeverity::Error, "document." + problem, message, request.path));
		return;
	}
	const auto *existing = view_.project.scan->find(request.path);
	const std::string relative = (fs::path(blank_placement_dir(kind)) / request.path).generic_string();
	if (!existing) {
		const auto target = fs::path(paths_.root) / relative;
		std::error_code ec;
		if (fs::exists(target, ec) || ec) {
			core_.report(make_diagnostic(DiagnosticSeverity::Error, "document.conflict", "Refresh before creating this file.", request.path));
			return;
		}
		std::vector<uint8_t> bytes; Diagnostic error;
		if (!make_blank(blank, kind, bytes, error)) { core_.report(error); return; }
		if (!ensure_directory(target.parent_path().generic_string(), message) ||
			!write_file_atomic(target.generic_string(), bytes.data(), bytes.size(), message)) {
			core_.report(make_diagnostic(DiagnosticSeverity::Error, "document.write", message, request.path));
			return;
		}
		core_.refresh();
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
		view_.documents.selection.select_only(document.path(), record);
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
			core_.report(make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This kind of file has no editor yet.", path));
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
		update_view(); core_.problems().validate_documents(); return;
	}
	core_.report(make_diagnostic(DiagnosticSeverity::Error, "document.missing", "The file was not found.", path));
}

// Files shows the file (and asks its new name when the request says so): a RevealFile event,
// one per ask, so the same file asked again is shown again.
void DocumentSet::show_in_files(const EditorRequest &request) {
	if (!view_.project.open) return;
	const AssetEntry *asset = core_.project_file(request.path);
	if (!asset) {
		core_.report(make_diagnostic(DiagnosticSeverity::Error, "document.missing", "The file was not found.", request.path));
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
	update_view(); core_.problems().validate_documents();
}

// The document's own path, however the request named it (a logical name included), so a
// selection joined by path stays in one document; records of another document make it the
// active one, the records named its selection. Only the records the document has are selected:
// one it does not hold, or of another kind than named, is left out (a repair after an edit asks
// only about the rows the edit changed).
void DocumentSet::select_record(const EditorRequest &request) {
	const DocumentBase *document = document_for(request.path);
	const std::string path = document ? document->path() : request.path.empty() ? view_.documents.active : request.path;
	const bool elsewhere = path != view_.documents.active;
	if (elsewhere) activate(path);
	const Document *records = document ? records_of(*document) : nullptr;
	const auto held = [records](const NodeAddress &address) {
		return records && has_record(*records, address);
	};
	std::vector<NodeAddress> others;
	for (const NodeAddress &address : request.records)
		if (held(address)) others.push_back(address);
	view_.documents.selection.select(path, held(request.address) ? request.address : NodeAddress(),
	                                 others, elsewhere ? SelectMode::Replace : request.mode);
	core_.touch(ViewConcern::Selection);
}

void DocumentSet::edit_record(const EditorRequest &request) {
	// A fix's edit opens its document first (a Problems row about a file not open).
	if (!document_for(request.path) && request.open_first && view_.project.open &&
	    !request.path.empty())
		open_document(request::open_document(request.path));
	auto *document = document_for(request.path);
	if (!document) {
		if (view_.project.open) core_.refuse_now("document.not_open", "Open the file before editing it.", request.path);
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
		core_.refuse_now("document.revert_nothing",
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
		if (view_.project.open) core_.refuse_now("document.not_open", "Open the file before undoing or redoing in it.", request.path);
		return;
	}
	const uint64_t before = document->revision(), generation = document->load_generation();
	const uint64_t serial = view_.documents.selection.serial;
	if (request.kind == EditorRequestKind::Undo) document->undo(); else document->redo();
	if (document->revision() != before)
		repair_selection(*document, generation, before, NodeAddress());
	if (view_.documents.selection.serial != serial) core_.touch(ViewConcern::Selection);
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
		if (path.empty()) core_.refuse_now("document.not_open", "Open a file before saving it.");
		else rewrite_file(path);
	}
}

void DocumentSet::save_all() {
	save_documents(dirty_files(), false);
}

// Writes each open document at `paths` that has unsaved edits (`rewrite`: an explicit Save,
// which also writes one with none whose file holds other bytes than it would write, and
// refuses one that does not serialize), past a failure: an Output line per file written, the
// refresh, then one finding per file that could not be, the status counting both. False when
// one could not be. (A save while a build packs never reaches here: the busy gate refused it.)
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
	for (DocumentBase *document : writes) {
		Diagnostic error;
		if (!document->save(error)) {
			// A file changed outside the editor under unsaved edits: the conflict is a row
			// until the document is read again (its Reload fix).
			if (error.code == "document.conflict" && document->dirty()) conflicts_.insert(document->path());
			failures.push_back(error);
			continue;
		}
		forget_file_state(document->path());
		++saved;
		core_.note("Saved " + document->path());
	}
	gesture_validation_due_ = false;
	update_view();
	core_.refresh();
	// Reported after the refresh, which rebuilds the Problems rows.
	for (const Diagnostic &d : failures) core_.report(d);
	view_.activity.status = "Saved " + std::to_string(saved) + " file(s)" +
	               (failures.empty() ? "." : "; " + std::to_string(failures.size()) + " could not be saved: see Problems.");
	core_.touch(ViewConcern::Output);
	return failures.empty();
}

// A Save of a file that is not open: read as its document type, written when it would
// write other bytes than the file holds (the canonical rewrite: the lines the game ignores
// dropped, the line ends fixed, a table regrouped), and left closed; the refresh then reads
// the file as written, so the findings the rewrite fixed leave Problems. One that does not
// serialize is refused with the reason (document.unserializable), the file untouched. Of two
// files of one name, the one project_file picks: the path named, else the first of the name.
void DocumentSet::rewrite_file(const std::string &path) {
	const AssetEntry *asset = core_.project_file(path);
	if (!asset) return core_.report(make_diagnostic(DiagnosticSeverity::Error, "document.missing", "The file was not found.", path));
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
	core_.refresh();
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

void DocumentSet::repair_selection(const DocumentBase &document, uint64_t load_generation,
                                   uint64_t revision, const NodeAddress &owner) {
	const Document *records = records_of(document);
	if (!records) return; // a document of another kind holds no records to select
	ChangeSet changes;
	const bool known = document.changes_since(load_generation, revision, changes);
	view_.documents.selection.repair(*records, known ? &changes : nullptr, owner);
}

// One EditRecord: a single edit or a batch over any rows, then the selection follows (what
// the edit made selected, a removed primary's owner) and the validation is left due (or, for a
// gesture, until it ends).
bool DocumentSet::apply_edits(DocumentBase &document, const std::vector<Edit> &edits) {
	last_edit_ok_ = false;
	// The selection follows a record document's records (a document of another kind holds none).
	const Document *records = records_of(document);
	NodeAddress owner;
	Document::Placement at;
	if (records && document.path() == view_.documents.active &&
	    records->placement(view_.documents.selection.primary, at))
		owner = at.owner;
	Diagnostic error;
	const uint64_t before = document.revision(), generation = document.load_generation();
	const std::string active = view_.documents.active;
	const uint64_t serial = view_.documents.selection.serial;
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
	// What the batch made and kept is selected, in its own document; a batch that kept nothing it
	// made repairs the selection as any other edit does.
	const bool made = adds && records && document.revision() != before;
	if (made) core_.outcome().made = records->last_made();
	if (made && !records->last_added_records().empty()) {
		core_.outcome().added = records->last_added_records();
		activate(document.path());
		view_.documents.selection.select_added(*records);
	} else if (document.revision() != before) {
		repair_selection(document, generation, before, owner);
	}
	if (view_.documents.active != active || view_.documents.selection.serial != serial)
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
	        document.path() == view_.documents.active
	                ? document.outermost(view_.documents.selection.records)
	                : std::vector<NodeAddress>();
	if (records.empty())
		return core_.refuse_now("document.copy", "Select the records to copy first.", document.path());
	std::string payload = document.copy(records);
	if (payload.empty())
		return core_.refuse_now("document.copy", "These records cannot be copied.", document.path());
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
		return core_.refuse_now("document.paste", "The clipboard is empty: copy records first.", document.path());
	Edit edit;
	edit.operation = EditOperation::Paste;
	edit.address.row = target.row;
	edit.parent = target.parent;
	edit.position = target.position;
	edit.value = view_.documents.clipboard;
	if (!target.named()) {
		// No target named: beside the primary record (the one position rule, position_after), or
		// into the primary row, at its end.
		const NodeAddress &primary = view_.documents.selection.primary;
		if (document.path() != view_.documents.active || !primary.row)
			return core_.refuse_now("document.paste", "Select where to paste.", document.path());
		edit.address.row = primary.row;
		if (!primary.child || !position_after(document, primary, edit.parent, edit.position)) {
			edit.parent = 0;
			edit.position = SIZE_MAX;
		}
	}
	apply_edits(document, {edit});
}

// Each selected record (a record inside another selected one goes with it) copied right
// after itself as the copies before it left its list (Edit::position's default for a Duplicate),
// rows and nested records of any rows in one batch, one step; the copies are selected, the copy
// of the primary (or of the record holding it) the primary, so the preview stays where it was.
void DocumentSet::duplicate_records(Document &document) {
	last_edit_ok_ = false;
	const std::vector<NodeAddress> records =
	        document.path() == view_.documents.active
	                ? document.outermost(view_.documents.selection.records)
	                : std::vector<NodeAddress>();
	if (records.empty())
		return core_.refuse_now("document.duplicate", "Select the records to duplicate first.", document.path());
	std::vector<NodeAddress> around = document.ancestors(view_.documents.selection.primary);
	around.push_back(view_.documents.selection.primary);
	size_t primary_edit = SIZE_MAX;
	std::vector<Edit> edits;
	for (const NodeAddress &record : records) {
		if (!has_record(document, record))
			return core_.refuse_now("document.selection", "The selected record no longer exists.", document.path());
		if (std::find(around.begin(), around.end(), record) != around.end())
			primary_edit = edits.size();
		Edit edit;
		edit.operation = EditOperation::Duplicate;
		edit.address = record;
		edits.push_back(edit);
	}
	if (!apply_edits(document, edits)) return;
	const std::vector<NodeId> &made = document.last_made();
	if (primary_edit < made.size() && made[primary_edit]) {
		view_.documents.selection.make_primary(document.address_of(made[primary_edit]));
		core_.touch(ViewConcern::Selection);
	}
	view_.activity.status = edits.size() == 1
	                                ? std::string("Duplicated a record.")
	                                : "Duplicated " + std::to_string(edits.size()) + " record(s).";
}

} // namespace opennova::editor
