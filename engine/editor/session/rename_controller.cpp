#include <editor/session/rename_controller.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

#include <editor/graph/asset_graph.h>
#include <editor/session/document_set.h>
#include <editor/session/problems_service.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

namespace {

// The name a rename request gives: a text, or a number typed as one (an item id).
std::string new_name_of(const EditorRequest &request) {
	if (const auto *text = std::get_if<std::string>(&request.edit.value)) return *text;
	if (const auto *number = std::get_if<int64_t>(&request.edit.value)) return std::to_string(*number);
	return std::string();
}

} // namespace

RenameController::RenameController(SessionCore &core) : core_(core), view_(core.view()), paths_(core.paths()) {}

// A file renamed with every reference rewritten (graph/rename_transaction), or refused
// with the reasons as findings; the rewritten documents that are open reload. A rename that
// would rewrite a file with unsaved edits, or leave them behind on the old name, never
// reaches here with them: it waits on the unsaved prompt first (UnsavedGuard), whose Save
// writes them.
void RenameController::rename_asset(const std::string &file, const std::string &new_name) {
	if (!view_.project_open) return;
	DocumentSet &documents = core_.documents();
	// The plan reads the graph: an edit a held pump made first reaches it (and the
	// Problems rows), so a reference it added is planned or refused like any other.
	core_.problems().validate_pending();
	const RenamePlan plan = plan_rename(paths_, view_.scan, core_.problems().graph(), file, new_name);
	if (!plan.ok()) {
		for (const Diagnostic &d : plan.refusals) core_.report(d);
		view_.status = "The rename was refused.";
		core_.touch(ViewConcern::Output);
		return;
	}
	std::vector<Diagnostic> findings;
	const bool ok = apply_rename(paths_, view_.document, view_.scan, core_.problems().graph(), plan, findings);
	std::vector<std::string> reload;
	for (const RenameSite &site : plan.sites)
		if (documents.document_for(site.file) && std::find(reload.begin(), reload.end(), site.file) == reload.end()) reload.push_back(site.file);
	// The document the modder was in stays active through the closes and reloads below
	// (the renamed file's own document follows it to the new name). A reloaded document
	// holds new records, so only an untouched one keeps its selection.
	std::string active = view_.active_document;
	const NodeAddress selection = view_.selection;
	const std::vector<NodeAddress> selected = view_.selected;
	bool keep_selection = std::find(reload.begin(), reload.end(), active) == reload.end();
	// A refused commit leaves the file where it was: its open document stays.
	DocumentBase *renamed = ok ? documents.document_for(plan.path) : nullptr;
	if (renamed) {
		const std::string renamed_path = renamed->path();
		const bool was_active = renamed_path == active;
		documents.close_document(renamed_path);
		core_.refresh();
		documents.open_document(make_request(EditorRequestKind::OpenDocument, plan.new_path));
		if (was_active) {
			active = view_.active_document;
			keep_selection = false;
		}
	}
	core_.refresh();
	for (const std::string &path : reload) documents.open_document(make_request(EditorRequestKind::ReloadDocument, path));
	bool active_open = active.empty();
	for (const auto &document : documents.documents()) active_open = active_open || document->path() == active;
	if (active_open) {
		documents.activate(active);
		view_.select_only(keep_selection ? selection : NodeAddress());
		if (keep_selection) view_.selected = selected;
		documents.select_first_screen();
	}
	// Reported last: the refresh and the reloads above rebuild the Problems rows.
	for (const Diagnostic &d : findings) core_.report(d);
	if (ok) {
		core_.note("Renamed " + plan.old_name + " to " + plan.new_name + " (" + std::to_string(plan.sites.size()) +
		           " reference" + (plan.sites.size() == 1 ? "" : "s") + " rewritten)");
		view_.status = "Renamed " + plan.old_name + " to " + plan.new_name + ".";
	} else {
		view_.status = "The rename did not finish.";
	}
	core_.touch(ViewConcern::Selection); // the active document and its selection, kept or started over
	core_.touch(ViewConcern::Output);
}

// Whether the project file at `path`, as saved, has a use that reaches exactly the renamed
// definition: the one the graph's lookup returns for it (users_of's rule), never a same-named
// definition another file shadows or another scope holds.
bool RenameController::saved_file_uses(const std::string &path, const SymbolRenamePlan &plan) const {
	const AssetGraph &graph = core_.problems().graph();
	const GraphSymbol *renamed = graph.symbol_at(plan.file, plan.locator, plan.field);
	const AssetEntry *asset = core_.project_file(path);
	Extracted saved;
	Diagnostic error;
	if (!renamed || renamed->inert || !asset || !extract_from_asset(paths_, view_.document, *asset, saved, error)) return false;
	for (const GraphEdge &edge : saved.edges)
		if (edge.kind == plan.kind && graph.resolve_symbol(edge.kind, edge.value, edge.scope) == renamed) return true;
	return false;
}

SymbolRenamePlan RenameController::plan_symbol(const EditorRequest &request) {
	// The plan reads the graph: an edit a held pump made first reaches it.
	core_.problems().validate_pending();
	const AssetGraph &graph = core_.problems().graph();
	const GraphSymbol *symbol = graph.symbol_at(request.path, request.text, request.edit.field);
	if (!symbol) {
		SymbolRenamePlan none;
		none.file = request.path;
		none.new_name = new_name_of(request);
		none.refusals.push_back(make_diagnostic(DiagnosticSeverity::Error, "rename.unknown_symbol",
		                                        request.path + " defines no name in field " + request.edit.field +
		                                                " of the record at " + request.text + ".",
		                                        request.path, request.edit.field));
		return none;
	}
	return plan_symbol_rename_project(view_.scan, graph, *symbol, new_name_of(request));
}

// What a rename would do, planned into the view (nothing written, nothing reported): a name's
// rename everywhere, or a file's (edit.field "").
void RenameController::preview(const EditorRequest &request) {
	if (!view_.project_open) return;
	SessionView::RenamePreview preview;
	preview.serial = view_.rename_preview.serial + 1;
	preview.ask_serial = view_.rename_preview.ask_serial + (request.flag ? 1 : 0);
	preview.requested = new_name_of(request);
	if (request.edit.field.empty()) {
		core_.problems().validate_pending();
		const RenamePlan plan = plan_rename(paths_, view_.scan, core_.problems().graph(), request.path, new_name_of(request));
		preview.path = plan.path.empty() ? request.path : plan.path;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.sites = plan.sites;
		preview.refusals = plan.refusals;
	} else {
		SymbolRenamePlan plan = plan_symbol(request);
		// What each file's own type makes of it, as the commit would (open documents as they
		// stand): a site its document refuses is a refusal here too.
		if (plan.ok()) {
			const std::vector<std::shared_ptr<DocumentBase>> &documents =
			        core_.documents().documents();
			std::vector<std::shared_ptr<const DocumentBase>> open(documents.begin(),
			                                                      documents.end());
			check_symbol_rename(paths_, view_.document, view_.scan, core_.problems().graph(), plan, open, plan.refusals);
		}
		preview.symbol = true;
		preview.kind = plan.kind;
		preview.path = plan.file;
		preview.locator = request.text;
		preview.field = request.edit.field;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.sites = plan.sites;
		preview.refusals = plan.refusals;
	}
	view_.rename_preview = std::move(preview);
	core_.touch(ViewConcern::Dialogs);
}

// A name renamed everywhere (graph/rename_transaction), or refused with the reasons as
// findings; the rewritten documents that are open reload. One that would rewrite a file with
// unsaved edits never reaches here with them: it waits on the unsaved prompt first
// (UnsavedGuard), whose Save writes them.
void RenameController::rename_symbol(const EditorRequest &request) {
	if (!view_.project_open) return;
	DocumentSet &documents = core_.documents();
	const SymbolRenamePlan plan = plan_symbol(request);
	if (!plan.ok()) {
		for (const Diagnostic &d : plan.refusals) core_.report(d);
		view_.status = "The rename was refused.";
		core_.touch(ViewConcern::Output);
		return;
	}
	std::vector<Diagnostic> findings;
	const bool ok = apply_symbol_rename(paths_, view_.document, view_.scan, core_.problems().graph(), plan, findings);
	std::vector<std::string> reload;
	for (const RenameSite &site : plan.sites)
		if (documents.document_for(site.file) && std::find(reload.begin(), reload.end(), site.file) == reload.end()) reload.push_back(site.file);
	// The document the modder was in stays active; one read again holds new records, so only an
	// untouched one keeps its selection.
	const std::string active = view_.active_document;
	const NodeAddress selection = view_.selection;
	const std::vector<NodeAddress> selected = view_.selected;
	const bool keep_selection = std::find(reload.begin(), reload.end(), active) == reload.end();
	core_.refresh();
	for (const std::string &path : reload) documents.open_document(make_request(EditorRequestKind::ReloadDocument, path));
	if (!active.empty() && documents.document_for(active)) {
		documents.activate(active);
		view_.select_only(keep_selection ? selection : NodeAddress());
		if (keep_selection) view_.selected = selected;
		// The renamed definition selected again where it was, its field shown.
		if (!keep_selection && active == plan.file)
			if (Document *defining = documents.records_for(active)) view_.select_only(defining->address_at(plan.locator));
		documents.select_first_screen();
	}
	for (const Diagnostic &d : findings) core_.report(d);
	if (ok) {
		const size_t uses = plan.sites.size() - 1;
		core_.note("Renamed " + std::string(reference_row(plan.kind).phrase) + " " + plan.old_name + " to " + plan.new_name +
		           " (" + std::to_string(uses) + " use" + (uses == 1 ? "" : "s") + " rewritten)");
		view_.status = "Renamed " + plan.old_name + " to " + plan.new_name + " everywhere.";
	} else {
		view_.status = "The rename did not finish.";
	}
	core_.touch(ViewConcern::Selection); // the active document and its selection, kept or started over
	core_.touch(ViewConcern::Output);
}

// A requirement satisfied by renaming a project file of the expected kind to the name
// the engine demands.
void RenameController::assign_requirement(const std::string &role, const std::string &file) {
	if (!view_.project_open) return;
	const RequirementRow *row = core_.requirement_row(role);
	if (!row) {
		core_.report(make_diagnostic(DiagnosticSeverity::Error, "requirement.unknown", "No requirement has the role '" + role + "'."));
		return;
	}
	// Nothing is renamed: a refusal, so whoever asked learns the assignment did not happen.
	if (row->state == RequirementState::Present) {
		core_.report(make_diagnostic(DiagnosticSeverity::Error, "requirement.assigned",
		                             row->name + " is already in the project: nothing was assigned.", row->asset_path));
		return;
	}
	const AssetEntry *asset = core_.project_file(file);
	if (!asset) {
		core_.report(make_diagnostic(DiagnosticSeverity::Error, "requirement.unknown_file", "The project has no file named '" + file + "'.", file));
		return;
	}
	if (asset->kind != row->expected_kind) {
		core_.report(make_diagnostic(DiagnosticSeverity::Error, "requirement.kind",
		                             asset->logical_name + " is " + asset_kind_label(asset->kind) + ", and " + row->name +
		                                     " must be " + asset_kind_label(row->expected_kind) + ".",
		                             asset->relative_path));
		return;
	}
	rename_asset(asset->relative_path, row->name);
}

void RenameController::unsaved_files(const EditorRequest &request, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	switch (request.kind) {
	case EditorRequestKind::RenameAsset: rename_unsaved(request.path, request.text, files); return;
	case EditorRequestKind::RenameSymbol:
		// The documents with unsaved edits among the files the plan rewrites, and those whose
		// saved file names the name (the commit reads the files on disk: an unsaved edit that
		// stopped naming it would leave the saved use behind); none when the plan is refused
		// anyway (the rename then says why, with nothing to save first). Saved, the rename is
		// planned again.
		if (view_.project_open && documents.documents_dirty()) {
			const SymbolRenamePlan plan = plan_symbol(request);
			if (!plan.ok()) return;
			for (const auto &document : documents.documents()) {
				if (!document->dirty()) continue;
				const bool site = std::any_of(plan.sites.begin(), plan.sites.end(),
				                              [&](const RenameSite &each) { return each.file == document->path(); });
				if (site || saved_file_uses(document->path(), plan)) files.push_back(document->path());
			}
		}
		return;
	case EditorRequestKind::AssignRequirement: {
		const RequirementRow *row = core_.requirement_row(request.text);
		const AssetEntry *asset = core_.project_file(request.path);
		if (row && asset && row->state != RequirementState::Present && asset->kind == row->expected_kind)
			rename_unsaved(asset->relative_path, row->name, files);
		return;
	}
	default: return;
	}
}

// The documents with unsaved edits a rename of `file` to `new_name` would rewrite (its
// plan's sites) or leave behind on the old name (the file's own), in the order they were
// opened; none when the plan is refused anyway (the rename then says why, with nothing to
// save first). The plan reads the graph: an edit a held pump made first reaches it.
void RenameController::rename_unsaved(const std::string &file, const std::string &new_name, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	if (!view_.project_open || !documents.documents_dirty()) return;
	core_.problems().validate_pending();
	const RenamePlan plan = plan_rename(paths_, view_.scan, core_.problems().graph(), file, new_name);
	if (!plan.ok()) return;
	for (const auto &document : documents.documents()) {
		if (!document->dirty()) continue;
		const std::string &path = document->path();
		if (path == plan.path || std::any_of(plan.sites.begin(), plan.sites.end(),
		                                     [&path](const RenameSite &site) { return site.file == path; }))
			files.push_back(path);
	}
}

} // namespace opennova::editor
