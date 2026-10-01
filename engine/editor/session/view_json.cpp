#include <editor/session/view_json.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>

#include <base/io/cp1252.h>
#include <editor/assets/asset_kind.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_run.h>
#include <editor/requirements/requirements.h>

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
// query pages them), and whether the editor asked to quit.
JsonValue project_section(const SessionView &view) {
	JsonValue out = JsonValue::make_object();
	out.set("open", boolean(view.project.open));
	out.set("quit_requested", boolean(view.dialogs.quit_requested));
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
	out.set("command_line", json_string(activity.play_command_line));
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
			if (!site.locator.empty())
				entry.set("locator", json_string(site.locator));
			// A text's site (S13 D9): its span, and the name it holds written as its text is, from
			// the game's code page (as the references query writes an edge's); the new name is as
			// typed.
			if (site.span.line) {
				JsonValue span = JsonValue::make_object();
				span.set("line", json_number(double(site.span.line)));
				span.set("column", json_number(double(site.span.column)));
				span.set("length", json_number(double(site.span.length)));
				entry.set("span", std::move(span));
			}
			entry.set("field", json_string(site.field));
			entry.set("before", json_string(site.span.line ? cp1252_to_utf8(site.before) : site.before));
			entry.set("after", json_string(site.after));
			sites.push(std::move(entry));
		}
		preview.set("sites", std::move(sites));
		preview.set("refusals", diagnostics_to_json(rename.refusals));
		preview.set("ok", boolean(rename.refusals.empty()));
		out.set("rename_preview", std::move(preview));
	}
	JsonValue settings = JsonValue::make_object();
	settings.set("failures", diagnostics_to_json(view.project.settings_result.failures));
	out.set("settings_result", std::move(settings));
	return out;
}

// How many findings the Problems rows hold, by severity (the problems query pages them).
JsonValue problem_counts_section(const SessionView &view) {
	size_t errors = 0, warnings = 0, infos = 0;
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.severity == DiagnosticSeverity::Error)
			++errors;
		else if (d.severity == DiagnosticSeverity::Warning)
			++warnings;
		else
			++infos;
	}
	JsonValue out = JsonValue::make_object();
	out.set("count", json_number(double(view.findings.diagnostics.size())));
	out.set("errors", json_number(double(errors)));
	out.set("warnings", json_number(double(warnings)));
	out.set("infos", json_number(double(infos)));
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
	out.set("recent_projects", strings_to_json(view.project.recent_projects));
	out.set("game_install", json_string(view.project.retail_directory));
	out.set("play_in_install", boolean(view.project.play_retail));
	out.set("runtime_setting", json_string(view.project.runtime_setting));
	out.set("import_dependencies", boolean(view.project.import_dependencies));
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
	{ S::Project, "project", concern_set({ C::Project, C::Files }), project_section,
			"The open project: open, its root, title, id, target game and features, file_count "
			"(the files query pages the files), and quit_requested." },
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
			"Play: the game's state, pid, mcp_port (0 when none with an endpoint runs), exit_code, "
			"the files it reported missing at boot, and what Play runs (the game install, in it or "
			"not, the runtime)." },
	{ S::Import, "import", concern_set({ C::Dialogs, C::Preferences, C::Files }), import_section,
			"The import dialog in short (open, with_dependencies, its lists' counts; the "
			"import_preview query pages its plan), the editor's import setting, the project's "
			"imported sources and the game install's file count." },
	{ S::Dialogs, "dialogs", concern_set({ C::Dialogs }), dialogs_section,
			"The unsaved-changes prompt (what waits, the files it lists, whether Discard is "
			"offered), the last rename's plan (rename_preview: its sites before and after, its "
			"refusals) and what the settings' last Apply could not write." },
	{ S::ProblemCounts, "problem_counts", concern_set({ C::Findings }), problem_counts_section,
			"How many Problems rows there are, by severity (the problems query pages them)." },
	{ S::GraphCounts, "graph_counts", concern_set({ C::Graph }), graph_counts_section,
			"What the asset graph holds: files, edges, symbols and missing (the references that "
			"resolve to nothing)." },
	{ S::Preferences, "preferences", concern_set({ C::Preferences }), preferences_section,
			"The editor's settings: the recent projects, the game install, Play in it, the "
			"runtime, the import setting." },
	{ S::Output, "output", concern_set({ C::Output }), output_section,
			"The output lines held, first and next by absolute index (the output query pages "
			"them)." },
	{ S::Events, "events", concern_set({ C::Selection, C::Dialogs }), events_section,
			"The view events held, first and next by seq (the events query pages them)." },
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

JsonValue source_to_json(const ImportSource &source) {
	return import_source_to_json(source);
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
		entry.set("needed_by", std::move(need));
	}
	if (!found)
		return entry;
	entry.set("source", source_to_json(row.source));
	entry.set("destination", json_string(row.destination));
	if (!row.made_from.empty())
		entry.set("made_from", json_string(row.made_from));
	entry.set("found_in", json_string(row.found_in));
	entry.set("selected", boolean(row.selected));
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
	out.set("validation", std::move(validation));
	JsonValue build = JsonValue::make_object();
	build.set("has_build", boolean(activity.has_build));
	if (activity.has_build) {
		const BuildReport &report = *activity.last_build;
		build.set("ok", boolean(report.ok));
		build.set("id", json_string(report.build_id));
		build.set("dir", json_string(report.build_dir));
		build.set("reused_existing", boolean(report.reused_existing));
		build.set("archives_written", json_number(double(report.archives_written.size())));
		build.set("archives_reused", json_number(double(report.archives_reused.size())));
		build.set("loose_written", json_number(double(report.loose_written.size())));
		build.set("diagnostics", diagnostics_to_json(report.diagnostics));
	}
	out.set("build", std::move(build));
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
	for (uint64_t i = page.cursor; i < page.last; ++i)
		lines.push(json_string(output.at(i)));
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

JsonValue import_preview_to_json(const SessionView &view, const JsonPage &page) {
	const DialogsView::ImportPreview &preview = view.dialogs.import_preview;
	const ImportPlan &plan = *preview.plan;
	JsonValue out = JsonValue::make_object();
	out.set("open", boolean(preview.open));
	out.set("with_dependencies", boolean(preview.with_dependencies));
	if (preview.changed)
		out.set("changed", boolean(true));
	// The plan's importable rows (the paged list) and the rows not found, apart, in plan order.
	std::vector<const ImportPlanRow *> rows, not_found;
	for (const ImportPlanRow &row : plan.rows)
		(row.state == ImportPlanRow::State::NotFound ? not_found : rows).push_back(&row);
	// `count` the rows'; the page runs on while any list it covers has entries past it.
	set_page(out, page, rows.size(),
			std::max({ preview.choices.size(), preview.roots.size(), not_found.size() }));
	JsonValue planned = JsonValue::make_array();
	for (size_t i = page.first(rows.size()); i < page.last(rows.size()); ++i)
		planned.push(plan_row_to_json(*rows[i]));
	out.set("rows", std::move(planned));
	const auto sources_page = [&page](const std::vector<ImportSource> &sources) {
		JsonValue list = JsonValue::make_array();
		for (size_t i = page.first(sources.size()); i < page.last(sources.size()); ++i)
			list.push(import_source_to_json(sources[i]));
		return list;
	};
	out.set("choice_count", json_number(double(preview.choices.size())));
	out.set("choices", sources_page(preview.choices));
	out.set("root_count", json_number(double(preview.roots.size())));
	out.set("roots", sources_page(preview.roots));
	JsonValue missing = JsonValue::make_array();
	for (size_t i = page.first(not_found.size()); i < page.last(not_found.size()); ++i)
		missing.push(plan_row_to_json(*not_found[i]));
	out.set("not_found_count", json_number(double(not_found.size())));
	out.set("not_found", std::move(missing));
	JsonValue not_followed = JsonValue::make_array();
	for (const ImportNotFollowed &kind : plan.not_followed) {
		JsonValue entry = JsonValue::make_object();
		if (kind.reference != ReferenceKind::None)
			entry.set("reference", json_string(reference_row(kind.reference).token));
		else
			entry.set("kind", json_string(asset_kind_token(kind.kind)));
		entry.set("count", json_number(double(kind.count)));
		entry.set("first", json_string(kind.first));
		not_followed.push(std::move(entry));
	}
	out.set("not_followed", std::move(not_followed));
	out.set("truncated", boolean(plan.truncated));
	out.set("diagnostics", diagnostics_to_json(plan.diagnostics));
	return out;
}

} // namespace opennova::editor
