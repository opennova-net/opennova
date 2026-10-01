#include <editor/session/import_controller.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/model/diagnostic.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/import_operation.h>
#include <editor/session/import_plan_operation.h>
#include <editor/session/problems_service.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

ImportController::ImportController(SessionCore &core) : core_(core), view_(core.view()), paths_(core.paths()) {}

void ImportController::preview_files(const EditorRequest &request) {
	if (!view_.project.open) return;
	std::vector<Diagnostic> diagnostics;
	std::vector<ImportSource> choices, roots;
	// A loose file picked is chosen; an archive's members are listed to choose from.
	for (ImportSource &source : list_import_sources(request.paths, diagnostics))
		(source.entry.empty() ? roots : choices).push_back(std::move(source));
	for (const auto &d : diagnostics) core_.report(d);
	preview(std::move(choices), std::move(roots), request.with_dependencies);
}

void ImportController::plan(const EditorRequest &request) {
	if (!view_.project.open) return;
	preview(view_.dialogs.import_preview.open ? view_.dialogs.import_preview.choices : std::vector<ImportSource>(), request.imports,
	        request.with_dependencies);
}

void ImportController::preview_install(const EditorRequest &request) {
	if (!view_.project.open) return;
	std::vector<Diagnostic> diagnostics;
	std::vector<ImportSource> sources = list_retail_import_sources(core_.game_install(), *view_.project.document, diagnostics);
	// With names (an Import fix): those files alone, chosen; a name the game data does
	// not have is a finding (unless the install itself is the finding). Without, every
	// file is listed to choose from.
	std::vector<ImportSource> named;
	for (const std::string &name : request.names) {
		const auto found = std::find_if(sources.begin(), sources.end(), [&name](const ImportSource &source) {
			return normalized_logical_name(source.entry) == normalized_logical_name(name);
		});
		if (found == sources.end()) {
			if (diagnostics.empty())
				diagnostics.push_back(make_finding(CoreFinding::ImportSource, DiagnosticSeverity::Error,
				                                   "The game data has no file named " + name + "."));
			continue;
		}
		named.push_back(*found);
	}
	if (!request.names.empty()) sources.clear();
	for (const auto &d : diagnostics) core_.report(d);
	preview(std::move(sources), std::move(named), request.with_dependencies);
}

void ImportController::cancel() {
	view_.dialogs.import_preview = DialogsView::ImportPreview();
	core_.touch(ViewConcern::Dialogs);
}

// The refresh with the import pass forced over one source (or all), as an operation
// (RefreshOperation): every stale source imports too, as on any refresh; `force` imports the named
// one even when unchanged (a changed importer, a wanted rebuild). The pass's findings on the
// sources asked for are what the operation came to.
void ImportController::reimport(const std::string &source, bool force) {
	if (!view_.project.open) return;
	if (core_.start_refresh(true, force, source)) view_.activity.status = "Importing again...";
	core_.touch(ViewConcern::Output);
}

// The import dialog on `roots` chosen among `choices` (each file once), planned with the
// files they need when `with_dependencies`: open while it has something to show, a list to
// choose from or a file chosen.
void ImportController::preview(std::vector<ImportSource> choices, std::vector<ImportSource> roots,
                               bool with_dependencies) {
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	preview.choices = std::move(choices);
	preview.roots.clear();
	for (ImportSource &root : roots)
		if (std::find(preview.roots.begin(), preview.roots.end(), root) == preview.roots.end())
			preview.roots.push_back(std::move(root));
	preview.with_dependencies = with_dependencies;
	preview.open = !preview.choices.empty() || !preview.roots.empty();
	if (preview.open) start_plan();
	else show_plan(std::make_shared<const ImportPlan>(), nullptr);
}

// The open preview's plan, made from its roots as the files are now, as an operation
// (ImportPlanOperation: the project read again, a copy of the asset graph brought up to it, the
// game install mounted, the plan); its findings are the dialog's to show. Every request that plans
// it validates first (its row's `validates`), so an edit a held pump made reaches the graph it
// copies. The dialog shows its files at once, and no plan until the operation's is made.
void ImportController::start_plan() {
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	preview.plan = std::make_shared<const ImportPlan>();
	preview.changed = false;
	const uint64_t id = core_.start_operation(std::make_unique<ImportPlanOperation>(paths_, *view_.project.document,
			core_.problems().graph(), view_.documents.open, preview.roots, preview.with_dependencies, core_.game_install()));
	if (id == 0) return core_.refuse_busy(std::string()); // the gate let no operation run beside it
	core_.outcome().operation = id;
	view_.activity.status = "Planning the import...";
	core_.touch(ViewConcern::Dialogs);
	core_.touch(ViewConcern::Output);
}

OperationOutcome ImportController::absorb_plan(ImportPlanOperation &operation) {
	show_plan(std::make_shared<const ImportPlan>(std::move(operation.plan())), nullptr);
	return OperationOutcome();
}

// A plan made for the dialog. Each posts an ImportPlanned event, on which the dialog takes the
// plan's checks again. `shown`: the plan an Import was shown, planned again before it writes; the
// preview says it changed (`changed`, the event's flag) when the new plan is not that one.
void ImportController::show_plan(std::shared_ptr<const ImportPlan> plan, const ImportPlan *shown) {
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	preview.plan = std::move(plan);
	preview.changed = shown && !same_import(*shown, *preview.plan);
	ViewEvent planned;
	planned.kind = ViewEventKind::ImportPlanned;
	planned.flag = preview.changed;
	view_.events.post(std::move(planned));
	if (preview.open) {
		size_t files = 0, found = 0, missing = 0;
		for (const ImportPlanRow &row : preview.plan->rows) {
			if (row.state == ImportPlanRow::State::NotFound) ++missing;
			else if (row.selected) ++files;
			if (row.state == ImportPlanRow::State::Found) ++found;
		}
		view_.activity.status = preview.roots.empty() ? std::string("Choose the files to import.")
		               : "Import preview: " + std::to_string(files) + " file" + (files == 1 ? "" : "s") + " to import" +
		                         (preview.with_dependencies ? " (" + std::to_string(found) + " the chosen ones need), " +
		                                                              std::to_string(missing) + " not found."
		                                                    : std::string("."));
	}
	core_.touch(ViewConcern::Dialogs);
	if (preview.open) core_.touch(ViewConcern::Output);
}

// The import dialog's "Include the files these need": the editor's preference, written from a
// copy (a preference that could not be written stays the one in effect, its failure a finding);
// an open preview is planned again with the setting asked for.
void ImportController::set_dependencies(bool with_dependencies) {
	EditorPreferences &preferences = core_.preferences();
	if (with_dependencies != preferences.values().import_dependencies) {
		Preferences editor = preferences.values();
		editor.import_dependencies = with_dependencies;
		Diagnostic error;
		if (!preferences.write(editor, error)) core_.report(error);
	}
	view_.project.import_dependencies = preferences.values().import_dependencies;
	view_.activity.status = with_dependencies ? "Imports bring the files the chosen ones need."
	                                          : "Imports take the chosen files alone.";
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	if (preview.open && preview.with_dependencies != with_dependencies) {
		preview.with_dependencies = with_dependencies;
		start_plan();
	}
	core_.touch(ViewConcern::Preferences);
	core_.touch(ViewConcern::Output);
}

// The rows kept, written the whole selection or none of it as far as the disk allows
// (import_assets), then one refresh, as an operation (ImportOperation). With a preview open the
// files are planned again first: when that is not the import the preview showed (a dependency new
// or gone, a file found in another place, a file that no longer reads), nothing is written and
// the dialog shows the new plan with a line saying so; a row the plan does not have is refused.
// An import that would write over a file with unsaved edits never reaches here: it waits on the
// unsaved prompt first (UnsavedGuard), whose Save writes them; the refresh after it reads again
// the open documents whose files it replaced.
void ImportController::import_files(const EditorRequest &request) {
	if (!view_.project.open) return;
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	std::unique_ptr<ImportPlanOperation> replan;
	std::shared_ptr<const ImportPlan> shown;
	if (preview.open) {
		shown = preview.plan;
		replan = std::make_unique<ImportPlanOperation>(paths_, *view_.project.document, core_.problems().graph(),
				view_.documents.open, preview.roots, preview.with_dependencies, core_.game_install());
	} else if (request.imports.empty()) {
		view_.activity.status = "Nothing to import.";
		core_.touch(ViewConcern::Output);
		return;
	}
	const uint64_t id = core_.start_operation(std::make_unique<ImportOperation>(paths_, *view_.project.document,
			request.imports, request.replace, std::move(replan), std::move(shown)));
	if (id == 0) return core_.refuse_busy(std::string()); // the gate let no operation run beside it
	core_.outcome().operation = id;
	view_.activity.status = "Importing...";
	core_.touch(ViewConcern::Output);
}

OperationOutcome ImportController::absorb_import(ImportOperation &operation) {
	OperationOutcome outcome;
	const auto refused = [&](const Diagnostic &d) {
		core_.report(d);
		outcome.end = OperationEnd::Failed;
		outcome.findings.push_back(d);
		return outcome;
	};
	if (operation.replanned()) {
		show_plan(operation.new_plan(), operation.shown().get());
		if (operation.changed()) {
			view_.activity.status = "The files changed since the preview: nothing was imported.";
			core_.touch(ViewConcern::Dialogs);
			core_.touch(ViewConcern::Output);
			return refused(make_finding(CoreFinding::ImportChanged, DiagnosticSeverity::Warning, "The files changed since the preview: nothing was imported. Check the import again."));
		}
		if (!operation.refusals().empty()) return refused(operation.refusals().front());
	}
	view_.dialogs.import_preview = DialogsView::ImportPreview();
	if (operation.imports().empty()) {
		view_.activity.status = "Nothing to import.";
		core_.touch(ViewConcern::Dialogs);
		core_.touch(ViewConcern::Output);
		return outcome;
	}
	const ImportResult &imported = operation.result();
	// What it wrote and what it did not reach are what the import came to (S13 A7's lists, the
	// operation's since the write is one, S13 A3), besides Output.
	outcome.imported = imported.imported;
	outcome.not_imported = imported.not_imported;
	if (operation.refreshed()) {
		// A Rescan: the open documents whose files it replaced read again, then the refresh.
		core_.documents().reload_changed();
		core_.absorb_refresh(operation.refresh());
	}
	for (const auto &path : imported.imported) core_.note("Imported " + path);
	for (const auto &path : imported.not_imported) core_.note("Not imported " + path);
	// What the import reported is what it came to: an error failed it (refused before anything was
	// written, or stopped part way, its not_imported files said).
	for (const auto &d : imported.diagnostics) {
		core_.report(d);
		outcome.findings.push_back(d);
		if (d.severity == DiagnosticSeverity::Error) outcome.end = OperationEnd::Failed;
	}
	const size_t done = imported.imported.size();
	if (!imported.not_imported.empty()) outcome.end = OperationEnd::Failed;
	view_.activity.status = !imported.not_imported.empty()
	                       ? std::to_string(done) + " of " + std::to_string(done + imported.not_imported.size()) +
	                                 " files imported: the import stopped at " + imported.not_imported.front() + "."
	                       : std::to_string(done) + " file(s) imported.";
	core_.touch(ViewConcern::Dialogs); // the preview closed
	core_.touch(ViewConcern::Output);
	return outcome;
}

void ImportController::refresh_install_files() {
	view_.project.retail_files = view_.project.open ? list_retail_file_names(core_.game_install(), *view_.project.document)
	                                        : std::vector<std::string>();
	core_.touch(ViewConcern::Files);
}

void ImportController::set_install_files(std::vector<std::string> names) {
	view_.project.retail_files = std::move(names);
	core_.touch(ViewConcern::Files);
}

// An import writes over a project file only when it replaces one (else a file of the name is
// refused, or kept when it holds the same bytes): the files its sources make land where the
// plan puts them. With the import dialog open that is the plan it shows (the ImportPlan
// operation's, S13 A3), reused: the import plans again before it writes and writes nothing when
// the plan is not that one. With none open, the files asked for are planned here, over the view's
// scan and graph, with no cap: import_assets writes every file of the request, so every
// destination is looked at.
void ImportController::unsaved_files(const EditorRequest &request, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	if (!view_.project.open || !request.replace || !documents.documents_dirty()) return;
	const DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	std::shared_ptr<const ImportPlan> plan = preview.open ? preview.plan : nullptr;
	if (!plan) {
		core_.problems().validate_pending();
		plan = std::make_shared<const ImportPlan>(plan_import(request.imports, false, paths_, *view_.project.document,
				*view_.project.scan, core_.problems().graph(), core_.game_install(), SIZE_MAX));
	}
	// A row the request asks for, which the plan finds: where its file lands.
	const auto writes = [&request](const ImportPlanRow &row) {
		return row.state != ImportPlanRow::State::NotFound &&
		       std::find(request.imports.begin(), request.imports.end(), row.source) != request.imports.end();
	};
	for (const auto &document : documents.documents()) {
		if (!document->dirty()) continue;
		if (std::any_of(plan->rows.begin(), plan->rows.end(), [&](const ImportPlanRow &row) {
			    return writes(row) && row.destination == document->path();
		    }))
			files.push_back(document->path());
	}
}

void ImportController::clear() {
	view_.dialogs.import_preview = DialogsView::ImportPreview();
	view_.project.imports = std::make_shared<const std::vector<ImportedSource>>();
	view_.project.retail_files.clear();
}

} // namespace opennova::editor
