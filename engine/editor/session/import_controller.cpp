#include <editor/session/import_controller.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <base/io/file_time.h>
#include <base/io/strutil.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/import/sidecar.h>
#include <editor/import/texture_source.h>
#include <editor/model/diagnostic.h>
#include <editor/model/field_text.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/import_operation.h>
#include <editor/session/import_plan_operation.h>
#include <editor/session/problems_service.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_core.h>
#include <editor/session/texture_import_state.h>
#include <editor/session/view/view_events.h>

namespace opennova::editor {

ImportController::ImportController(SessionCore &core) : core_(core), view_(core.view()), paths_(core.paths()) {}

// An import's one Output line (the UX round's problems lane): how many files, how many bytes, and how many
// of each kind, the most first ("Imported 2,237 files (233.4 MB): Texture 1,093, Model 592, Wave 300 and 9
// more kinds."), each file as the scan lists it now (an import's files are read into it before this).
std::string ImportController::import_words(const std::vector<std::string> &paths) const {
	std::map<AssetKind, size_t> kinds;
	uint64_t bytes = 0;
	for (const std::string &path : paths)
		if (const AssetEntry *entry = view_.project.scan ? view_.project.scan->at_path(path) : nullptr) {
			++kinds[entry->kind];
			bytes += entry->size_bytes;
		}
	char size[32];
	if (bytes < (uint64_t(1) << 20)) std::snprintf(size, sizeof(size), "%.1f KB", double(bytes) / 1024.0);
	else std::snprintf(size, sizeof(size), "%.1f MB", double(bytes) / double(uint64_t(1) << 20));
	std::string out = "Imported " + grouped(paths.size()) + (paths.size() == 1 ? " file" : " files") + " (" + size + ")";
	std::vector<std::pair<size_t, AssetKind>> most;
	for (const auto &[kind, count] : kinds) most.emplace_back(count, kind);
	std::sort(most.begin(), most.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	constexpr size_t kNamed = 4;
	for (size_t i = 0; i < most.size() && i < kNamed; ++i)
		out += (i == 0 ? ": " : ", ") + std::string(asset_kind_label(most[i].second)) + " " + grouped(most[i].first);
	if (most.size() > kNamed) out += " and " + counted(most.size() - kNamed, "more kind");
	return out + ". Its files are folded under this line.";
}

void ImportController::preview_files(const EditorRequest &request) {
	if (!view_.project.open) return;
	std::vector<Diagnostic> diagnostics;
	std::vector<ImportChoice> choices, roots;
	std::vector<ImportChoiceFacts> listed, facts;
	// A loose file picked is chosen; an archive's members are listed to choose from, with their facts.
	std::vector<ImportChoice> sources = list_import_choices(request.paths, diagnostics, &listed);
	for (size_t i = 0; i < sources.size(); ++i) {
		if (sources[i].entry.empty()) {
			roots.push_back(std::move(sources[i]));
			continue;
		}
		choices.push_back(std::move(sources[i]));
		facts.push_back(listed[i]);
	}
	for (const auto &d : diagnostics) core_.report(d);
	preview(std::move(choices), std::move(facts), std::move(roots), request.with_dependencies);
}

void ImportController::plan(const EditorRequest &request) {
	if (!view_.project.open) return;
	const DialogsView::ImportPreview &open = view_.dialogs.import_preview;
	preview(open.open ? open.choices : std::vector<ImportChoice>(), open.open ? open.facts : std::vector<ImportChoiceFacts>(),
	        request.imports, request.with_dependencies);
}

void ImportController::preview_install(const EditorRequest &request) {
	if (!view_.project.open) return;
	// Every file chosen takes no names and no walk: a request asking for either too is refused, not
	// served in part (review F14).
	if (request.all && (!request.names.empty() || request.with_dependencies))
		return core_.refuse_now(CoreFinding::ImportRequest,
		                        "Every file of the game install is chosen with no walk: \"all\" takes neither \"names\" nor "
		                        "\"with_dependencies\".");
	std::vector<Diagnostic> diagnostics;
	std::vector<ImportChoiceFacts> facts;
	std::vector<ImportChoice> sources =
	        list_retail_import_choices(core_.game_install(), *view_.project.document, diagnostics, &facts);
	// Everything: every file chosen, none to choose from, no walk (the closure of everything is
	// everything: nothing a walk could find is not chosen already).
	if (request.all) {
		for (const auto &d : diagnostics) core_.report(d);
		preview({}, {}, std::move(sources), false, true);
		return;
	}
	// With names (an Import fix): those files alone, chosen; a name the game data does
	// not have is a finding (unless the install itself is the finding). Without, every
	// file is listed to choose from.
	std::vector<ImportChoice> named;
	std::map<std::string, size_t> by_name; // the install's files by name, the first of each, once (review F7)
	if (!request.names.empty())
		for (size_t i = 0; i < sources.size(); ++i) by_name.emplace(normalized_logical_name(sources[i].name()), i);
	for (const std::string &name : request.names) {
		const auto found = by_name.find(normalized_logical_name(name));
		if (found == by_name.end()) {
			if (diagnostics.empty())
				diagnostics.push_back(make_finding(CoreFinding::ImportNotFound, DiagnosticSeverity::Error,
				                                   "The game data has no file named " + name + "."));
			continue;
		}
		named.push_back(sources[found->second]);
	}
	if (!request.names.empty()) {
		sources.clear();
		facts.clear();
	}
	for (const auto &d : diagnostics) core_.report(d);
	preview(std::move(sources), std::move(facts), std::move(named), request.with_dependencies);
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

// The record of the import `path` names, each value set as its option's row takes it ("" back to its
// row's fallback: left out of the record), written only when it changed, then the refresh that imports
// it again (the pass takes a record that changed as stale, and drops an output the import no longer
// makes).
void ImportController::set_options(const std::string &path,
                                   const std::vector<std::pair<std::string, std::string>> &values) {
	if (!view_.project.open) return;
	TextureImportState state;
	std::string error;
	if (!texture_import_state(view_, path, state, error))
		return core_.refuse_now(CoreFinding::ImportOption, "No import options to set: " + error, path);
	ImportSidecar sidecar = state.sidecar;
	for (const auto &[key, value] : values) {
		const ImportOptionRow *row = import_option_row(state.importer->options, key);
		if (!row) {
			std::string keys;
			for (const ImportOptionRow &each : state.importer->options) keys += (keys.empty() ? "" : ", ") + each.key;
			return core_.refuse_now(CoreFinding::ImportOption,
			                        "The " + state.sidecar.importer + " importer has no option '" + key +
			                                "' (its options: " + keys + ").",
			                        state.source);
		}
		const std::string taken = row->keeps_case ? value : strutil::to_lower(value);
		if (taken.empty()) {
			sidecar.options.erase(key);
			continue;
		}
		if (!import_option_accepts(*row, taken))
			return core_.refuse_now(CoreFinding::ImportOption,
			                        "The " + state.sidecar.importer + " importer's " + key + " takes " +
			                                import_option_takes(*row) + "; '" + value + "' is none of them.",
			                        state.source);
		sidecar.options[key] = taken;
	}
	if (sidecar.options == state.sidecar.options) return;
	Diagnostic write_error;
	if (!save_import_sidecar(join_path(view_.project.root, state.record), sidecar, write_error))
		return core_.refuse_now(CoreFinding::ImportSidecar, write_error.message, state.record);
	reimport(state.source, false);
}

// The plan a Replace of `request.path` by the image request.paths names would carry out (refusals inside):
// the image read (a file on disk, or a project file by its path), what the texture's uses ask of it.
TextureSourcePlan ImportController::replace_plan(const EditorRequest &request) const {
	TextureSourcePlan plan;
	if (request.paths.size() != 1) {
		plan.refusals.push_back(make_finding(CoreFinding::TextureReplace, DiagnosticSeverity::Error, "A texture is replaced by one image.",
		                                     request.path));
		return plan;
	}
	const std::string &image = request.paths.front();
	const std::string file = path_of(image).is_absolute() ? image : join_path(view_.project.root, image);
	std::vector<uint8_t> bytes;
	std::string message;
	if (!read_file_bytes(file, bytes, message)) {
		plan.refusals.push_back(make_finding(CoreFinding::TextureReplace, DiagnosticSeverity::Error,
		                                     basename_of(image) + " could not be read: " + message, request.path));
		return plan;
	}
	const ImportOptions overrides(request.values.begin(), request.values.end());
	return plan_texture_replace(paths_, *view_.project.scan, request.path, basename_of(image), bytes, overrides,
	                            texture_use_asks(view_, request.path));
}

void ImportController::replace_texture(const EditorRequest &request) {
	if (!view_.project.open) return;
	const TextureSourcePlan plan = replace_plan(request);
	const std::string image = request.paths.empty() ? std::string() : request.paths.front();
	if (!plan.ok()) {
		for (size_t i = 0; i + 1 < plan.refusals.size(); ++i) core_.report(plan.refusals[i]);
		return core_.refuse_now(CoreFinding::TextureReplace, plan.refusals.back().message, plan.refusals.back().asset);
	}
	// The file set aside takes its open document with it: one with unsaved edits waits for them.
	if (!plan.replaced.empty())
		if (DocumentBase *open = core_.documents().document_for(plan.replaced)) {
			if (open->dirty())
				return core_.refuse_now(CoreFinding::TextureReplace,
				                        plan.texture + " is open with unsaved edits: save or discard them before replacing it.",
				                        plan.replaced);
			core_.documents().close_document(plan.replaced);
		}
	std::vector<Diagnostic> findings;
	if (!apply_texture_source(paths_, plan, findings)) {
		for (size_t i = 0; i + 1 < findings.size(); ++i) core_.report(findings[i]);
		return core_.refuse_now(CoreFinding::TextureReplace,
		                        findings.empty() ? std::string("The texture could not be replaced.") : findings.back().message, plan.texture);
	}
	close_texture_source();
	core_.note("Replaced " + plan.texture + " with " + basename_of(image) + ": " + plan.source + " makes it now, as its import record says" +
	           (plan.replaced.empty() ? std::string(".") : "; the file it replaced is kept under " + std::string(kReplacedFolder) + "/."));
	reimport(plan.source, true);
}

void ImportController::preview_texture_source(const EditorRequest &request) {
	if (!view_.project.open || !view_.project.scan) return;
	DialogsView::TextureSourcePreview &preview = view_.dialogs.texture_source;
	const uint64_t serial = preview.serial + 1;
	preview = DialogsView::TextureSourcePreview();
	preview.open = true;
	preview.serial = serial;
	preview.texture = request.path;
	preview.image = request.paths.empty() ? std::string() : request.paths.front();
	preview.values = request.values;
	// The stored forms the texture's name offers: the format's tokens within its extension.
	const std::string extension = strutil::to_lower(utf8_of(path_of(request.path).extension()));
	if (extension == ".tga") preview.forms = {"tga", "tga24"};
	else if (extension == ".pcx") preview.forms = {"pcx", "pcx24"};
	else if (extension == ".dds") preview.forms = {"dxt5", "dxt1", "argb"};
	const TextureSourcePlan plan = preview.image.empty() ? plan_texture_source(paths_, *view_.project.scan, request.path)
	                                                     : replace_plan(request);
	if (!plan.ok()) {
		preview.refusal = plan.refusals.back().message;
	} else {
		preview.changes = plan.changes;
		preview.before_words = plan.before_words;
		preview.after_words = plan.after_words;
		const auto format = plan.options.find("format");
		const auto dds = plan.options.find("dds");
		preview.form = format == plan.options.end() ? std::string("tga")
		               : format->second == "dds" ? (dds == plan.options.end() ? std::string("dxt5") : dds->second)
		                                         : format->second;
		if (view_.documents.thumbnails) {
			const AssetEntry *entry = view_.project.scan->at_path(request.path);
			if (!entry) entry = view_.project.scan->find(basename_of(request.path));
			if (entry) preview.before = view_.documents.thumbnails->make_now(view_, entry->relative_path, TextureLoadTransform::None);
			if (!plan.made.empty()) preview.after = view_.documents.thumbnails->picture_of(plan.made_name, plan.made);
		}
	}
	core_.touch(ViewConcern::Dialogs);
}

void ImportController::close_texture_source() {
	if (!view_.dialogs.texture_source.open) return;
	const uint64_t serial = view_.dialogs.texture_source.serial;
	view_.dialogs.texture_source = DialogsView::TextureSourcePreview();
	view_.dialogs.texture_source.serial = serial;
	core_.touch(ViewConcern::Dialogs);
}

// The OpenExternally view event the Shell opens a source by, and the status line.
void ImportController::post_open_externally(const std::string &source) {
	ViewEvent open;
	open.kind = ViewEventKind::OpenExternally;
	open.path = join_path(view_.project.root, source);
	view_.events.post(std::move(open));
	core_.touch(ViewConcern::Dialogs);
	view_.activity.status = "Opening " + source + " in its program.";
	core_.touch(ViewConcern::Output);
}

// A source edited in place (a PNG the game reads as it is) open with unsaved edits: its program would not
// see them, and a refresh would read over them.
bool ImportController::source_dirty(const std::string &source, CoreFinding code) {
	const DocumentBase *open = core_.documents().document_for(source);
	if (!open || !open->dirty()) return false;
	core_.refuse_now(code, source + " is open with unsaved edits: save or discard them before its program edits it.", source);
	return true;
}

void ImportController::open_texture_source(const EditorRequest &request) {
	if (!view_.project.open) return;
	const TextureSourcePlan plan = plan_texture_source(paths_, *view_.project.scan, request.path);
	if (!plan.ok()) return core_.refuse_now(CoreFinding::TextureExternal, plan.refusals.back().message, plan.refusals.back().asset);
	if (!plan.bytes.empty())
		return core_.refuse_now(CoreFinding::TextureExternal,
		                        plan.texture + " has no source yet: Edit in its program (edit_externally) makes one, " + plan.source +
		                                ", which its import turns into it.",
		                        request.path);
	if (source_dirty(plan.source, CoreFinding::TextureExternal)) return;
	post_open_externally(plan.source);
}

void ImportController::edit_externally(const EditorRequest &request) {
	if (!view_.project.open) return;
	const TextureSourcePlan plan = plan_texture_source(paths_, *view_.project.scan, request.path);
	if (!plan.ok()) return core_.refuse_now(CoreFinding::TextureExternal, plan.refusals.back().message, plan.refusals.back().asset);
	if (plan.bytes.empty() && source_dirty(plan.source, CoreFinding::TextureExternal)) return;
	if (!plan.bytes.empty()) {
		// A plain texture's source made once: the file set aside takes its open document with it.
		if (DocumentBase *open = core_.documents().document_for(plan.replaced)) {
			if (open->dirty())
				return core_.refuse_now(CoreFinding::TextureExternal,
				                        plan.texture + " is open with unsaved edits: save or discard them before its program edits it.",
				                        plan.replaced);
			core_.documents().close_document(plan.replaced);
		}
		std::vector<Diagnostic> findings;
		if (!apply_texture_source(paths_, plan, findings)) {
			for (size_t i = 0; i + 1 < findings.size(); ++i) core_.report(findings[i]);
			return core_.refuse_now(CoreFinding::TextureExternal,
			                        findings.empty() ? std::string("No source could be made.") : findings.back().message, plan.texture);
		}
		core_.note("Made " + plan.source + " for " + plan.texture + ": its import makes the texture from it now, so what its "
		           "program saves there comes back; the file it replaced is kept under " + std::string(kReplacedFolder) + "/.");
		reimport(plan.source, true);
	}
	close_texture_source();
	post_open_externally(plan.source);
}

// What a program saved of the watched files, refreshed alone (the sources imported again, the scan updated
// for them and the files that moved); a file written too recently waits for a later check, never read
// half-written.
void ImportController::refresh_changed_sources() {
	if (!view_.project.open || !view_.project.scan) return;
	ExternalChanges changes = external_changes(paths_, *view_.project.scan,
	                                           view_.project.imports ? *view_.project.imports : std::vector<ImportedSource>(),
	                                           io::file_clock_now_ticks());
	if (changes.empty()) return;
	if (core_.start_changed_refresh(std::move(changes)))
		view_.activity.status = "Reading what its program saved...";
	core_.touch(ViewConcern::Output);
}

// The import dialog on `roots` chosen among `choices` (each file once, `facts` saying each one's
// kind and size), planned with the files they need when `with_dependencies`: open while it has
// something to show, a list to choose from or a file chosen. `all`: the roots are every file of the
// game install.
void ImportController::preview(std::vector<ImportChoice> choices, std::vector<ImportChoiceFacts> facts,
                               std::vector<ImportChoice> roots, bool with_dependencies, bool all) {
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	preview.choices = std::move(choices);
	preview.facts = std::move(facts);
	preview.facts.resize(preview.choices.size());
	preview.roots.clear();
	if (all) {
		// The install lists each file once already (list_retail_import_choices): nine thousand
		// roots are not looked up one by one.
		preview.roots = std::move(roots);
	} else {
		std::set<ImportChoice> seen; // each root once, in log time
		for (ImportChoice &root : roots)
			if (seen.insert(root).second) preview.roots.push_back(std::move(root));
	}
	preview.with_dependencies = with_dependencies;
	preview.all = all;
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
	const uint64_t id = core_.start_operation(std::make_unique<ImportPlanOperation>(core_.problems(), paths_,
			*view_.project.document, core_.problems().graph(), view_.documents.open, preview.roots,
			preview.with_dependencies, core_.game_install()));
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
		size_t files = 0, found = 0, missing = 0, held = 0;
		for (const ImportPlanRow &row : preview.plan->rows) {
			if (row.state == ImportPlanRow::State::NotFound) ++missing;
			else if (row.selected) ++files;
			if (row.state == ImportPlanRow::State::Found) ++found;
			held += row.held ? 1 : 0;
		}
		view_.activity.status = preview.roots.empty() ? std::string("Choose the files to import.")
		               : "Import preview: " + counted(files, "file") + " to import" +
		                         (preview.with_dependencies ? " (" + grouped(found) + " the chosen ones need), " +
		                                                              grouped(missing) + " not found."
		                                                    : std::string(".")) +
		                         (held ? " " + grouped(held) + " the project has already: kept unless replaced." : "");
	}
	core_.touch(ViewConcern::Dialogs);
	if (preview.open) core_.touch(ViewConcern::Output);
}

// The import dialog's "Include the files these need": the editor's preference, written from a
// copy (a preference that could not be written stays the one in effect, its failure a finding).
// An open preview is planned again with the setting asked for, whatever the setting was (S13 A3:
// a plan it takes the place of is never left half made), as a plan_import plans it, through the
// busy gate: a running plan gives way to it, and another operation refuses the plan (the setting
// stays written, the dialog on the plan it shows).
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
	core_.touch(ViewConcern::Preferences);
	core_.touch(ViewConcern::Output);
	const DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	// A preview of everything stays as it is: a walk of every file finds nothing not chosen.
	if (preview.open && !preview.all) {
		EditorRequest replan = request::of(EditorRequestKind::PlanImport);
		replan.imports = preview.roots;
		replan.with_dependencies = with_dependencies;
		serve_request(core_, replan);
	}
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
	// An import of nothing named and nothing planned would plan again only to close the dialog: refused
	// (review F14).
	if (!request.planned && request.imports.empty())
		return core_.refuse_now(CoreFinding::ImportRequest,
		                        "An import names its files (\"imports\") or takes the open preview's plan (\"planned\"): "
		                        "this one does neither.");
	DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	std::vector<ImportChoice> imports;
	if (!sources_of(request, imports)) return;
	std::unique_ptr<ImportPlanOperation> replan;
	std::shared_ptr<const ImportPlan> shown;
	if (preview.open) {
		shown = preview.plan;
		replan = std::make_unique<ImportPlanOperation>(core_.problems(), paths_, *view_.project.document,
				core_.problems().graph(), view_.documents.open, preview.roots, preview.with_dependencies,
				core_.game_install());
	} else if (imports.empty()) {
		view_.activity.status = "Nothing to import.";
		core_.touch(ViewConcern::Output);
		return;
	}
	const uint64_t id = core_.start_operation(std::make_unique<ImportOperation>(paths_, *view_.project.document,
			std::move(imports), request.replace, std::move(replan), std::move(shown)));
	if (id == 0) return core_.refuse_busy(std::string()); // the gate let no operation run beside it
	core_.outcome().operation = id;
	view_.activity.status = "Importing...";
	core_.touch(ViewConcern::Output);
}

bool ImportController::sources_of(const EditorRequest &request, std::vector<ImportChoice> &imports) {
	if (!request.planned) {
		imports = request.imports;
		return true;
	}
	const DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	if (!preview.open) {
		core_.report(make_finding(CoreFinding::ImportNotPlanned, DiagnosticSeverity::Error,
		                          "No import preview is open: plan the files first (preview_import, "
		                          "preview_install_import)."));
		view_.activity.status = "Nothing is planned to import.";
		core_.touch(ViewConcern::Output);
		return false;
	}
	// The rows the plan takes (the dialog's checks of a new plan): each source once, a converter's
	// outputs sharing theirs; a row the project cannot take is left out, as the command line leaves
	// it; a file the project holds is left as it is unless the request replaces (review F2).
	std::set<std::string> taken;
	for (const ImportPlanRow &row : preview.plan->rows) {
		if (row.state == ImportPlanRow::State::NotFound || !(row.selected || (row.held && request.replace)) ||
		    !row.problem.empty())
			continue;
		const std::string key = row.source.path + '\n' + row.source.entry + '\n' + (row.source.install ? '1' : '0') +
		                        (row.source.native ? '1' : '0') + '\n' + row.source.as;
		if (taken.insert(key).second) imports.push_back(row.source);
	}
	return true;
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
	// One line for the import, its files folded under it (the UX round's problems lane: a large import's
	// lines no longer push everything else out of Output).
	std::vector<std::string> each;
	for (const auto &path : imported.imported) each.push_back("Imported " + path);
	for (const auto &path : imported.not_imported) each.push_back("Not imported " + path);
	core_.note_folded(import_words(imported.imported), std::move(each));
	if (!imported.not_imported.empty())
		core_.note("Not imported: " + counted(imported.not_imported.size(), "file") + " (the import stopped at " +
		           imported.not_imported.front() + ").");
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
	                       ? grouped(done) + " of " + counted(done + imported.not_imported.size(), "file") +
	                                 " imported: the import stopped at " + imported.not_imported.front() + "."
	                       : counted(done, "file") + " imported.";
	core_.touch(ViewConcern::Dialogs); // the preview closed
	core_.touch(ViewConcern::Output);
	return outcome;
}

void ImportController::refresh_install_files() {
	view_.project.retail_files = view_.project.open ? list_retail_file_names(core_.game_install(), *view_.project.document)
	                                        : std::vector<std::string>();
	view_.project.base_files = view_.project.open ? list_base_file_names(core_.game_install(), *view_.project.document)
	                                      : std::vector<std::string>();
	core_.touch(ViewConcern::Files);
}

void ImportController::set_install_files(std::vector<std::string> names, std::vector<std::string> base) {
	view_.project.retail_files = std::move(names);
	view_.project.base_files = std::move(base);
	core_.touch(ViewConcern::Files);
}

// An import writes over a project file only when it replaces one (else a file of the name is
// refused, or kept when it holds the same bytes): the files its sources make land where the
// plan puts them. With the import dialog open that is the plan it shows (the ImportPlan
// operation's, S13 A3), reused: the import plans again before it writes and writes nothing when
// the plan is not that one. With none open, the files asked for are planned here, with no cap
// (import_assets writes every file of the request, so every destination is looked at) and none of
// what they need: that plan reads the scan alone, never the graph, so no validation runs first.
void ImportController::unsaved_files(const EditorRequest &request, std::vector<std::string> &files) {
	DocumentSet &documents = core_.documents();
	if (!view_.project.open || !request.replace || !documents.documents_dirty()) return;
	const DialogsView::ImportPreview &preview = view_.dialogs.import_preview;
	std::shared_ptr<const ImportPlan> plan = preview.open ? preview.plan : nullptr;
	if (!plan) {
		if (request.planned) return; // refused once it is served: no preview is open
		plan = std::make_shared<const ImportPlan>(plan_import(request.imports, false, paths_, *view_.project.document,
				*view_.project.scan, core_.problems().graph(), core_.game_install(), SIZE_MAX));
	}
	// A row the request asks for (with planned, one the plan takes), which the plan finds: where
	// its file lands. The places written, gathered once over the rows (a whole install's nine
	// thousand, each source looked up in log time: review F7), then each unsaved document's.
	const std::set<ImportChoice> asked(request.imports.begin(), request.imports.end());
	const auto writes = [&request, &asked](const ImportPlanRow &row) {
		if (row.state == ImportPlanRow::State::NotFound) return false;
		if (request.planned) return (row.selected || (row.held && request.replace)) && row.problem.empty();
		return asked.count(row.source) > 0;
	};
	std::set<std::string> written;
	for (const ImportPlanRow &row : plan->rows)
		if (writes(row)) written.insert(row.destination);
	for (const auto &document : documents.documents())
		if (document->dirty() && written.count(document->path())) files.push_back(document->path());
}

void ImportController::clear() {
	view_.dialogs.import_preview = DialogsView::ImportPreview();
	view_.project.imports = std::make_shared<const std::vector<ImportedSource>>();
	view_.project.retail_files.clear();
	view_.project.base_files.clear();
}

} // namespace opennova::editor
