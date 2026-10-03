#include <editor/session/document_set.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/model/diagnostic.h>
#include <editor/model/field_text.h>
#include <editor/model/text_document.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/project/project_files.h>
#include <editor/session/problems_service.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <runtime/mission/mission_sidecars.h>

namespace fs = std::filesystem;

namespace opennova::editor {

DocumentSet::DocumentSet(SessionCore &core) : core_(core), view_(core.view()), paths_(core.paths()) {}

// --- what is open ----------------------------------------------------------------------------

DocumentBase *DocumentSet::document_for(const std::string &path) {
	const std::string &wanted = path.empty() ? view_.documents.active : path;
	for (auto &document : documents_)
		if (document->path() == wanted || normalized_logical_name(basename_of(document->path())) == normalized_logical_name(wanted))
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
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This kind of file has no editor yet.", relative);
		return nullptr;
	}
	std::shared_ptr<DocumentBase> document = type->make();
	if (!document->load(join_path(paths_.root, relative), relative, kind, view_.project.document->target_game,
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
	blank.values = request.values;
	for (const RequirementRow &row : view_.project.requirements->rows)
		if (row.expected_kind == kind && normalized_logical_name(row.name) == normalized_logical_name(request.path))
			blank.role = row.role;
	// A refusal says why in Problems, and the status line that nothing was made.
	const auto refuse = [&](const Diagnostic &finding) {
		core_.report(finding);
		view_.activity.status = request.path + " was not created: see Problems.";
		core_.touch(ViewConcern::Output);
	};
	const BlankFactory *factory = find_blank_factory_for_role(blank.role);
	if (!factory) factory = find_blank_factory_for_kind(kind);
	if (!factory) {
		refuse(make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "The editor cannot create this kind of file.", request.path));
		return;
	}
	// The values its blank takes (ADR 0046 S14): each one a parameter of the blank, a required one
	// given, and one that names a file a file of the project (a mission's terrain and environment:
	// a project is its own files), looked up as its loader looks it up. Nothing is made otherwise.
	std::string why;
	if (!blank_values_fit(*factory, blank, why)) {
		refuse(make_finding(CoreFinding::DocumentValues, DiagnosticSeverity::Error, why, request.path));
		return;
	}
	for (size_t i = 0; i < factory->param_count; ++i) {
		const BlankParam &param = factory->params[i];
		const std::string &value = blank.value(param.token);
		if (param.reference == ReferenceKind::None || value.empty()) continue;
		const auto in_project = [this](const std::string &name) { return view_.project.scan->find(name) != nullptr; };
		bool found = false;
		for (const std::string &candidate : reference_file_candidates(param.reference, value, -1, in_project))
			found = found || in_project(candidate);
		if (found) continue;
		refuse(make_finding(CoreFinding::DocumentValues, DiagnosticSeverity::Error,
		                          "The project has no " + std::string(param.token) + " named " + value +
		                                  ": import it first (Files > Import).",
		                          request.path));
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
		refuse(make_finding(code, DiagnosticSeverity::Error, message, request.path));
		return;
	}
	const auto *existing = view_.project.scan->find(request.path);
	const std::string relative = join_path(folder, request.path);
	if (!existing) {
		const auto target = path_of(paths_.root) / path_of(relative);
		std::error_code ec;
		if (fs::exists(system_path(utf8_of(target)), ec) || ec) { // a project past MAX_PATH too
			refuse(make_finding(CoreFinding::DocumentConflict, DiagnosticSeverity::Error, "Refresh before creating this file.", request.path));
			return;
		}
		std::vector<uint8_t> bytes; Diagnostic error;
		if (!make_blank(blank, kind, bytes, error)) { refuse(error); return; }
		if (!ensure_directory(utf8_of(target.parent_path()), message) ||
			!write_file_atomic(utf8_of(target), bytes.data(), bytes.size(), message)) {
			refuse(make_finding(CoreFinding::DocumentWrite, DiagnosticSeverity::Error, message, request.path));
			return;
		}
		std::vector<std::string> made{relative};
		core_.note("Created " + relative);
		// A new mission comes with the text table the game finds by its name (its title in the
		// mission list, its briefing), where the project has none of that name: made beside the
		// string tables. One that cannot be made leaves the mission made, and says so.
		if (kind == AssetKind::Mission) {
			const mission::Sidecar *text = mission::sidecar_for_role("text");
			const std::string table = text ? mission::sidecar_name(request.path, *text) : std::string();
			const BlankFactory *text_factory = find_blank_factory_for_role(kBlankMissionTextRole);
			if (!table.empty() && text_factory && !view_.project.scan->find(table)) {
				BlankRequest text_blank;
				text_blank.logical_name = table;
				text_blank.role = kBlankMissionTextRole;
				text_blank.project_title = blank.project_title;
				text_blank.values = {{"title", blank_mission_title(blank)}};
				const std::string text_relative = join_path(asset_kind_row(AssetKind::Strings).folder, table);
				const auto text_target = path_of(paths_.root) / path_of(text_relative);
				std::vector<uint8_t> text_bytes;
				if (!fs::exists(system_path(utf8_of(text_target)), ec) && text_factory->make(text_blank, text_bytes, error) &&
				    ensure_directory(utf8_of(text_target.parent_path()), message) &&
				    write_file_atomic(utf8_of(text_target), text_bytes.data(), text_bytes.size(), message)) {
					made.push_back(text_relative);
					core_.note("Created " + text_relative);
				} else {
					const std::string &reason = !message.empty() ? message : error.message;
					core_.report(make_finding(CoreFinding::DocumentWrite, DiagnosticSeverity::Warning,
					                          "The mission's text table " + table + " was not made" +
					                                  (reason.empty() ? std::string(".") : ": " + reason),
					                          request.path));
				}
			}
		}
		core_.update_files(made); // the files made, read into the scan alone
	}
	// The status line says what came of it (a kind the editor edits opened as well), never the line
	// an earlier request left.
	const std::string made = existing ? existing->relative_path + " is in the project already." : "Created " + relative + ".";
	if (is_editable_kind(kind)) {
		open_document(request::open_document(request.path));
		if (!document_for(request.path)) return; // the open said why
	}
	view_.activity.status = made;
	core_.touch(ViewConcern::Output);
}

void DocumentSet::open_document(const EditorRequest &request) {
	if (!view_.project.open) return;
	const std::string path = request.path.empty() ? view_.documents.active : request.path;
	// The record a request names (by its address, or by its locator: a Go to) is selected,
	// and its field (a Problems row's, the defining field a Go to shows) shown: a RevealRecord
	// event for the document's view and the Inspector, one per ask (the same row clicked again
	// shows it again). A text document's place (its locator "line:column": a Go to's span, a
	// Problems row's line) is a RevealText event for its view.
	const auto select_named = [this, &request](const DocumentBase &document) {
		if (text_of(document)) {
			size_t line = 0, column = 0;
			if (!TextDocument::read_locator(request.locator, line, column)) return;
			ViewEvent reveal;
			reveal.kind = ViewEventKind::RevealText;
			reveal.path = document.path();
			reveal.locator = request.locator;
			view_.events.post(std::move(reveal));
			return;
		}
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
	// The status line says what came of the request (never the line an earlier one left): the
	// document shown, read or read again, or that it was not.
	const auto say = [this](std::string line) {
		view_.activity.status = std::move(line);
		core_.touch(ViewConcern::Output);
	};
	const auto refuse = [&](const Diagnostic &finding) {
		core_.report(finding);
		say(path + " could not be opened: see Problems.");
	};
	if (request.kind == EditorRequestKind::OpenDocument && document_for(path)) {
		// An open document comes back with the selection it had, unless the request names
		// a record (a Problems row, a Go to).
		const DocumentBase &document = *document_for(path);
		activate(document.path());
		if (request.address.row || !request.locator.empty()) select_named(document);
		core_.touch(ViewConcern::Selection);
		say("Showing " + document.path() + ".");
		return;
	}
	for (const auto &asset : view_.project.scan->entries) {
		if (asset.relative_path != path && normalized_logical_name(asset.logical_name) != normalized_logical_name(path)) continue;
		const DocumentType *type = document_type_for(asset.kind);
		if (!type) {
			refuse(make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This kind of file has no editor yet.", path));
			return;
		}
		std::shared_ptr<DocumentBase> document = type->make(); Diagnostic error;
		if (!document->load(join_path(paths_.root, asset.relative_path),
					asset.relative_path, asset.kind, view_.project.document->target_game, error)) {
			refuse(error);
			return;
		}
		for (auto it = documents_.begin(); it != documents_.end(); ++it)
			if ((*it)->path() == asset.relative_path) { documents_.erase(it); break; }
		remembered_.erase(asset.relative_path); // read again: its records have new identities
		forget_file_state(asset.relative_path);
		end_gesture_in(asset.relative_path, true); // and a gesture open in it is over
		documents_.push_back(document);
		activate(document->path());
		select_named(*document);
		select_first_screen(); // no record named: a menu shows its first screen
		core_.touch(ViewConcern::Selection);
		say((request.kind == EditorRequestKind::ReloadDocument ? "Reloaded " : "Opened ") + document->path() + ".");
		update_view(); core_.problems().validate_later(); return;
	}
	refuse(make_finding(CoreFinding::DocumentMissing, DiagnosticSeverity::Error, "The file was not found.", path));
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
	// Files selects it, as a click there does (S18).
	select_file(asset->relative_path);
	core_.touch(ViewConcern::Selection);
}

void DocumentSet::select_file(const std::string &path) {
	if (!view_.project.open) return;
	FileSelection selected;
	if (!path.empty()) {
		const AssetEntry *asset = core_.project_file(path);
		if (!asset) {
			core_.report(make_finding(CoreFinding::DocumentMissing, DiagnosticSeverity::Error, "The file was not found.", path));
			return;
		}
		selected.path = asset->relative_path;
		selected.type = asset_kind_row(asset->kind).document;
	}
	// A file a viewport draws whether or not it is open leads the Preview window (a texture's).
	const bool lead = file_preview_kind(selected.type) != ViewportKind::kCount;
	if (selected.path == view_.documents.file_selected.path && lead == view_.documents.files_lead) return;
	view_.documents.file_selected = std::move(selected);
	view_.documents.files_lead = lead;
	core_.touch(ViewConcern::Selection);
}

void DocumentSet::close_document(const std::string &requested) {
	const std::string path = requested.empty() ? view_.documents.active : requested;
	for (auto it = documents_.begin(); it != documents_.end(); ++it)
		if ((*it)->path() == path) { documents_.erase(it); break; }
	remembered_.erase(path);
	forget_file_state(path);
	end_gesture_in(path, true);
	if (view_.documents.active == path) {
		activate(documents_.empty() ? "" : documents_.back()->path());
		core_.touch(ViewConcern::Selection);
	}
	update_view(); core_.problems().validate_later();
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
	const uint64_t before = document->revision(), generation = document->load_generation();
	const uint64_t serial = view_.documents.selection.serial;
	const bool undo = request.kind == EditorRequestKind::Undo;
	// What the step did, said as it is taken back or made again (the UX round's problems lane).
	const std::string words = undo ? document->undo_words() : document->redo_words();
	if (undo) document->undo(); else document->redo();
	if (document->revision() != before)
		repair_selection(*document, generation, before, NodeAddress());
	if (view_.documents.selection.serial != serial) core_.touch(ViewConcern::Selection);
	const std::string file = basename_of(document->path());
	view_.activity.status = document->revision() == before
	                                ? std::string(undo ? "Nothing to undo in " : "Nothing to redo in ") + file + "."
	                                : std::string(undo ? "Undid: " : "Redid: ") + (words.empty() ? "an edit of " + file : words) + ".";
	core_.touch(ViewConcern::Output);
	update_view();
	// An undo or a redo ends the document's gesture: the validation its edits left waiting runs now.
	if (document->revision() != before) core_.problems().validate_later();
	end_gesture_in(document->path(), true);
}

// The coalesced group, or the gesture, of the document at `path` ends; a gesture open in another
// document stays open (its canvas ends it as it lets go, the wire's as its last sample comes).
void DocumentSet::end_edit(const std::string &path) {
	DocumentBase *document = document_for(path);
	if (!document) return;
	document->end_edit_group();
	end_gesture_in(document->path(), true);
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
	// A Save ends the gesture of each document it saves (its save's checkpoint ended its group): a
	// file written is validated as the scan reads it, and one not written now.
	for (const std::string &path : paths)
		end_gesture_in(path, std::find(written.begin(), written.end(), path) == written.end());
	update_view();
	core_.update_files(written);
	// Reported after the scan's update, which leaves the validation that rebuilds the rows due.
	for (const Diagnostic &d : failures) core_.report(d);
	view_.activity.status = "Saved " + counted(saved, "file") +
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
	view_.activity.status = "Saved " + relative + ".";
	core_.touch(ViewConcern::Output);
}

// EndEdit on every open document: the coalesced group (typing) and the gesture (a drag) end,
// and the validation a gesture's edits left waiting is due.
void DocumentSet::end_edit_groups() {
	for (const auto &document : documents_) document->end_edit_group();
	end_gestures(true);
}

bool DocumentSet::gesture_open() const {
	return !view_.documents.gestures.empty();
}

void DocumentSet::open_gesture(const std::string &path, uint64_t token, int64_t sampled_ms) {
	std::vector<OpenGesture> &gestures = view_.documents.gestures;
	for (OpenGesture &gesture : gestures) {
		if (gesture.path != path) continue;
		if (gesture.token == token) {
			if (sampled_ms >= 0) gesture.sampled_ms = sampled_ms;
			return;
		}
		// Another gesture changing the document: the one open in it ends first.
		end_gesture_in(path, true);
		break;
	}
	gestures.push_back(OpenGesture{ path, token, sampled_ms });
}

void DocumentSet::end_gesture_in(const std::string &path, bool validate) {
	std::vector<OpenGesture> &gestures = view_.documents.gestures;
	const auto open = std::find_if(gestures.begin(), gestures.end(),
			[&path](const OpenGesture &gesture) { return gesture.in(path); });
	if (open == gestures.end()) return;
	gestures.erase(open);
	if (validate) core_.problems().validate_later();
}

void DocumentSet::end_gestures(bool validate) {
	if (view_.documents.gestures.empty()) return;
	view_.documents.gestures.clear();
	if (validate) core_.problems().validate_later();
}

void DocumentSet::discard(const std::string &path) {
	for (auto it = documents_.begin(); it != documents_.end(); ++it)
		if ((*it)->path() == path) { documents_.erase(it); break; }
	remembered_.erase(path);
	forget_file_state(path);
	end_gesture_in(path, true);
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
	end_gestures(false);
	// No file of a project closed is selected in Files any more.
	view_.documents.file_selected = FileSelection();
	view_.documents.files_lead = false;
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

namespace {

// A batch of Removes alone, each naming a record it holds (a Delete, a Cut; never one naming what
// an earlier edit of its batch made).
bool removes_only(const std::vector<Edit> &edits) {
	for (const Edit &edit : edits)
		if (edit.operation != EditOperation::Remove || is_batch_made(edit.address.row) ||
		    is_batch_made(edit.address.child))
			return false;
	return !edits.empty();
}

// A batch of Sets of one field, each naming a record it holds (the Inspector's edit of a field, on
// one record or on several together).
bool sets_one_field(const std::vector<Edit> &edits) {
	for (const Edit &edit : edits)
		if (edit.operation != EditOperation::Set || edit.field.empty() || edit.field != edits.front().field ||
		    is_batch_made(edit.address.row) || is_batch_made(edit.address.child))
			return false;
	return !edits.empty();
}

// A batch of Sets of one record (a drag's sides of one window).
bool sets_one_record(const std::vector<Edit> &edits) {
	for (const Edit &edit : edits)
		if (edit.operation != EditOperation::Set || edit.field.empty() || !(edit.address == edits.front().address) ||
		    is_batch_made(edit.address.row) || is_batch_made(edit.address.child))
			return false;
	return !edits.empty();
}

} // namespace

// One EditRecord: a single edit or a batch over any rows, then the selection follows (what
// the edit made selected, a removed primary's owner) and the validation is left due (or, for a
// gesture, until it ends). A batch of Removes alone is what the type makes of removing those
// records (Document::removal_edits, S14: a mission's marker takes the stops that visit it with
// it), refused as the type refuses it (document.collection), nothing applied.
bool DocumentSet::apply_edits(DocumentBase &document, const std::vector<Edit> &requested) {
	last_edit_ok_ = false;
	// The selection follows a record document's records (a document of another kind holds none).
	const Document *records = records_of(document);
	std::vector<Edit> expanded;
	const std::vector<Edit> *edits = &requested;
	if (records && removes_only(requested)) {
		std::vector<NodeAddress> removed;
		for (const Edit &edit : requested) removed.push_back(edit.address);
		std::string why;
		if (!records->removal_edits(removed, expanded, why)) {
			core_.refuse_now(CoreFinding::DocumentCollection,
			                 why.empty() ? std::string("These records cannot be removed.") : why, document.path());
			return false;
		}
		for (Edit &edit : expanded) edit.gesture = requested.front().gesture;
		edits = &expanded;
	}
	NodeAddress owner;
	Document::Placement at;
	if (records && document.path() == view_.documents.active &&
	    records->placement(view_.documents.selection.primary, at))
		owner = at.owner;
	Diagnostic error;
	const uint64_t before = document.revision(), generation = document.load_generation();
	const std::string active = view_.documents.active;
	const uint64_t serial = view_.documents.selection.serial;
	// The records a removal takes or a set changes, in words as they read before the edit (the status
	// line's, ADR 0046 S15: a set of an entity's item renames it).
	const bool removal = records && removes_only(requested);
	const std::string named = records && (removal || sets_one_field(requested) || sets_one_record(requested))
	                                  ? records_words(*records, requested)
	                                  : std::string();
	// A record's new name is its own edit: its uses keep the old name until Rename everywhere
	// (RenameSymbol) rewrites them.
	if (!document.apply(*edits, error)) {
		core_.report(error);
		return false;
	}
	last_edit_ok_ = true;
	bool adds = false;
	uint64_t gesture = 0;
	for (const Edit &edit : *edits) {
		adds = adds || edit.operation == EditOperation::Add || edit.operation == EditOperation::Duplicate ||
		       edit.operation == EditOperation::Paste;
		if (!gesture) gesture = edit.gesture;
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
	// edits validate once it ends (EndEdit, Undo, Redo, Save), the gesture the document's open one
	// meanwhile (S13 V7).
	if (document.revision() != before) {
		if (gesture) open_gesture(document.path(), gesture);
		else core_.problems().validate_later();
	}
	view_.activity.status = removal ? "Removed " + named + "." : records ? edit_words(*records, *edits, named) : std::string();
	if (view_.activity.status.empty()) view_.activity.status = "Edited " + document.path() + ".";
	// The step is named with the same words, what Undo and Redo say and the Edit menu names.
	if (document.revision() != before) name_step(document);
	core_.touch(ViewConcern::Output);
	return true;
}

// The step the last edit made named with the status line's words (DocumentBase::name_step), its full
// stop dropped: "Undid: <words>." puts it back.
void DocumentSet::name_step(DocumentBase &document) const {
	std::string words = view_.activity.status;
	while (!words.empty() && words.back() == '.') words.pop_back();
	document.name_step(std::move(words));
}

// The records of edits in words (ADR 0046 S15: record_display with the graph's names): one by its
// title ("Ranger #12"), several by their count.
std::string DocumentSet::records_words(const Document &document, const std::vector<Edit> &edits) const {
	std::vector<NodeAddress> records;
	for (const Edit &edit : edits)
		if (std::find(records.begin(), records.end(), edit.address) == records.end()) records.push_back(edit.address);
	if (records.size() != 1) return std::to_string(records.size()) + " records";
	std::optional<GraphNameSource> names;
	if (view_.findings.graph) names.emplace(*view_.findings.graph);
	return record_display(document, records.front(), names ? &*names : nullptr);
}

// What a batch did, in words, for the status line (ADR 0046 S15): a field set by its name, on its
// record by its title as it read before (`named`), to what its new value names ("Set Group of Ranger
// #12 to Group 3 (5 entities)."); a row added by its title; "" for any other batch (the line then
// names the file).
std::string DocumentSet::edit_words(const Document &document, const std::vector<Edit> &edits,
                                    const std::string &named) const {
	std::optional<GraphNameSource> names;
	if (view_.findings.graph) names.emplace(*view_.findings.graph);
	const NameSource *source = names ? &*names : nullptr;
	if (edits.empty()) return std::string();
	const Edit &first = edits.front();
	if (!named.empty() && sets_one_field(edits) && document.row(first.address.row)) {
		const FieldSchema *schema = nullptr;
		for (const FieldSchema &field : document.fields(first.address.kind))
			if (field.id == first.field) schema = &field;
		if (!schema) return std::string();
		const FieldUse use = document.field_on(first.address, *schema);
		Value value;
		if (!document.get(first.address, first.field, value)) return std::string();
		const DisplayName words = value_display(document, first.address, use, value, source);
		std::vector<FieldChoice> own;
		const std::string shown = words.text.empty() ? shown_value(*schema, document.choices_on(first.address, use, own), value) : words.text;
		return "Set " + field_title(use) + " of " + named + " to " + shown + ".";
	}
	// Several fields of one record (a drag's left, top, right and bottom; the UX round's problems lane):
	// the fields by name, on the record by its title as it read before.
	if (!named.empty() && edits.size() > 1 && document.row(first.address.row)) {
		std::vector<std::string> titles;
		for (const Edit &edit : edits) {
			if (edit.operation != EditOperation::Set || !(edit.address == first.address)) return std::string();
			std::string title = edit.field;
			for (const FieldSchema &field : document.fields(edit.address.kind))
				if (field.id == edit.field) title = field_title(document.field_on(edit.address, field));
			if (std::find(titles.begin(), titles.end(), title) == titles.end()) titles.push_back(title);
		}
		std::string joined;
		for (size_t i = 0; i < titles.size(); ++i)
			joined += (i == 0 ? "" : i + 1 == titles.size() ? " and " : ", ") + titles[i];
		return "Set " + joined + " of " + named + ".";
	}
	if (edits.size() == 1 && first.operation == EditOperation::Add && document.last_added())
		return "Added " + record_display(document, document.address_of(document.last_added()), source) + ".";
	if (std::all_of(edits.begin(), edits.end(), [](const Edit &edit) { return edit.operation == EditOperation::Duplicate; }))
		return edits.size() == 1 ? "Duplicated " + record_display(document, first.address, source) + "."
		                         : "Duplicated " + counted(edits.size(), "record") + ".";
	return std::string();
}

void DocumentSet::copy_records(Document &document, bool cut) {
	last_edit_ok_ = false;
	const std::vector<NodeAddress> records =
	        document.path() == view_.documents.active
	                ? document.outermost(view_.documents.selection.records)
	                : std::vector<NodeAddress>();
	if (records.empty())
		return core_.refuse_now(CoreFinding::DocumentCopy, "Select the records to copy first.", document.path());
	std::string payload = document.copy(records);
	if (payload.empty())
		return core_.refuse_now(CoreFinding::DocumentCopy, "These records cannot be copied.", document.path());
	if (!cut) {
		view_.documents.clipboard = std::move(payload);
		core_.touch(ViewConcern::Selection);
		last_edit_ok_ = true;
		view_.activity.status = "Copied " + counted(records.size(), "record") + ".";
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
	// The records go to the clipboard once their removes apply: a cut the document refuses (a menu
	// screen's only root window among windows of several screens) leaves the clipboard as it was.
	if (!apply_edits(document, removes)) return;
	view_.documents.clipboard = std::move(payload);
	core_.touch(ViewConcern::Selection);
	view_.activity.status = "Cut " + counted(records.size(), "record") + ".";
	name_step(document);
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
	// Rows of the file (a payload the type pastes at the top level, Document::pastes_rows, S14): a
	// named target's position among the rows; else after the selected record's row (the one
	// position rule, position_after), or at the end with none selected. The type's order then puts
	// each row where it goes (Document::row_position).
	const bool rows = document.pastes_rows(view_.documents.clipboard);
	if (rows) {
		edit.address.row = 0;
		edit.parent = 0;
	}
	if (!target.named()) {
		const NodeAddress &primary = view_.documents.selection.primary;
		const bool selected = document.path() == view_.documents.active && primary.row;
		if (rows) {
			edit.position = SIZE_MAX;
			if (selected) position_after(document, {primary.row, primary.kind, 0}, edit.parent, edit.position);
		} else {
			// No target named: beside the primary record, or into the primary row, at its end.
			if (!selected)
				return core_.refuse_now(CoreFinding::DocumentPaste, "Select where to paste.", document.path());
			edit.address.row = primary.row;
			if (!primary.child || !position_after(document, primary, edit.parent, edit.position)) {
				edit.parent = 0;
				edit.position = SIZE_MAX;
			}
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
		return core_.refuse_now(CoreFinding::DocumentDuplicate, "Select the records to duplicate first.", document.path());
	std::vector<NodeAddress> around = document.ancestors(view_.documents.selection.primary);
	around.push_back(view_.documents.selection.primary);
	size_t primary_edit = SIZE_MAX;
	std::vector<Edit> edits;
	for (const NodeAddress &record : records) {
		if (!has_record(document, record))
			return core_.refuse_now(CoreFinding::DocumentSelection, "The selected record no longer exists.", document.path());
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
	view_.activity.status = edits.size() == 1 ? "Duplicated " + records_words(document, edits) + "."
	                                          : "Duplicated " + counted(edits.size(), "record") + ".";
	name_step(document);
}

} // namespace opennova::editor
