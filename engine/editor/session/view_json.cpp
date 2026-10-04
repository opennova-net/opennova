#include <editor/session/view_json.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>

#include <base/io/cp1252.h>
#include <editor/assets/asset_kind.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_plan_groups.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_run.h>
#include <editor/project_build/export_build.h>
#include <editor/requirements/requirements.h>
#include <editor/session/problem_query.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

using C = ViewConcern;

JsonValue boolean(bool value) {
	return JsonValue::make_bool(value);
}

JsonValue strings_to_json(const std::vector<std::string> &values) {
	JsonValue out = JsonValue::make_array();
	for (const std::string &value : values)
		out.push(json_string(value));
	return out;
}

const char *requirement_state_token(RequirementState state) {
	switch (state) {
		case RequirementState::Present:
			return "present";
		case RequirementState::Missing:
			return "missing";
		case RequirementState::WrongKind:
			return "wrong_kind";
	}
	return "missing";
}

// --- the sections --------------------------------------------------------------------------------

// The status line: the last thing that happened, in a line.
JsonValue status_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	out.set("status", json_string(view.activity.status));
	return out;
}

// The open project: its folder, its project document, how many files the scan lists (the files
// query pages them), and whether the editor asked to quit; the game install's expansions (S16), a
// new project's to build on as an open one's.
JsonValue project_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	out.set("open", boolean(view.project.open));
	out.set("quit_requested", boolean(view.dialogs.quit_requested));
	const auto listed = [](const std::vector<ProjectView::InstallExpansion> &installs) {
		JsonValue expansions = JsonValue::make_array();
		for (const ProjectView::InstallExpansion &installed : installs) {
			JsonValue entry = JsonValue::make_object();
			entry.set("name", json_string(installed.name));
			entry.set("title", json_string(installed.title));
			entry.set("description", json_string(installed.description));
			expansions.push(std::move(entry));
		}
		return expansions;
	};
	out.set("install_expansions", listed(view.project.install_expansions));
	// The install a new project opens with (the editor's last chosen): what New project offers.
	out.set("new_project_expansions", listed(view.project.new_project_expansions));
	// The last install checked (the UX round's project lane): what it holds, in a line.
	const InstallCheck &check = view.project.install_check;
	JsonValue checked = JsonValue::make_object();
	checked.set("root", json_string(check.root));
	checked.set("game", json_string(check.game));
	checked.set("exists", boolean(check.exists));
	checked.set("ok", boolean(check.ok()));
	checked.set("files", json_number(double(check.files)));
	checked.set("executable", boolean(check.executable));
	JsonValue check_expansions = JsonValue::make_array();
	for (const InstallCheck::Expansion &each : check.expansions) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(each.name));
		entry.set("title", json_string(each.title));
		check_expansions.push(std::move(entry));
	}
	checked.set("expansions", std::move(check_expansions));
	JsonValue missing_archives = JsonValue::make_array();
	for (const std::string &name : check.missing_archives) missing_archives.push(json_string(name));
	checked.set("missing_archives", std::move(missing_archives));
	if (check.build) checked.set("build", boolean(true));
	checked.set("words", json_string(check.words()));
	out.set("install_check", std::move(checked));
	// Why the last New project or Open was refused (the welcome page's line), and the editor's own install.
	out.set("refused", json_string(view.project.refused));
	out.set("editor_install", json_string(view.project.editor_install));
	if (!view.project.open)
		return out;
	const ProjectDocument &document = *view.project.document;
	out.set("root", json_string(view.project.root));
	out.set("title", json_string(document.title));
	out.set("id", json_string(document.project_id));
	out.set("target_game", json_string(document.target_game));
	JsonValue features = JsonValue::make_object();
	features.set("menu", boolean(document.features.menu));
	features.set("mission", boolean(document.features.mission));
	features.set("multiplayer", boolean(document.features.multiplayer));
	out.set("features", std::move(features));
	// Its expansion (S16): the one it builds as ("" standalone) and the installed one it builds on.
	JsonValue expansion = JsonValue::make_object();
	expansion.set("name", json_string(document.expansion.name));
	expansion.set("builds_on", json_string(document.expansion.builds_on));
	out.set("expansion", std::move(expansion));
	out.set("file_count", json_number(double(view.project.scan->entries.size())));
	return out;
}

// The requirements report: the counts and every row, a row the last boot report named marked.
JsonValue requirements_section(const SessionView &view) {
	const RequirementReport &report = *view.project.requirements;
	JsonValue out = JsonValue::make_object();
	out.set("total", json_number(double(report.required_total)));
	out.set("missing", json_number(double(report.required_missing)));
	out.set("wrong_kind", json_number(double(report.required_wrong_kind)));
	JsonValue rows = JsonValue::make_array();
	for (const RequirementRow &row : report.rows) {
		JsonValue entry = JsonValue::make_object();
		entry.set("role", json_string(row.role));
		entry.set("name", json_string(row.name));
		entry.set("phase", json_string(requirement_phase_label(row.phase)));
		entry.set("severity", json_number(double(row.severity)));
		entry.set("required", boolean(row.required));
		entry.set("state", json_string(requirement_state_token(row.state)));
		entry.set("expected_kind", json_string(asset_kind_token(row.expected_kind)));
		if (!row.asset_path.empty())
			entry.set("asset", json_string(row.asset_path));
		if (row.found_kind != AssetKind::Unknown)
			entry.set("found_kind", json_string(asset_kind_token(row.found_kind)));
		if (view.activity.missing_at_boot(row.name))
			entry.set("boot_missing", boolean(true));
		rows.push(std::move(entry));
	}
	out.set("rows", std::move(rows));
	return out;
}

// The open documents in short (the documents query pages them whole) and the active one.
JsonValue documents_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	out.set("active", json_string(view.documents.active));
	// The file whose page shows beside the documents (the file_page query reads it), where one does.
	if (!view.documents.page.empty()) out.set("page", json_string(view.documents.page));
	JsonValue open = JsonValue::make_array();
	for (const auto &document : view.documents.open) {
		if (!document)
			continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("path", json_string(document->path()));
		entry.set("kind", json_string(asset_kind_token(document->kind())));
		entry.set("dirty", boolean(document->dirty()));
		entry.set("revision", json_number(double(document->revision())));
		entry.set("can_undo", boolean(document->can_undo()));
		entry.set("can_redo", boolean(document->can_redo()));
		open.push(std::move(entry));
	}
	out.set("count", json_number(double(open.array.size())));
	out.set("open", std::move(open));
	return out;
}

// The selection in the active document (S13 D7: records of any of its rows): the primary record,
// its records (every selected one, the word select_record's field takes), and the clipboard's size.
// Its serial stays in the process: a client reads the Selection concern's stamp (since).
JsonValue selection_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	out.set("document", json_string(view.documents.active));
	out.set("primary", address_to_json(view.documents.selection.primary));
	JsonValue records = JsonValue::make_array();
	for (const NodeAddress &address : view.documents.selection.records)
		records.push(address_to_json(address));
	out.set("records", std::move(records));
	out.set("clipboard_bytes", json_number(double(view.documents.clipboard.size())));
	return out;
}

// Play: the game's state, process, port, exit and boot report, and what Play runs.
JsonValue run_section(const SessionView &view) {
	const ActivityView &activity = view.activity;
	JsonValue out = JsonValue::make_object();
	out.set("state", json_string(play_state_label(activity.play_state)));
	out.set("pid", json_number(double(activity.play_pid)));
	out.set("mcp_port", json_number(double(activity.play_mcp_port)));
	out.set("mission", json_string(activity.play_mission));
	out.set("behind", boolean(activity.play_behind));
	out.set("command_line", json_string(activity.play_command_line));
	out.set("run_dir", json_string(activity.play_run_dir));
	out.set("log_file", json_string(activity.play_log_file));
	out.set("exited_on_its_own", boolean(activity.play_exited_on_its_own));
	out.set("exit_code",
			activity.play_exit_code >= 0 ? json_number(double(activity.play_exit_code))
										 : JsonValue::make_null());
	out.set("in_install", boolean(view.project.play_retail));
	out.set("game_install", json_string(view.project.retail_directory));
	out.set("source_run", boolean(activity.source_run));
	out.set("runtime_executable", json_string(activity.runtime_executable));
	out.set("runtime_setting", json_string(view.project.runtime_setting));
	out.set("boot_missing", strings_to_json(activity.boot_missing));
	return out;
}

// The import dialog in short (the import_preview query pages its plan), the editor's import
// setting, the project's imported sources and the game install's file count.
JsonValue import_section(const SessionView &view) {
	const DialogsView::ImportPreview &preview = view.dialogs.import_preview;
	const ImportPlan &plan = *preview.plan;
	JsonValue out = JsonValue::make_object();
	out.set("open", boolean(preview.open));
	out.set("with_dependencies", boolean(preview.with_dependencies));
	if (preview.all) out.set("all", boolean(true));
	if (preview.changed)
		out.set("changed", boolean(true));
	size_t rows = 0, not_found = 0;
	for (const ImportPlanRow &row : plan.rows)
		++(row.state == ImportPlanRow::State::NotFound ? not_found : rows);
	out.set("choice_count", json_number(double(preview.choices.size())));
	out.set("root_count", json_number(double(preview.roots.size())));
	out.set("row_count", json_number(double(rows)));
	out.set("not_found_count", json_number(double(not_found)));
	out.set("truncated", boolean(plan.truncated));
	out.set("import_dependencies", boolean(view.project.import_dependencies));
	out.set("install_files", json_number(double(view.project.retail_files.size())));
	// The importable sources in the project and what their importers made.
	JsonValue imported = JsonValue::make_array();
	for (const ImportedSource &source : *view.project.imports) {
		JsonValue entry = JsonValue::make_object();
		entry.set("source", json_string(source.source));
		entry.set("sidecar", json_string(source.sidecar));
		entry.set("importer", json_string(source.importer));
		entry.set("ok", boolean(source.ok));
		entry.set("reimported", boolean(source.reimported));
		entry.set("inputs", strings_to_json(source.inputs));
		entry.set("outputs", strings_to_json(source.outputs));
		imported.push(std::move(entry));
	}
	out.set("imported", std::move(imported));
	return out;
}

// What waits on the author: the unsaved-changes prompt, the last rename's plan, and what the
// settings' last Apply could not write.
JsonValue dialogs_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	const DialogsView::UnsavedPrompt &unsaved = view.dialogs.unsaved_prompt;
	JsonValue prompt = JsonValue::make_object();
	prompt.set("open", boolean(unsaved.open));
	if (unsaved.open) {
		prompt.set("action", json_string(editor_request_kind_token(unsaved.action)));
		if (!unsaved.target.empty())
			prompt.set("target", json_string(unsaved.target));
		prompt.set("files", strings_to_json(unsaved.files));
		prompt.set("can_discard", boolean(unsaved.can_discard));
	}
	out.set("unsaved_prompt", std::move(prompt));
	// What the last preview_rename planned: every site, before and after, and the refusals.
	const DialogsView::RenamePreview &rename = view.dialogs.rename_preview;
	if (rename.serial) {
		JsonValue preview = JsonValue::make_object();
		preview.set("serial", json_number(double(rename.serial)));
	// The last rename's way back (preview_rename_back): its sites only those the rename wrote.
	preview.set("back", boolean(rename.back));
		preview.set("symbol", boolean(rename.symbol));
		if (rename.symbol)
			preview.set("kind", json_string(reference_row(rename.kind).token));
		preview.set("path", json_string(rename.path));
		if (!rename.locator.empty())
			preview.set("locator", json_string(rename.locator));
		if (!rename.field.empty())
			preview.set("field", json_string(rename.field));
		preview.set("old_name", json_string(rename.old_name));
		preview.set("new_name", json_string(rename.new_name));
		JsonValue sites = JsonValue::make_array();
		for (const RenameSite &site : *rename.sites) {
			JsonValue entry = JsonValue::make_object();
			entry.set("file", json_string(site.file));
			if (!site.record.empty())
				entry.set("record", json_string(site.record));
			// The record in its type's own words where they are not its name (the plain-words lane).
			if (!site.record_title.empty()) entry.set("record_title", json_string(site.record_title));
			if (!site.locator.empty())
				entry.set("locator", json_string(site.locator));
			// A text's site (S13 D9): its span, and the name it holds (UTF-8, read from the game's code
			// page as the graph's names are); the new name is as typed.
			if (site.span.line) {
				JsonValue span = JsonValue::make_object();
				span.set("line", json_number(double(site.span.line)));
				span.set("column", json_number(double(site.span.column)));
				span.set("length", json_number(double(site.span.length)));
				entry.set("span", std::move(span));
			}
			entry.set("field", json_string(site.field));
			entry.set("before", json_string(site.before)); // UTF-8, as the graph's names are
			entry.set("after", json_string(site.after));
			sites.push(std::move(entry));
		}
		preview.set("sites", std::move(sites));
		preview.set("companions", strings_to_json(rename.companions));
		preview.set("refusals", diagnostics_to_json(rename.refusals));
		preview.set("ok", boolean(rename.refusals.empty()));
		out.set("rename_preview", std::move(preview));
	}
	JsonValue settings = JsonValue::make_object();
	settings.set("failures", diagnostics_to_json(view.project.settings_result.failures));
	out.set("settings_result", std::move(settings));
	// What a Replace or an Edit externally would do, asked before (S18: preview_texture_source).
	const DialogsView::TextureSourcePreview &source = view.dialogs.texture_source;
	JsonValue texture_source = JsonValue::make_object();
	texture_source.set("open", boolean(source.open));
	texture_source.set("serial", json_number(double(source.serial)));
	if (source.open) {
		texture_source.set("texture", json_string(source.texture));
		texture_source.set("image", json_string(source.image));
		texture_source.set("forms", strings_to_json(source.forms));
		texture_source.set("form", json_string(source.form));
		texture_source.set("changes", strings_to_json(source.changes));
		texture_source.set("before", json_string(source.before_words));
		texture_source.set("after", json_string(source.after_words));
		texture_source.set("refusal", json_string(source.refusal));
	}
	out.set("texture_source", std::move(texture_source));
	return out;
}

// How many findings the Problems rows hold, by severity (the problems query pages them), counted as
// Problems and the menu bar count them: the modder's, the game's own data's apart (ADR 0046 S15).
JsonValue problem_counts_section(const SessionView &view) {
	const ProblemCounts counts = count_problems(view);
	JsonValue out = JsonValue::make_object();
	out.set("count", json_number(double(view.findings.diagnostics.size())));
	out.set("errors", json_number(double(counts.errors)));
	out.set("warnings", json_number(double(counts.warnings)));
	out.set("infos", json_number(double(counts.infos)));
	JsonValue original = JsonValue::make_object();
	original.set("errors", json_number(double(counts.original_errors)));
	original.set("warnings", json_number(double(counts.original_warnings)));
	original.set("infos", json_number(double(counts.original_infos)));
	out.set("original", std::move(original));
	// The rows a build is refused for (the gate's refusals; Problems marks them "Blocks the build").
	out.set("blocking", json_number(double(counts.blocking)));
	return out;
}

// What the asset graph holds: its files, edges, symbols and missing edges (a count kept by the
// graph, never a walk), totals that move only with its generation, as Graph does. What its last
// update did (GraphStats) moves with every update, one that changed nothing too, so it stays in
// C++.
JsonValue graph_counts_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	if (!view.findings.graph)
		return out;
	const AssetGraph &graph = *view.findings.graph;
	out.set("files", json_number(double(graph.index().slot_count())));
	out.set("edges", json_number(double(graph.edge_count())));
	out.set("symbols", json_number(double(graph.symbol_count())));
	out.set("missing", json_number(double(graph.missing_count())));
	return out;
}

// The editor's settings the windows read.
JsonValue preferences_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	// Each recent project with what its project file says (the UX round's project lane).
	JsonValue recent = JsonValue::make_array();
	for (size_t i = 0; i < view.project.recent_projects.size(); ++i) {
		JsonValue entry = JsonValue::make_object();
		entry.set("root", json_string(view.project.recent_projects[i]));
		if (i < view.project.recent_details.size() && view.project.recent_details[i].root == view.project.recent_projects[i]) {
			const ProjectView::RecentProject &details = view.project.recent_details[i];
			entry.set("found", boolean(details.found));
			entry.set("title", json_string(details.title));
			entry.set("game", json_string(details.game));
			entry.set("expansion", json_string(details.expansion));
			entry.set("builds_on", json_string(details.builds_on));
		}
		recent.push(std::move(entry));
	}
	out.set("recent_projects", std::move(recent));
	out.set("game_install", json_string(view.project.retail_directory));
	out.set("play_in_install", boolean(view.project.play_retail));
	out.set("runtime_setting", json_string(view.project.runtime_setting));
	out.set("import_dependencies", boolean(view.project.import_dependencies));
	out.set("build_folder", json_string(view.project.build_folder));
	JsonValue items = JsonValue::make_array();
	for (const int64_t item : view.project.recent_items) items.push(json_number(double(item)));
	out.set("recent_items", std::move(items));
	return out;
}

// The output lines held, by absolute index (the output query pages them).
JsonValue output_section(const SessionView &view) {
	const OutputLog &output = view.activity.output;
	JsonValue out = JsonValue::make_object();
	out.set("first", json_number(double(output.first_index())));
	out.set("next", json_number(double(output.next_index())));
	out.set("count", json_number(double(output.next_index() - output.first_index())));
	return out;
}

// The view events held, by seq (the events query pages them).
JsonValue events_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	out.set("first", json_number(double(view.events.first_seq())));
	out.set("next", json_number(double(view.events.next_seq())));
	out.set("count", json_number(double(view.events.held().size())));
	return out;
}

using S = ViewSection;

constexpr ViewSectionRow kSections[] = {
	{ S::Status, "status", concern_set({ C::Output }), status_section,
			"The status line: the last thing that happened, in a line." },
	{ S::Project, "project", concern_set({ C::Project, C::Files, C::Preferences }), project_section,
			"The open project: open, its root, title, id, target game, features and expansion {name, "
			"builds_on} (S16: \"\" a standalone project, \"\" the base game), file_count (the files query "
			"pages the files), and quit_requested; open or not, install_expansions, the game install's "
			"expansions [{name, title, description}] (its folder's name, the Mods list's name and "
			"description), new_project_expansions, the same of the install a new project opens with "
			"(the one last chosen), install_check, the last install checked (check_install, new_project's): its "
			"root, game, exists, ok (the boot table's three archives there and mounting, and no build of a "
			"project's), files, executable, expansions [{name, title}], missing_archives, build and words; "
			"refused, why the last new_project or open_project was refused (\"\" since one started or went "
			"through), and editor_install, the editor's own game install (a new project's)." },
	{ S::Requirements, "requirements", concern_set({ C::Files, C::Run }), requirements_section,
			"The required files: total, missing, wrong_kind, and every row with its role, name, "
			"state and expected kind (boot_missing where the last game reported it missing)." },
	{ S::Documents, "documents", concern_set({ C::Documents, C::DocumentSet, C::ActiveDocument }),
			documents_section,
			"The open documents in short (path, kind, dirty, revision, can_undo, can_redo; the "
			"documents query answers each whole) and the active one." },
	{ S::Selection, "selection", concern_set({ C::Selection }), selection_section,
			"The selection in the active document, over any of its rows: its primary record and "
			"its records, every selected one ({row, kind, child}), and the clipboard's size." },
	{ S::Operation, "operation", concern_set({ C::Operation }), activity_operation_to_json,
			"The operation that runs (its kind: open, refresh, build, import_plan, import_apply or "
			"rename_apply; done and total in its unit, what it works on, cancellable, what it reads "
			"and writes), what the last one came to, the validation the polls step (running, the "
			"files done of total: the problems are the last composed until it ends) and the last "
			"build." },
	{ S::Run, "run", concern_set({ C::Run, C::Preferences }), run_section,
			"Play: the game's state, pid, mcp_port (0 when none with an endpoint runs), the mission "
			"it was started in (\"\" at its menu), behind (its window started behind the others), exit_code, "
			"the run directory it runs in and the log there Play tails (run_dir, log_file: never "
			"the build directory), the files it reported missing at boot, and what Play runs (the "
			"game install, in it or not, the runtime)." },
	{ S::Import, "import", concern_set({ C::Dialogs, C::Preferences, C::Files }), import_section,
			"The import dialog in short (open, with_dependencies, its lists' counts; the "
			"import_preview query pages its plan), the editor's import setting, the project's "
			"import sources (each with the other files its import read, inputs, and the files it "
			"made, outputs) and the game install's file count." },
	{ S::Dialogs, "dialogs", concern_set({ C::Dialogs }), dialogs_section,
			"The unsaved-changes prompt (what waits, the files it lists, whether Discard is "
			"offered), the last rename's plan (rename_preview: its sites before and after, its "
			"refusals), what the settings' last Apply could not write, and the texture_source "
			"dialog (S18: open, serial, the texture, the image, the stored forms offered and the "
			"one written, the changes, the texture before and after in words, the refusal)." },
	{ S::ProblemCounts, "problem_counts", concern_set({ C::Findings, C::DocumentSet }), problem_counts_section,
			"How many Problems rows there are, by severity (the problems query pages them)." },
	{ S::GraphCounts, "graph_counts", concern_set({ C::Graph }), graph_counts_section,
			"What the asset graph holds: files, edges, symbols and missing (the references that "
			"resolve to nothing)." },
	{ S::Preferences, "preferences", concern_set({ C::Preferences }), preferences_section,
			"The editor's settings: the recent projects [{root, found, title, game, expansion, builds_on}], "
			"the game install, Play in it, the runtime, the import setting." },
	{ S::Output, "output", concern_set({ C::Output }), output_section,
			"The output lines held, first and next by absolute index (the output query pages "
			"them)." },
	{ S::Events, "events", concern_set({ C::Selection, C::Dialogs, C::Workspace }), events_section,
			"The view events held, first and next by seq (the events query pages them)." },
	{ S::Workspace, "workspace", concern_set({ C::Workspace, C::Preferences, C::DocumentSet, C::ActiveDocument }),
			workspace_to_json,
			"What the windows show of their own (set_workspace sets it; the catalog's workspace lists the parts): "
			"the file whose card shows (card {path}, \"\" none), the sound the editor plays (sound {path, state: "
			"idle, starting, playing, ended, stopped or failed, serial, error}), the build result's panel "
			"(build_result {open}), the new-project form (new_project {open: File > New project...'s modal, title, "
			"dir, game_install as the form shows it, install_named, builds_on, as_expansion, expansion}), Project "
			"settings (settings {open, and while open its fields}), the New file prompt (new_file {kind, \"\" "
			"closed, name, values}), Rename... (file_rename {path, name}), Rename everywhere (rename {open, path, "
			"locator, field, old_name, kind, name}) and Rename back (rename_back {open}), the find bar (find {open, "
			"text, match_case}), Find in project (project_find {open, text}), Files' filter (files {filter, kind}), "
			"Problems' filters and confirmation (problems {severities, text, scope, group, fixable, blocking, "
			"confirm {group | required | finding, label}, confirm_serial}) and each open document's views "
			"(documents [{path, active, filter, kinds, all_rows, sort, every, inspector_filter, new_window_type, "
			"remove_screen, remap_from, remap_to}])." },
};

static_assert(std::size(kSections) == kViewSectionCount, "every view section has exactly one row");

constexpr bool sections_in_order() {
	for (size_t i = 0; i < kViewSectionCount; ++i)
		if (kSections[i].section != static_cast<ViewSection>(i))
			return false;
	return true;
}
static_assert(sections_in_order(), "the view section rows follow the enum's order");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Every section a token of its own, a writer, a concern it follows and a doc.
constexpr bool sections_named() {
	for (size_t i = 0; i < kViewSectionCount; ++i) {
		const ViewSectionRow &row = kSections[i];
		if (!row.token[0] || !row.doc[0] || !row.write || !row.concerns)
			return false;
		for (size_t j = i + 1; j < kViewSectionCount; ++j)
			if (same_text(row.token, kSections[j].token))
				return false;
	}
	return true;
}
static_assert(sections_named(),
		"each view section has a token of its own, a writer, its concerns and a doc");

JsonValue source_to_json(const ImportChoice &source) {
	return import_choice_to_json(source);
}

// One row of the import plan as the dialog shows it.
JsonValue plan_row_to_json(const ImportPlanRow &row) {
	const bool found = row.state != ImportPlanRow::State::NotFound;
	JsonValue entry = JsonValue::make_object();
	entry.set("state",
			json_string(row.state == ImportPlanRow::State::Selected ? "selected"
							: found									? "found"
																	: "not_found"));
	entry.set("name", json_string(row.name));
	entry.set("kind", json_string(asset_kind_token(row.kind)));
	if (!row.needed_by.file.empty()) {
		JsonValue need = JsonValue::make_object();
		need.set("file", json_string(row.needed_by.file));
		need.set("record", json_string(row.needed_by.record));
		need.set("field", json_string(row.needed_by.field));
		need.set("reference", json_string(reference_row(row.needed_by.reference).token));
		need.set("name", json_string(row.needed_by.name));
		if (row.needed_by.loader_arg >= 0)
			need.set("loader_arg", json_number(double(row.needed_by.loader_arg)));
		// What wanted it in words (the UX round's project lane): the dialog's Needed by.
		need.set("words", json_string(import_need_text(row.needed_by)));
		entry.set("needed_by", std::move(need));
	}
	// Every planned file that names it, where more than the first does.
	if (row.wanted_by.size() > 1) entry.set("wanted_by", strings_to_json(row.wanted_by));
	if (!found)
		return entry;
	entry.set("source", source_to_json(row.source));
	entry.set("destination", json_string(row.destination));
	entry.set("size", json_number(double(row.size)));
	if (!row.made_from.empty())
		entry.set("made_from", json_string(row.made_from));
	entry.set("found_in", json_string(row.found_in));
	entry.set("selected", boolean(row.selected));
	if (row.held)
		entry.set("held", boolean(true));
	// Whether the project's file of the name holds the same bytes, where the plan compared them.
	if (row.held && row.held_as != ImportPlanRow::Held::Unknown)
		entry.set("held_same", boolean(row.held_as == ImportPlanRow::Held::Same));
	if (!row.problem.empty())
		entry.set("problem", json_string(row.problem));
	if (!row.rivals.empty()) {
		JsonValue rivals = JsonValue::make_array();
		for (const ImportRival &rival : row.rivals) {
			JsonValue other = JsonValue::make_object();
			other.set("name", json_string(rival.name));
			other.set("found_in", json_string(rival.found_in));
			other.set("differs", boolean(rival.differs));
			other.set("source", source_to_json(rival.source));
			rivals.push(std::move(other));
		}
		entry.set("rivals", std::move(rivals));
	}
	return entry;
}

// A page by absolute index or seq: [cursor, last) of what is held from `first` to `next`, a
// cursor below `first` moved up to it.
struct CursorPage {
	uint64_t cursor = 0;
	uint64_t last = 0;
};

CursorPage cursor_page(uint64_t first, uint64_t next, uint64_t cursor, size_t limit) {
	CursorPage page;
	page.cursor = std::min<uint64_t>(std::max<uint64_t>(cursor, first), next);
	page.last = std::min<uint64_t>(page.cursor + limit, next);
	return page;
}

void set_cursor_page(JsonValue &out, uint64_t first, uint64_t next, const CursorPage &page) {
	out.set("first", json_number(double(first)));
	out.set("next", json_number(double(next)));
	out.set("count", json_number(double(next - first)));
	out.set("cursor", json_number(double(page.cursor)));
	out.set("next_cursor", json_number(double(page.last)));
}

} // namespace

const ViewSectionRow &view_section_row(ViewSection section) {
	const size_t index = static_cast<size_t>(section);
	return kSections[index < kViewSectionCount ? index : 0];
}

bool view_section_from_token(const std::string &token, ViewSection &out) {
	for (const ViewSectionRow &row : kSections) {
		if (token == row.token) {
			out = row.section;
			return true;
		}
	}
	return false;
}

JsonValue view_section_to_json(const SessionView &view, ViewSection section) {
	return view_section_row(section).write(view);
}

bool view_section_moved(const SessionView &view, ViewSection section, uint64_t since) {
	const ConcernSet concerns = view_section_row(section).concerns;
	for (size_t i = 0; i < kViewConcernCount; ++i) {
		const ViewConcern concern = static_cast<ViewConcern>(i);
		if ((concerns & concern_bit(concern)) && view.revisions.stamp(concern) > since)
			return true;
	}
	return false;
}

JsonValue operation_status_to_json(const OperationStatus &status) {
	JsonValue out = JsonValue::make_object();
	out.set("running", boolean(status.running()));
	if (!status.running())
		return out;
	out.set("id", json_number(double(status.id)));
	out.set("kind", json_string(operation_kind_row(status.kind).token));
	out.set("label", json_string(status.label));
	out.set("done", json_number(double(status.done)));
	out.set("total", json_number(double(status.total)));
	out.set("unit", json_string(operation_unit_token(status.unit)));
	out.set("cancellable", boolean(status.cancellable));
	// What it reads and writes: a request that writes either, or reads what it writes, waits.
	JsonValue reads = JsonValue::make_array();
	for (const char *token : holds_tokens(status.reads))
		reads.push(json_string(token));
	out.set("reads", std::move(reads));
	JsonValue writes = JsonValue::make_array();
	for (const char *token : holds_tokens(status.writes))
		writes.push(json_string(token));
	out.set("writes", std::move(writes));
	return out;
}

JsonValue operation_outcome_to_json(const OperationOutcome &outcome) {
	if (outcome.id == 0)
		return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	out.set("id", json_number(double(outcome.id)));
	out.set("kind", json_string(operation_kind_row(outcome.kind).token));
	out.set("end", json_string(operation_end_token(outcome.end)));
	out.set("findings", diagnostics_to_json(outcome.findings));
	// An import's write: the files it wrote and those it did not reach, each only when it has any.
	if (!outcome.imported.empty()) out.set("imported", strings_to_json(outcome.imported));
	if (!outcome.not_imported.empty()) out.set("not_imported", strings_to_json(outcome.not_imported));
	return out;
}

JsonValue activity_operation_to_json(const SessionView &view) {
	const ActivityView &activity = view.activity;
	JsonValue out = JsonValue::make_object();
	out.set("operation", operation_status_to_json(activity.operation));
	out.set("last_operation", operation_outcome_to_json(activity.last_operation));
	JsonValue validation = JsonValue::make_object();
	validation.set("running", boolean(activity.validation.running));
	validation.set("done", json_number(double(activity.validation.done)));
	validation.set("total", json_number(double(activity.validation.total)));
	validation.set("read", boolean(activity.validation.read));
	out.set("validation", std::move(validation));
	JsonValue build = JsonValue::make_object();
	build.set("has_build", boolean(activity.has_build));
	if (activity.has_build) {
		const BuildReport &report = *activity.last_build;
		build.set("ok", boolean(report.ok));
		build.set("id", json_string(report.build_id));
		build.set("dir", json_string(report.build_dir));
		build.set("expansion", json_string(report.expansion));
		build.set("reused_existing", boolean(report.reused_existing));
		build.set("archives_written", json_number(double(report.archives_written.size())));
		build.set("archives_reused", json_number(double(report.archives_reused.size())));
		build.set("archives_linked", json_number(double(report.archives_linked.size())));
		build.set("loose_written", json_number(double(report.loose_written.size())));
		build.set("files_hashed", json_number(double(report.files_hashed)));
		build.set("bytes_hashed", json_number(double(report.bytes_hashed)));
		// An expansion's files left out as the base game's own (ADR 0046 S16, lean packing).
		JsonValue same = JsonValue::make_object();
		same.set("files", json_number(double(report.same_as_base_files)));
		same.set("bytes", json_number(double(report.same_as_base_bytes)));
		same.set("base_bytes_read", json_number(double(report.base_bytes_read)));
		build.set("same_as_base", std::move(same));
		build.set("diagnostics", diagnostics_to_json(report.diagnostics));
		// The build panel's (the UX round's problems lane): refused by the gate, and each file it published
		// with its size (how long it took is the panel's alone: a wall clock no two runs share).
		build.set("refused", boolean(report.refused));
		JsonValue built = JsonValue::make_array();
		for (const BuiltFile &file : report.built) {
			JsonValue entry = JsonValue::make_object();
			entry.set("name", json_string(file.name));
			entry.set("bytes", json_number(double(file.bytes)));
			if (file.archive) {
				entry.set("files", json_number(double(file.files)));
				entry.set("reused", boolean(file.reused));
			}
			built.push(std::move(entry));
		}
		build.set("built", std::move(built));
		// The folder's builds of other projects, left as they are (several projects building into one folder).
		JsonValue others = JsonValue::make_array();
		for (const std::string &name : report.others) others.push(json_string(name));
		build.set("others", std::move(others));
	}
	out.set("build", std::move(build));
	JsonValue exported = JsonValue::make_object();
	exported.set("has_export", boolean(activity.has_export));
	if (activity.has_export) {
		const ExportReport &report = *activity.last_export;
		exported.set("ok", boolean(report.ok));
		exported.set("dir", json_string(report.export_dir));
		exported.set("files", json_number(double(report.files.size())));
		exported.set("bytes", json_number(double(report.bytes)));
		exported.set("diagnostics", diagnostics_to_json(report.diagnostics));
	}
	out.set("export", std::move(exported));
	return out;
}

JsonValue view_event_to_json(const ViewEvent &event) {
	JsonValue out = JsonValue::make_object();
	out.set("seq", json_number(double(event.seq)));
	out.set("kind", json_string(view_event_kind_token(event.kind)));
	if (!event.path.empty())
		out.set("path", json_string(event.path));
	if (event.address.row)
		out.set("address", address_to_json(event.address));
	if (!event.field.empty())
		out.set("field", json_string(event.field));
	if (!event.locator.empty())
		out.set("locator", json_string(event.locator));
	if (event.flag)
		out.set("flag", boolean(true));
	if (event.tag)
		out.set("tag", json_number(double(event.tag)));
	return out;
}

JsonValue output_page_to_json(const OutputLog &output, uint64_t cursor, size_t limit) {
	const uint64_t first = output.first_index(), next = output.next_index();
	const CursorPage page = cursor_page(first, next, cursor, limit);
	JsonValue out = JsonValue::make_object();
	set_cursor_page(out, first, next, page);
	JsonValue lines = JsonValue::make_array();
	JsonValue folded = JsonValue::make_array();
	for (uint64_t i = page.cursor; i < page.last; ++i) {
		lines.push(json_string(output.at(i)));
		// A line with others folded under it (an import's files, the game's log): where, and how many.
		const size_t count = output.folded_at(i).size() + output.folded_dropped(static_cast<size_t>(i - output.first_index()));
		if (count == 0) continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("at", json_number(double(i)));
		entry.set("count", json_number(double(count)));
		folded.push(std::move(entry));
	}
	out.set("lines", std::move(lines));
	out.set("folded", std::move(folded));
	return out;
}

JsonValue output_folded_to_json(const OutputLog &output, uint64_t at, uint64_t cursor, size_t limit) {
	const std::vector<std::string> &folded = output.folded_at(at);
	const CursorPage page = cursor_page(0, folded.size(), cursor, limit);
	JsonValue out = JsonValue::make_object();
	out.set("at", json_number(double(at)));
	out.set("line", json_string(output.at(at)));
	out.set("dropped", json_number(double(output.folded_dropped(static_cast<size_t>(at - output.first_index())))));
	set_cursor_page(out, 0, folded.size(), page);
	JsonValue lines = JsonValue::make_array();
	for (uint64_t i = page.cursor; i < page.last; ++i) lines.push(json_string(folded[size_t(i)]));
	out.set("lines", std::move(lines));
	return out;
}

JsonValue events_page_to_json(const ViewEvents &events, uint64_t cursor, size_t limit) {
	const uint64_t first = events.first_seq(), next = events.next_seq();
	const CursorPage page = cursor_page(first, next, cursor, limit);
	JsonValue out = JsonValue::make_object();
	set_cursor_page(out, first, next, page);
	const std::deque<ViewEvent> &held = events.held();
	JsonValue items = JsonValue::make_array();
	for (uint64_t seq = page.cursor; seq < page.last; ++seq)
		items.push(view_event_to_json(held[size_t(seq - first)]));
	out.set("items", std::move(items));
	return out;
}

JsonValue import_preview_to_json(const SessionView &view, const JsonPage &page, AssetKind kind) {
	const DialogsView::ImportPreview &preview = view.dialogs.import_preview;
	const ImportPlan &plan = *preview.plan;
	JsonValue out = JsonValue::make_object();
	out.set("open", boolean(preview.open));
	out.set("with_dependencies", boolean(preview.with_dependencies));
	if (preview.all) out.set("all", boolean(true));
	if (preview.changed)
		out.set("changed", boolean(true));
	// The plan's importable rows (the paged list; those of one kind when one is asked) and the
	// rows not found, apart, in plan order.
	const bool one_kind = kind != AssetKind::kCount;
	if (one_kind) out.set("kind", json_string(asset_kind_token(kind)));
	std::vector<const ImportPlanRow *> rows, not_found;
	for (const ImportPlanRow &row : plan.rows) {
		if (row.state == ImportPlanRow::State::NotFound) not_found.push_back(&row);
		else if (!one_kind || row.kind == kind) rows.push_back(&row);
	}
	// `count` the rows'; the page runs on while any list it covers has entries past it.
	set_page(out, page, rows.size(),
			std::max({ preview.choices.size(), preview.roots.size(), not_found.size() }));
	// What the whole plan copies, whatever the page shows of it: in all, and by kind.
	out.set("total_bytes", json_number(double(plan.total_bytes())));
	JsonValue summary = JsonValue::make_array();
	for (const ImportPlanKind &entry : plan.by_kind()) {
		JsonValue line = JsonValue::make_object();
		line.set("kind", json_string(asset_kind_token(entry.kind)));
		line.set("files", json_number(double(entry.files)));
		line.set("bytes", json_number(double(entry.bytes)));
		summary.push(std::move(line));
	}
	out.set("summary", std::move(summary));
	// The rows by what they come for (import_plan_groups: each chosen file, the kinds of the files it
	// brings under it), every group whatever the page; each row says its group.
	const std::vector<ImportPlanGroup> grouped_rows = import_plan_groups(plan);
	std::map<const ImportPlanRow *, size_t> group_of;
	JsonValue groups = JsonValue::make_array();
	for (size_t g = 0; g < grouped_rows.size(); ++g) {
		const ImportPlanGroup &group = grouped_rows[g];
		for (const size_t row : group.rows) group_of.emplace(&plan.rows[row], g);
		JsonValue line = JsonValue::make_object();
		line.set("depth", json_number(double(group.depth)));
		if (group.parent != ImportPlanGroup::kNone) line.set("parent", json_number(double(group.parent)));
		if (group.chosen()) line.set("chosen", json_string(plan.rows[group.root].name));
		line.set("kind", json_string(asset_kind_token(group.kind)));
		line.set("files", json_number(double(group.files)));
		line.set("bytes", json_number(double(group.bytes)));
		if (!group.also.empty()) line.set("also", json_number(double(group.also.size())));
		groups.push(std::move(line));
	}
	out.set("groups", std::move(groups));
	JsonValue planned = JsonValue::make_array();
	for (size_t i = page.first(rows.size()); i < page.last(rows.size()); ++i) {
		JsonValue row = plan_row_to_json(*rows[i]);
		if (const auto in = group_of.find(rows[i]); in != group_of.end()) row.set("group", json_number(double(in->second)));
		planned.push(std::move(row));
	}
	out.set("rows", std::move(planned));
	const auto sources_page = [&page](const std::vector<ImportChoice> &sources) {
		JsonValue list = JsonValue::make_array();
		for (size_t i = page.first(sources.size()); i < page.last(sources.size()); ++i)
			list.push(import_choice_to_json(sources[i]));
		return list;
	};
	out.set("choice_count", json_number(double(preview.choices.size())));
	// Each choice with its kind by its name and its size as stored (the chooser's columns).
	JsonValue choices = JsonValue::make_array();
	for (size_t i = page.first(preview.choices.size()); i < page.last(preview.choices.size()); ++i) {
		JsonValue choice = import_choice_to_json(preview.choices[i]);
		if (i < preview.facts.size()) {
			choice.set("kind", json_string(asset_kind_token(preview.facts[i].kind)));
			choice.set("size", json_number(double(preview.facts[i].size)));
		}
		choices.push(std::move(choice));
	}
	out.set("choices", std::move(choices));
	out.set("root_count", json_number(double(preview.roots.size())));
	out.set("roots", sources_page(preview.roots));
	JsonValue missing = JsonValue::make_array();
	for (size_t i = page.first(not_found.size()); i < page.last(not_found.size()); ++i)
		missing.push(plan_row_to_json(*not_found[i]));
	out.set("not_found_count", json_number(double(not_found.size())));
	out.set("not_found", std::move(missing));
	const auto counted = [](const std::vector<ImportNotFollowed> &entries) {
		JsonValue list = JsonValue::make_array();
		for (const ImportNotFollowed &kind : entries) {
			JsonValue entry = JsonValue::make_object();
			if (kind.reference != ReferenceKind::None)
				entry.set("reference", json_string(reference_row(kind.reference).token));
			else
				entry.set("kind", json_string(asset_kind_token(kind.kind)));
			entry.set("count", json_number(double(kind.count)));
			entry.set("first", json_string(kind.first));
			list.push(std::move(entry));
		}
		return list;
	};
	out.set("not_followed", counted(plan.not_followed));
	out.set("undefined", counted(plan.undefined));
	out.set("shadowed", counted(plan.shadowed));
	out.set("truncated", boolean(plan.truncated));
	out.set("diagnostics", diagnostics_to_json(plan.diagnostics));
	return out;
}

} // namespace opennova::editor
