#include <editor/session/rename_controller.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>

#include <base/io/hash.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/navigation_controller.h>
#include <editor/session/problems_service.h>
#include <editor/session/rename_operation.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

RenameController::RenameController(SessionCore &core) : core_(core), view_(core.view()), paths_(core.paths()) {}

// A file renamed with every reference rewritten (graph/rename_transaction), or refused with the
// reasons as findings: an operation (RenameOperation) that joins the validation left due, plans
// (the plan reads the graph: an edit saved before it is planned or refused like any other), stages
// and commits, whose finish reloads the rewritten documents that are open (absorb_rename). A
// rename that would rewrite a file with unsaved edits, or leave them behind on the old name, never
// reaches here with them: it waits on the unsaved prompt first (UnsavedGuard), whose Save writes
// them.
void RenameController::rename_asset(const std::string &file, const std::string &new_name) {
	if (!view_.project.open) return;
	const RenameOperation::Kept kept{view_.documents.active, view_.documents.selection};
	RenameOperation::FilePlanner planner = [this, file, new_name](std::shared_ptr<const AssetScan> &scan) {
		scan = view_.project.scan;
		return plan_rename(paths_, *scan, core_.problems().graph(), file, new_name);
	};
	const uint64_t id = core_.start_operation(std::make_unique<RenameOperation>(core_.problems(), paths_,
			*view_.project.document, core_.problems().graph(), std::move(planner), kept));
	if (id == 0) return core_.refuse_busy(file); // the gate let no operation run beside it
	core_.outcome().operation = id;
	view_.activity.status = "Renaming " + basename_of(file) + " to " + new_name + "...";
	core_.touch(ViewConcern::Output);
}

void RenameController::split_texture(const EditorRequest &request) {
	if (!view_.project.open) return;
	const RenameOperation::Kept kept{view_.documents.active, view_.documents.selection};
	RenameOperation::FilePlanner planner = [this, request](std::shared_ptr<const AssetScan> &scan) {
		scan = view_.project.scan;
		const BaseNames base{&view_.project.base_files};
		return plan_split(paths_, *scan, core_.problems().graph(), request.path, request.new_name, request.paths, &base);
	};
	const uint64_t id = core_.start_operation(std::make_unique<RenameOperation>(core_.problems(), paths_,
			*view_.project.document, core_.problems().graph(), std::move(planner), kept));
	if (id == 0) return core_.refuse_busy(request.path); // the gate let no operation run beside it
	core_.outcome().operation = id;
	view_.activity.status = "Splitting " + basename_of(request.path) + " into " + request.new_name + "...";
	core_.touch(ViewConcern::Output);
}

// A rename's commit read back into the session. A plan refused wrote nothing: its refusals are
// what the rename came to. Else the document the modder was in stays active through the closes and
// reloads (a file's rename: the renamed file's own document follows it to the new name); a
// reloaded document holds new records, so only an untouched one keeps its selection, which is the
// one kept from the start unless the modder selected while the rename ran (the selection's serial
// moved since: theirs stays). What the commit touched is read again: an import source's refresh
// (its outputs made under the new name), else the touched files alone (AssetScan::update).
OperationOutcome RenameController::absorb_rename(RenameOperation &operation) {
	OperationOutcome outcome;
	if (operation.refused()) {
		for (const Diagnostic &d : operation.refusals()) core_.report(d);
		view_.activity.status = "The rename was refused.";
		core_.touch(ViewConcern::Output);
		outcome.end = OperationEnd::Failed;
		outcome.findings = operation.refusals();
		return outcome;
	}
	DocumentSet &documents = core_.documents();
	const RenameTransaction &transaction = *operation.transaction();
	const bool ok = transaction.ok();
	const std::vector<RenameSite> &sites = operation.symbol() ? operation.symbol_plan().sites : operation.file_plan().sites;
	const auto read_files = [&]() {
		if (ProjectRefresh *refresh = operation.refresh()) core_.absorb_refresh(*refresh);
		else core_.update_files(transaction.touched());
	};
	std::vector<std::string> reload;
	for (const RenameSite &site : sites)
		if (documents.document_for(site.file) && std::find(reload.begin(), reload.end(), site.file) == reload.end()) reload.push_back(site.file);
	const RenameOperation::Kept &kept = operation.kept();
	const Selection wanted = view_.documents.selection.serial == kept.selection.serial ? kept.selection
	                                                                                    : view_.documents.selection;
	std::string active = kept.active;
	bool keep_selection = std::find(reload.begin(), reload.end(), active) == reload.end();
	bool files_read = false;
	const bool split = !operation.symbol() && operation.file_plan().split;
	if (!operation.symbol() && ok && !split) {
		const RenamePlan &plan = operation.file_plan();
		// The renamed file and a mission's companions moved with it (a refused commit leaves them
		// where they were, their open documents too): each open document closed (the unsaved prompt
		// saved its edits first), the files read again, each opened again at its new path; the active
		// one stays active there.
		std::vector<std::pair<std::string, std::string>> moved{{plan.path, plan.new_path}};
		for (const RenameOutput &companion : plan.companions) moved.emplace_back(companion.path, companion_path(companion));
		// A card of a file it moved shows it where it went (the MCP gaps lane).
		if (workspace_follows_moves(view_.workspace, moved)) core_.touch(ViewConcern::Workspace);
		// So do the navigation history's places of them.
		core_.navigation().follow_moves(moved);
		std::vector<std::string> reopen;
		std::string active_now;
		for (const auto &[from, to] : moved) {
			DocumentBase *open = documents.document_for(from);
			if (!open) continue;
			const std::string open_path = open->path();
			if (open_path == active) active_now = to;
			documents.close_document(open_path);
			reopen.push_back(to);
		}
		if (!reopen.empty()) {
			read_files();
			files_read = true;
			for (const std::string &to : reopen) documents.open_document(request::open_document(to));
			if (!active_now.empty()) {
				documents.open_document(request::open_document(active_now));
				active = view_.documents.active;
				keep_selection = false;
			}
		}
	}
	if (!files_read) read_files();
	for (const std::string &path : reload) documents.open_document(request::reload_document(path));
	if (operation.symbol()) {
		const SymbolRenamePlan &plan = operation.symbol_plan();
		if (!active.empty() && documents.document_for(active)) {
			documents.activate(active);
			if (keep_selection) view_.documents.selection.restore(wanted);
			else view_.documents.selection.select_only(active, NodeAddress());
			// The renamed definition selected again where it was, its field shown.
			if (!keep_selection && active == plan.file)
				if (Document *defining = documents.records_for(active))
					view_.documents.selection.select_only(active, defining->address_at(plan.locator));
			documents.select_first_screen();
		}
	} else {
		bool active_open = active.empty();
		for (const auto &document : documents.documents()) active_open = active_open || document->path() == active;
		if (active_open) {
			documents.activate(active);
			if (keep_selection) view_.documents.selection.restore(wanted);
			else view_.documents.selection.select_only(active, NodeAddress());
			documents.select_first_screen();
		}
	}
	// Reported last: the scan's update and the reloads above leave the rows' validation due.
	for (const Diagnostic &d : transaction.findings()) core_.report(d);
	// Undo does not take a rename back (it rewrote files): the way back is said, and the Edit menu offers
	// the rename back (the UX round's problems lane).
	const auto way_back = [](const std::string &from, const std::string &to) {
		return " Undo does not take it back: Edit > Rename " + to + " back to " + from + " does.";
	};
	const bool back = backing_;
	backing_ = false;
	const std::string renamed = back ? " back to " : " to ";
	if (ok && operation.symbol()) {
		const SymbolRenamePlan &plan = operation.symbol_plan();
		const size_t uses = plan.sites.empty() ? 0 : plan.sites.size() - 1;
		core_.note("Renamed " + std::string(reference_row(plan.kind).phrase) + " " + plan.old_name + renamed + plan.new_name +
		           " (" + std::to_string(uses) + " use" + (uses == 1 ? "" : "s") + " rewritten)." +
		           way_back(plan.old_name, plan.new_name));
		view_.activity.status = "Renamed " + plan.old_name + renamed + plan.new_name + (back ? "." : " everywhere.");
		view_.activity.last_rename = {true, true, plan.file, plan.locator, plan.field, plan.old_name, plan.new_name, plan.kind,
		                              plan.scope};
	} else if (ok && split) {
		// A split: the copy made and the uses moved to it; no way back but a rename of the copy.
		const RenamePlan &plan = operation.file_plan();
		std::set<std::string> files;
		for (const RenameSite &site : plan.sites) files.insert(site.file);
		std::string named;
		for (const std::string &file : files) named += (named.empty() ? "" : ", ") + basename_of(file);
		core_.note("Split " + plan.old_name + ": " + plan.new_name + " is a copy of it, which the " +
		           counted(plan.sites.size(), "use") + " in " + named + " now name" +
		           (plan.split_source.empty() ? std::string() : " (made by " + plan.split_source + ", a copy of its source)") +
		           ". Undo does not take it back.");
		view_.activity.status = "Split " + plan.old_name + " into " + plan.new_name + ".";
	} else if (ok) {
		const RenamePlan &plan = operation.file_plan();
		std::string companions;
		for (const RenameOutput &companion : plan.companions)
			companions += (companions.empty() ? ", with " : ", ") + companion.old_name + " to " + companion.new_name;
		core_.note("Renamed " + plan.old_name + renamed + plan.new_name + " (" + std::to_string(plan.sites.size()) +
		           " reference" + (plan.sites.size() == 1 ? "" : "s") + " rewritten" + companions + ")." +
		           way_back(plan.old_name, plan.new_name));
		view_.activity.status = "Renamed " + plan.old_name + renamed + plan.new_name + ".";
		view_.activity.last_rename = {true, false, plan.new_path, std::string(), std::string(), plan.old_name, plan.new_name,
		                              ReferenceKind::None, std::string()};
	} else {
		view_.activity.status = "The rename did not finish.";
	}
	// What it did, for its way back: the sites as it left them (a text's later sites on a line moved by the
	// names before them), each file it wrote by its bytes' hash now. A split has none.
	if (ok && !split) {
		Done done;
		done.symbol = operation.symbol();
		if (done.symbol) {
			const SymbolRenamePlan &plan = operation.symbol_plan();
			done.kind = plan.kind;
			done.file = plan.file;
			done.field = plan.field;
			done.scope = plan.scope;
			done.from = plan.old_name;
			done.to = plan.new_name;
		} else {
			const RenamePlan &plan = operation.file_plan();
			done.file = plan.new_path;
			done.from = plan.old_name;
			done.to = plan.new_name;
		}
		done.sites = sites;
		for (RenameSite &site : done.sites) {
			if (!site.span.line) continue;
			ptrdiff_t moved = 0;
			for (const RenameSite &earlier : sites)
				if (earlier.file == site.file && earlier.span.line == site.span.line && earlier.span.column < site.span.column)
					moved += ptrdiff_t(earlier.after.size()) - ptrdiff_t(earlier.span.length);
			site.span.column = size_t(ptrdiff_t(site.span.column) + moved);
			site.span.length = site.after.size();
		}
		for (const RenameSite &site : done.sites) done.written[site.file] = hash_of(site.file);
		done_ = std::move(done);
	}
	core_.touch(ViewConcern::Selection); // the active document and its selection, kept or started over
	core_.touch(ViewConcern::Output);
	outcome.end = ok ? OperationEnd::Done : OperationEnd::Failed;
	outcome.findings = transaction.findings();
	return outcome;
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
	if (!renamed || renamed->inert || !asset || !extract_from_asset(paths_, *view_.project.document, *asset, saved, error)) return false;
	for (const GraphEdge &edge : saved.edges)
		if (edge.kind == plan.kind && graph.resolve_symbol(edge.kind, edge.value, edge.scope) == renamed) return true;
	return false;
}

// The plan over the graph as the validation last left it: the rename's own plan is made once the
// validation it joins has ended; a preview's and the unsaved guard's read the graph as it stands.
SymbolRenamePlan RenameController::plan_symbol(const EditorRequest &request) {
	const AssetGraph &graph = core_.problems().graph();
	// The file as every other request names it: its project-relative path, or its name alone (the
	// graph keys a file's symbols by its path).
	const AssetEntry *file = view_.project.open ? core_.project_file(request.path) : nullptr;
	const std::string path = file ? file->relative_path : request.path;
	const GraphSymbol *symbol = graph.symbol_at(path, request.locator, request.field);
	if (!symbol) {
		SymbolRenamePlan none;
		none.file = path;
		none.new_name = request.new_name;
		none.refusals.push_back(make_finding(CoreFinding::RenameUnknownSymbol, DiagnosticSeverity::Error,
		                                     path + " defines no name in field " + request.field +
		                                             " of the record at " + request.locator + ".",
		                                     path, request.field));
		return none;
	}
	return plan_symbol_rename_project(*view_.project.scan, graph, *symbol, request.new_name);
}

// What a rename would do, planned into the view (nothing written, nothing reported): a name's
// rename everywhere, or a file's (no field), over the graph as the validation last left it (the
// rename itself plans again once the validation it joins has ended). One that asks the new name
// (ask_name) posts an AskRename event naming the preview by its serial: the Rename everywhere
// dialog opens on it.
void RenameController::preview(const EditorRequest &request) {
	if (!view_.project.open) return;
	DialogsView::RenamePreview preview;
	preview.serial = view_.dialogs.rename_preview.serial + 1;
	preview.requested = request.new_name;
	if (request.field.empty()) {
		const RenamePlan plan =
		        plan_rename(paths_, *view_.project.scan, core_.problems().graph(), request.path, request.new_name);
		preview.path = plan.path.empty() ? request.path : plan.path;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.sites = std::make_shared<const std::vector<RenameSite>>(plan.sites);
		for (const RenameOutput &companion : plan.companions) preview.companions.push_back(companion.old_name + " to " + companion.new_name);
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
			check_symbol_rename(paths_, *view_.project.document, *view_.project.scan, core_.problems().graph(), plan, open, plan.refusals);
		}
		preview.symbol = true;
		preview.kind = plan.kind;
		preview.path = plan.file;
		preview.locator = request.locator;
		preview.field = request.field;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.sites = std::make_shared<const std::vector<RenameSite>>(plan.sites);
		preview.refusals = plan.refusals;
	}
	view_.dialogs.rename_preview = std::move(preview);
	if (request.ask_name) {
		const DialogsView::RenamePreview &made = view_.dialogs.rename_preview;
		ViewEvent ask;
		ask.kind = ViewEventKind::AskRename;
		ask.path = made.path;
		ask.field = made.field;
		ask.tag = made.serial;
		view_.events.post(std::move(ask));
	}
	core_.touch(ViewConcern::Dialogs);
}

// A name renamed everywhere (graph/rename_transaction), or refused with the reasons as
// findings: an operation (RenameOperation) that joins the validation left due, plans, stages and
// commits, whose finish reloads the rewritten documents that are open (absorb_rename). One that
// would rewrite a file with unsaved edits never reaches here with them: it waits on the unsaved
// prompt first (UnsavedGuard), whose Save writes them.
void RenameController::rename_symbol(const EditorRequest &request) {
	if (!view_.project.open) return;
	const RenameOperation::Kept kept{view_.documents.active, view_.documents.selection};
	RenameOperation::SymbolPlanner planner = [this, request](std::shared_ptr<const AssetScan> &scan) {
		scan = view_.project.scan;
		return plan_symbol(request);
	};
	const uint64_t id = core_.start_operation(std::make_unique<RenameOperation>(core_.problems(), paths_,
			*view_.project.document, core_.problems().graph(), std::move(planner), kept));
	if (id == 0) return core_.refuse_busy(request.path); // the gate let no operation run beside it
	core_.outcome().operation = id;
	view_.activity.status = "Renaming in " + basename_of(request.path) + " everywhere...";
	core_.touch(ViewConcern::Output);
}

// A requirement satisfied by renaming a project file of the expected kind to the name
// the engine demands. A role or a file the request names that does not fit is the request's own fault:
// refused, so whoever asked learns the assignment did not happen, and no Problems row.
void RenameController::assign_requirement(const std::string &role, const std::string &file) {
	if (!view_.project.open) return;
	const RequirementRow *row = core_.requirement_row(role);
	if (!row) {
		core_.refuse_request(CoreFinding::RequirementUnknown, "No requirement has the role '" + role + "'.", std::string(),
		                     DiagnosticSeverity::Error);
		return;
	}
	if (row->state == RequirementState::Present) {
		core_.refuse_request(CoreFinding::RequirementAssigned, row->name + " is already in the project: nothing was assigned.",
		                     row->asset_path, DiagnosticSeverity::Error);
		return;
	}
	const AssetEntry *asset = core_.project_file(file);
	if (!asset) {
		core_.refuse_request(CoreFinding::RequirementUnknownFile, "The project has no file named '" + file + "'.", file,
		                     DiagnosticSeverity::Error);
		return;
	}
	if (asset->kind != row->expected_kind) {
		core_.refuse_request(CoreFinding::RequirementKind,
		                     asset->logical_name + " is " + asset_kind_label(asset->kind) + ", and " + row->name +
		                             " must be " + asset_kind_label(row->expected_kind) + ".",
		                     asset->relative_path, DiagnosticSeverity::Error);
		return;
	}
	rename_asset(asset->relative_path, row->name);
}

void RenameController::unsaved_files(const EditorRequest &request, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	switch (request.kind) {
	case EditorRequestKind::RenameBack: {
		// The documents with unsaved edits among the files the way back rewrites (saved, a file it wrote is
		// no longer as it left it: the plan then says so); none when there is no way back.
		if (!view_.project.open || !done_ || !documents.documents_dirty()) return;
		if (unsaved_while_due(files)) return;
		std::vector<std::string> rewrites;
		if (done_->symbol)
			for (const RenameSite &site : plan_back_symbol().sites) rewrites.push_back(site.file);
		else {
			const RenamePlan plan = plan_back_file();
			rewrites.push_back(plan.path);
			for (const RenameSite &site : plan.sites) rewrites.push_back(site.file);
		}
		for (const auto &document : documents.documents())
			if (document->dirty() && std::find(rewrites.begin(), rewrites.end(), document->path()) != rewrites.end())
				files.push_back(document->path());
		return;
	}
	case EditorRequestKind::RenameAsset: rename_unsaved(request.path, request.new_name, files); return;
	case EditorRequestKind::SplitTexture: {
		// The documents with unsaved edits among the files the split rewrites.
		if (!view_.project.open || !documents.documents_dirty()) return;
		if (unsaved_while_due(files)) return;
		const BaseNames base{&view_.project.base_files};
		const RenamePlan plan = plan_split(paths_, *view_.project.scan, core_.problems().graph(), request.path, request.new_name,
		                                   request.paths, &base);
		if (!plan.ok()) return;
		for (const auto &document : documents.documents())
			if (document->dirty() && std::any_of(plan.sites.begin(), plan.sites.end(),
			                                     [&](const RenameSite &site) { return site.file == document->path(); }))
				files.push_back(document->path());
		return;
	}
	case EditorRequestKind::RenameSymbol:
		// The documents with unsaved edits among the files the plan rewrites, and those whose
		// saved file names the name (the commit reads the files on disk: an unsaved edit that
		// stopped naming it would leave the saved use behind); none when the plan is refused
		// anyway (the rename then says why, with nothing to save first). Saved, the rename is
		// planned again. While a validation is due or under way every one is (unsaved_while_due).
		if (view_.project.open && documents.documents_dirty()) {
			if (unsaved_while_due(files)) return;
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
		const RequirementRow *row = core_.requirement_row(request.role);
		const AssetEntry *asset = core_.project_file(request.path);
		if (row && asset && row->state != RequirementState::Present && asset->kind == row->expected_kind)
			rename_unsaved(asset->relative_path, row->name, files);
		return;
	}
	default: return;
	}
}

// While a validation is due or under way (S13 A3: the polls step it, and no request runs it), the
// graph may not hold the edits the documents have not been validated with, so the plan cannot say
// which of them a rename rewrites: every document with unsaved edits is listed (the prompt saves
// them all, and the rename plans again once the validation it joins has ended). True then.
bool RenameController::unsaved_while_due(std::vector<std::string> &files) {
	if (!core_.problems().validating()) return false;
	for (const auto &document : core_.documents().documents())
		if (document->dirty()) files.push_back(document->path());
	return true;
}

// The documents with unsaved edits a rename of `file` to `new_name` would rewrite (its
// plan's sites) or leave behind on the old name (the file's own, a mission's companions moved
// with it), in the order they were opened; none when the plan is refused anyway (the rename then
// says why, with nothing to save first). The plan reads the graph, which holds every edit when no
// validation is due (else unsaved_while_due).
void RenameController::rename_unsaved(const std::string &file, const std::string &new_name, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	if (!view_.project.open || !documents.documents_dirty()) return;
	if (unsaved_while_due(files)) return;
	const RenamePlan plan = plan_rename(paths_, *view_.project.scan, core_.problems().graph(), file, new_name);
	if (!plan.ok()) return;
	for (const auto &document : documents.documents()) {
		if (!document->dirty()) continue;
		const std::string &path = document->path();
		if (path == plan.path ||
		    std::any_of(plan.sites.begin(), plan.sites.end(), [&path](const RenameSite &site) { return site.file == path; }) ||
		    std::any_of(plan.companions.begin(), plan.companions.end(),
		                [&path](const RenameOutput &companion) { return companion.path == path; }))
			files.push_back(path);
	}
}

uint64_t RenameController::hash_of(const std::string &file) const {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!read_file_bytes(join_path(paths_.root, file), bytes, error)) return 0;
	return io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
}

void RenameController::keep_written(std::vector<RenameSite> &sites, std::vector<Diagnostic> &refusals) const {
	const Done &done = *done_;
	// A file the rename wrote, changed since: which of its uses it wrote can no longer be told from the others.
	for (const auto &[file, hash] : done.written)
		if (hash_of(file) != hash)
			refusals.push_back(make_finding(CoreFinding::RenameSite, DiagnosticSeverity::Error,
			                                file + " changed since the rename: which of its uses of " + done.to +
			                                        " the rename wrote can no longer be told. Rename it everywhere instead, "
			                                        "or put the file back as the rename left it.",
			                                file));
	// A native text's sites have no place in the file but their record (graph/native_text_sites.h), which
	// tells them apart; any other site's record may be what was renamed (a screen's NAME).
	const auto same = [](const RenameSite &a, const RenameSite &b) {
		const bool placed = !a.locator.empty() || a.span.line;
		return a.file == b.file && (placed || a.record == b.record) && a.locator == b.locator && a.field == b.field &&
		       a.span.line == b.span.line &&
		       (!a.span.line || a.span.column == b.span.column);
	};
	std::vector<RenameSite> kept;
	for (const RenameSite &site : sites)
		if (std::any_of(done.sites.begin(), done.sites.end(), [&](const RenameSite &wrote) { return same(wrote, site); }))
			kept.push_back(site);
	// Every site it wrote is one to take back: one the plan no longer has no longer names it.
	for (const RenameSite &wrote : done.sites)
		if (done.written.count(wrote.file) && hash_of(wrote.file) == done.written.at(wrote.file) &&
		    std::none_of(kept.begin(), kept.end(), [&](const RenameSite &site) { return same(wrote, site); }))
			refusals.push_back(make_finding(CoreFinding::RenameSite, DiagnosticSeverity::Error,
			                                wrote.file + (wrote.record.empty() ? std::string() : ": " + wrote.record) +
			                                        " no longer names " + done.to + " where the rename wrote it.",
			                                wrote.file, wrote.field));
	sites = std::move(kept);
}

SymbolRenamePlan RenameController::plan_back_symbol() {
	const Done &done = *done_;
	const AssetGraph &graph = core_.problems().graph();
	// The definition the rename gave the new name, by itself: its kind, its name, the file and field defining it.
	const GraphSymbol *defined = nullptr;
	for (const GraphSymbol *symbol : graph.symbols_named(done.kind, done.to, done.scope))
		if (symbol->file == done.file && symbol->field == done.field) {
			defined = symbol;
			break;
		}
	if (!defined) {
		SymbolRenamePlan none;
		none.kind = done.kind;
		none.file = done.file;
		none.field = done.field;
		none.old_name = done.to;
		none.new_name = done.from;
		none.refusals.push_back(make_finding(CoreFinding::RenameUnknownSymbol, DiagnosticSeverity::Error,
		                                     done.file + " no longer defines " + done.to + ": there is nothing to rename back.",
		                                     done.file, done.field));
		return none;
	}
	SymbolRenamePlan plan = plan_symbol_rename_project(*view_.project.scan, graph, *defined, done.from);
	keep_written(plan.sites, plan.refusals);
	return plan;
}

RenamePlan RenameController::plan_back_file() {
	const Done &done = *done_;
	if (!view_.project.scan->at_path(done.file)) {
		RenamePlan none;
		none.path = done.file;
		none.old_name = done.to;
		none.new_name = done.from;
		none.refusals.push_back(make_finding(CoreFinding::RenameUnknownFile, DiagnosticSeverity::Error,
		                                     "The project no longer has " + done.file + ": there is nothing to rename back.",
		                                     done.file));
		return none;
	}
	RenamePlan plan = plan_rename(paths_, *view_.project.scan, core_.problems().graph(), done.file, done.from);
	keep_written(plan.sites, plan.refusals);
	return plan;
}

void RenameController::preview_back(const EditorRequest &request) {
	if (!view_.project.open) return;
	DialogsView::RenamePreview preview;
	preview.serial = view_.dialogs.rename_preview.serial + 1;
	preview.back = true;
	if (!done_) {
		preview.refusals.push_back(make_finding(CoreFinding::RenameUnknownSymbol, DiagnosticSeverity::Error,
		                                        "No rename has finished in this project to take back."));
		preview.sites = std::make_shared<const std::vector<RenameSite>>();
	} else if (done_->symbol) {
		const SymbolRenamePlan plan = plan_back_symbol();
		preview.symbol = true;
		preview.kind = plan.kind;
		preview.path = plan.file;
		preview.locator = plan.locator;
		preview.field = plan.field;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.requested = plan.new_name;
		preview.sites = std::make_shared<const std::vector<RenameSite>>(plan.sites);
		preview.refusals = plan.refusals;
	} else {
		const RenamePlan plan = plan_back_file();
		preview.path = plan.path;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.requested = plan.new_name;
		preview.sites = std::make_shared<const std::vector<RenameSite>>(plan.sites);
		for (const RenameOutput &companion : plan.companions) preview.companions.push_back(companion.old_name + " to " + companion.new_name);
		preview.refusals = plan.refusals;
	}
	view_.dialogs.rename_preview = std::move(preview);
	if (request.ask_name) {
		ViewEvent ask;
		ask.kind = ViewEventKind::AskRename;
		ask.path = view_.dialogs.rename_preview.path;
		ask.field = view_.dialogs.rename_preview.field;
		ask.tag = view_.dialogs.rename_preview.serial;
		view_.events.post(std::move(ask));
	}
	core_.touch(ViewConcern::Dialogs);
}

void RenameController::rename_back() {
	if (!view_.project.open) return;
	if (!done_) {
		core_.report(make_finding(CoreFinding::RenameUnknownSymbol, DiagnosticSeverity::Error,
		                          "No rename has finished in this project to take back."));
		return;
	}
	const RenameOperation::Kept kept{view_.documents.active, view_.documents.selection};
	uint64_t id = 0;
	if (done_->symbol) {
		RenameOperation::SymbolPlanner planner = [this](std::shared_ptr<const AssetScan> &scan) {
			scan = view_.project.scan;
			return plan_back_symbol();
		};
		id = core_.start_operation(std::make_unique<RenameOperation>(core_.problems(), paths_, *view_.project.document,
		                                                             core_.problems().graph(), std::move(planner), kept));
	} else {
		RenameOperation::FilePlanner planner = [this](std::shared_ptr<const AssetScan> &scan) {
			scan = view_.project.scan;
			return plan_back_file();
		};
		id = core_.start_operation(std::make_unique<RenameOperation>(core_.problems(), paths_, *view_.project.document,
		                                                             core_.problems().graph(), std::move(planner), kept));
	}
	if (id == 0) return core_.refuse_busy(done_->file); // the gate let no operation run beside it
	backing_ = true;
	core_.outcome().operation = id;
	view_.activity.status = "Renaming " + done_->to + " back to " + done_->from + "...";
	core_.touch(ViewConcern::Output);
}

bool rename_back_offered(const SessionView &view) {
	const ActivityView::LastRename &last = view.activity.last_rename;
	if (!last.made || !view.project.open) return false;
	if (!last.symbol) return view.project.scan && view.project.scan->at_path(last.path);
	if (!view.findings.graph) return false;
	for (const GraphSymbol *symbol : view.findings.graph->symbols_named(last.kind, last.to, last.scope))
		if (symbol->file == last.path && symbol->field == last.field) return true;
	return false;
}

} // namespace opennova::editor
