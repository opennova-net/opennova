#include <editor/session/import_controller.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_preferences.h>
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
				diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.source",
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

// The refresh with the import pass forced over one source (or all): every stale source
// imports too, as on any refresh; `force` imports the named one even when unchanged (a
// changed importer, a wanted rebuild).
void ImportController::reimport(const std::string &source, bool force) {
	if (!view_.project.open) return;
	const ImportRunResult imported = core_.refresh(force, source);
	// The pass's findings are Problems rows already (they ride the scan); the ones on the
	// sources asked for are also this request's outcome.
	std::vector<std::string> asked;
	for (const ImportedSource &ran : imported.sources)
		if (import_source_named(source, ran.source)) {
			asked.push_back(ran.source);
			asked.push_back(ran.sidecar);
		}
	for (const Diagnostic &d : imported.diagnostics)
		if (std::find(asked.begin(), asked.end(), d.asset) != asked.end()) core_.record_outcome(d);
	view_.activity.status = std::to_string(imported.reimported) + " source" + (imported.reimported == 1 ? "" : "s") + " imported.";
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
	plan_preview();
}

// The open preview's plan, made from its roots as the files are now: the project read again
// (the view's scan may be older than a change made outside the editor; a scan writes
// nothing, unlike a refresh, which runs the import pass), resolved by a copy of the asset
// graph brought up to it (its cache reads again only the files that changed); its findings
// are the dialog's to show. Every request that plans it validates first (its row's
// `validates`), so an edit a held pump made reaches the graph it copies. Each plan made posts
// an ImportPlanned event, on which the dialog takes the plan's checks again. `shown`: the plan
// an Import was shown, planned again before it writes; the preview says it changed (`changed`,
// the event's flag) when the new plan is not that one.
void ImportController::plan_preview(const ImportPlan *shown) {
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	if (preview.open) {
		const AssetScan scan = scan_project_assets(paths_, *view_.project.document);
		AssetGraph graph = core_.problems().graph();
		graph.update(paths_, *view_.project.document, scan, view_.documents.open);
		preview.plan = std::make_shared<const ImportPlan>(plan_import(preview.roots, preview.with_dependencies,
				paths_, *view_.project.document, scan, graph, core_.game_install()));
	} else {
		preview.plan = std::make_shared<const ImportPlan>();
	}
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
		plan_preview();
	}
	core_.touch(ViewConcern::Preferences);
	core_.touch(ViewConcern::Output);
}

// The rows kept, written the whole selection or none of it as far as the disk allows
// (import_assets), then one refresh. With a preview open the files are planned again first:
// when that is not the import the preview showed (a dependency new or gone, a file found in
// another place, a file that no longer reads), nothing is written and the dialog shows the
// new plan with a line saying so; a row the plan does not have is refused. An import that
// would write over a file with unsaved edits never reaches here: it waits on the unsaved
// prompt first (UnsavedGuard), whose Save writes them; the rescan after it reads again
// the open documents whose files it replaced.
void ImportController::import_files(const EditorRequest &request) {
	if (!view_.project.open) return;
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	if (preview.open) {
		const std::shared_ptr<const ImportPlan> shown = preview.plan;
		plan_preview(shown.get());
		if (preview.changed) {
			view_.activity.status = "The files changed since the preview: nothing was imported.";
			core_.touch(ViewConcern::Dialogs);
			core_.touch(ViewConcern::Output);
			return core_.refuse_now("import.changed",
			                        "The files changed since the preview: nothing was imported. Check the import again.");
		}
		for (const ImportSource &import : request.imports) {
			const bool planned = std::any_of(preview.plan->rows.begin(), preview.plan->rows.end(), [&import](const ImportPlanRow &row) {
				return row.state != ImportPlanRow::State::NotFound && row.source == import;
			});
			if (!planned)
				return core_.report(make_diagnostic(DiagnosticSeverity::Error, "import.not_planned",
				                                    import.name() + " is not in the import preview: plan it first.", import.name()));
		}
	}
	view_.dialogs.import_preview = DialogsView::ImportPreview();
	if (request.imports.empty()) {
		view_.activity.status = "Nothing to import.";
		core_.touch(ViewConcern::Dialogs);
		core_.touch(ViewConcern::Output);
		return;
	}
	const ImportResult imported = import_assets(request.imports, paths_, *view_.project.document, request.replace);
	if (!imported.imported.empty()) {
		// A Rescan: the open documents whose files it replaced read again, then the refresh.
		core_.documents().reload_changed();
		core_.refresh();
	}
	for (const auto &path : imported.imported) core_.note("Imported " + path);
	for (const auto &path : imported.not_imported) core_.note("Not imported " + path);
	for (const auto &d : imported.diagnostics) core_.report(d);
	const size_t done = imported.imported.size();
	view_.activity.status = !imported.not_imported.empty()
	                       ? std::to_string(done) + " of " + std::to_string(done + imported.not_imported.size()) +
	                                 " files imported: the import stopped at " + imported.not_imported.front() + "."
	                       : std::to_string(done) + " file(s) imported.";
	core_.touch(ViewConcern::Dialogs); // the preview closed
	core_.touch(ViewConcern::Output);
}

void ImportController::refresh_install_files() {
	view_.project.retail_files = view_.project.open ? list_retail_file_names(core_.game_install(), *view_.project.document)
	                                        : std::vector<std::string>();
	core_.touch(ViewConcern::Files);
}

// An import writes over a project file only when it replaces one (else a file of the name is
// refused, or kept when it holds the same bytes): the files its sources make land where the
// plan puts them. The plan has no cap here: import_assets writes every file of the request, so
// every destination is looked at.
void ImportController::unsaved_files(const EditorRequest &request, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	if (!view_.project.open || !request.replace || !documents.documents_dirty()) return;
	core_.problems().validate_pending();
	const ImportPlan plan = plan_import(request.imports, false, paths_, *view_.project.document, *view_.project.scan, core_.problems().graph(),
	                                    core_.game_install(), SIZE_MAX);
	for (const auto &document : documents.documents()) {
		if (!document->dirty()) continue;
		if (std::any_of(plan.rows.begin(), plan.rows.end(), [&document](const ImportPlanRow &row) {
			    return row.state != ImportPlanRow::State::NotFound && row.destination == document->path();
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
