#include <editor/session/play_log.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <utility>

#include <base/gameprofile/graphics_log.h>
#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_edge.h>
#include <editor/graph/reference_kinds.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirement_words.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

bool same_name(const std::string &a, const std::string &b) {
	return pff::normalized_logical_name(basename_of(a)) == pff::normalized_logical_name(basename_of(b));
}

std::string capitalized(std::string text) {
	if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
	return text;
}

// The words a line said of what the game does without the name, as a clause.
std::string clause(const std::string &words) {
	return words.empty() ? std::string(".") : ": " + words + ".";
}

// Whether a reference of `edge`'s kind names `name` as the game looked it up: of a file kind, one of the
// names its loader opens (reference_file_candidates, asked as if `name` were there) or the name as written;
// of a symbol, the name as the kind compares it.
bool edge_names(const AssetGraph &graph, const GraphEdge &edge, const std::string &name) {
	const ReferenceKindRow &row = reference_row(edge.kind);
	if (row.resolution == ReferenceResolution::File) {
		const std::string value = graph.resolve_style(edge.value);
		if (same_name(value, name) || same_name(edge.target, name)) return true;
		for (const std::string &candidate :
		     reference_file_candidates(edge.kind, value, edge.loader_arg, [&](const std::string &file) { return same_name(file, name); }))
			if (same_name(candidate, name)) return true;
		return false;
	}
	if (!row.names_symbol() || row.resolution == ReferenceResolution::Record) return false;
	if (row.name_case == NameCase::Exact) return edge.value == name || edge.target == name;
	return strutil::iequals(edge.value, name) || strutil::iequals(edge.target, name);
}

// A row of `code` placed on `edge` (its file, record, field and text span), about what it names as the
// graph's missing finding words it (its subject).
Diagnostic on_edge(const AssetGraph &graph, const GraphEdge &edge, CoreFinding code, DiagnosticSeverity severity,
                   std::string message) {
	Diagnostic d = make_finding(code, severity, std::move(message), edge.source, edge.field);
	d.record = edge.record;
	d.record_key = edge.record_key;
	d.record_title = edge.record_title;
	d.line = edge.span.line;
	d.column = edge.span.column;
	d.row_id = edge.address.row;
	d.child_id = edge.address.child;
	d.record_kind = edge.address.kind;
	d.subject = graph.missing_finding(edge).subject;
	return d;
}

// "'Wooden barrel' in items.def names the model 'onbarrel'"
std::string who_names(const GraphEdge &edge) {
	const std::string who = edge.record.empty() ? edge.source : "'" + edge.record + "' in " + edge.source;
	return who + " names " + reference_row(edge.kind).phrase + " '" + edge.value + "'";
}

void add_once(std::vector<Diagnostic> &rows, Diagnostic d) {
	if (std::find(rows.begin(), rows.end(), d) == rows.end()) rows.push_back(std::move(d));
}

// A row about a file the game opens by its own name: the manifest row's role and name where it has one
// (the requirement's fixes), what the game does without it in its words; an Info for one the game goes on
// without (an optional row, a name it has no row of).
Diagnostic file_row(const std::string &name, std::string message, const std::string &asset) {
	const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_find(name.c_str());
	const std::string without = row ? requirement_without(row->role) : std::string();
	if (!without.empty()) message += " " + without;
	const DiagnosticSeverity severity = row && row->severity != gameprofile::RES_OPTIONAL ? DiagnosticSeverity::Warning
	                                                                                      : DiagnosticSeverity::Info;
	Diagnostic d = make_finding(CoreFinding::PlayFileMissing, severity, std::move(message), asset);
	d.subject = RequirementSubject{row ? std::string(row->role) : std::string(), row ? std::string(row->name) : name};
	return d;
}

} // namespace

std::vector<Diagnostic> resource_miss_findings(const gameprofile::ResourceMiss &miss, const PlayGame &game,
                                               const SessionView &view) {
	std::vector<Diagnostic> rows;
	if (!view.project.open || !view.project.scan || miss.name.empty()) return rows;
	const AssetGraph *graph = view.findings.graph.get();
	ReferenceKind kind = ReferenceKind::None;
	const bool reference = miss.kind != gameprofile::resource_kind::kFile && reference_kind_from_token(miss.kind, kind) &&
	                       kind != ReferenceKind::None;
	// The file that named it, as the project has it (a line names it by its file name).
	const AssetEntry *by = miss.by.empty() ? nullptr : view.project.scan->find(basename_of(miss.by));
	const std::string by_path = by ? by->relative_path : std::string();
	if (reference && graph) {
		const auto visit = [&](const GraphEdge &edge) {
			if (edge.kind != kind || !edge_names(*graph, edge, miss.name)) return;
			if (by && !same_name(edge.source, by->relative_path)) return;
			add_once(rows, on_edge(*graph, edge, CoreFinding::PlayReferenceMissing, DiagnosticSeverity::Warning,
			                       who_names(edge) + ", which " + game.name + " could not find in its last Play" +
			                               clause(miss.words)));
		};
		// A name the project lacks is named by the graph's missing edges alone (kept as it resolves them); one
		// the project has (the game did not find it all the same) by any edge, every one looked at.
		for (const GraphEdge *edge : graph->missing()) visit(*edge);
		if (rows.empty()) graph->for_each_edge(visit);
		if (!rows.empty()) return rows;
	}
	const std::string named_by = by ? ", which " + by->logical_name + " names," : std::string();
	if (!reference || gameprofile::gameprofile_required_resource_find(miss.name.c_str())) {
		rows.push_back(file_row(miss.name,
		                        capitalized(game.name) + " looked for " + miss.name + named_by + " in its last Play and did not find it" +
		                                clause(miss.words),
		                        by_path));
		return rows;
	}
	// A name no file of the project names (one the engine looks up by itself, or one whose file names it in
	// a way the graph does not read): the name alone, its fixes a missing reference's.
	const ReferenceKindRow &row = reference_row(kind);
	Diagnostic d = make_finding(CoreFinding::PlayReferenceMissing, DiagnosticSeverity::Warning,
	                            capitalized(game.name) + " looked for " + row.phrase + " '" + miss.name + "'" + named_by +
	                                    " in its last Play and did not find it" + clause(miss.words),
	                            by_path);
	d.subject = ReferenceSubject{kind, miss.name, std::string(), -1};
	rows.push_back(std::move(d));
	return rows;
}

std::vector<Diagnostic> install_log_findings(const InstallLogs &logs, const PlayGame &game, const SessionView &view) {
	std::vector<Diagnostic> rows;
	if (!view.project.open || !view.project.scan) return rows;
	const AssetScan &scan = *view.project.scan;
	const std::string named = capitalized(game.name);
	std::set<std::string> opened;
	if (logs.file_log) {
		for (const std::string &name : logs.file_log->from_archives) opened.insert(pff::normalized_logical_name(basename_of(name)));
		for (const std::string &name : logs.file_log->from_disk) opened.insert(pff::normalized_logical_name(basename_of(name)));
	}
	const auto was_opened = [&](const std::string &name) { return opened.count(pff::normalized_logical_name(basename_of(name))) != 0; };
	if (logs.exited_on_its_own && logs.file_log && !logs.file_log->archives.empty()) {
		// The boot's text tables, right after the archives, in their order (gameprofile::kBootTextTables):
		// gameerr.bin's lack shows earlyerr.txt's line 4 and the boot goes on (its row's RES_DIALOG); any
		// other's ends it.
		for (const char *name : gameprofile::kBootTextTables) {
			if (was_opened(name)) continue;
			const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_find(name);
			const bool dialog = row && row->severity == gameprofile::RES_DIALOG;
			rows.push_back(file_row(name,
			                        named + " opened its archives but not " + name + " in its last Play" +
			                                (dialog ? ": it showed earlyerr.txt's line 4 and went on." : ", and stopped there."),
			                        std::string()));
			if (!dialog) break;
		}
	}
	if (logs.exited_on_its_own && !logs.file_log) {
		// No archive opened: the early error dialog of earlyerr.txt's line 3, then the exit.
		const AssetEntry *early = scan.find("earlyerr.txt");
		Diagnostic d = make_finding(CoreFinding::PlayFileMissing, DiagnosticSeverity::Warning,
		                            named + " opened none of its archives in its last Play (it left no file log): it shows "
		                                   "earlyerr.txt's line 3 and quits.",
		                            early ? early->relative_path : std::string());
		if (early) d.line = 3;
		rows.push_back(std::move(d));
	}
	// What the files the game read name that the project does not have.
	if (const AssetGraph *graph = view.findings.graph.get(); graph && !opened.empty())
		for (const GraphEdge *edge : graph->missing()) {
			if (!was_opened(edge->source)) continue;
			if (graph->missing_finding(*edge).row() != &finding_code(CoreFinding::ReferenceMissing)) continue;
			add_once(rows, on_edge(*graph, *edge, CoreFinding::PlayReferenceMissing, DiagnosticSeverity::Warning,
			                       who_names(*edge) + ": " + game.name + " read " + basename_of(edge->source) +
			                               " in its last Play, and the build holds no " + reference_row(edge->kind).label +
			                               " of that name."));
		}
	for (const gameprofile::GraphicsLogMission &mission : gameprofile::graphics_log_missions(logs.graphics_log)) {
		if (mission.complete) continue;
		const AssetEntry *file = scan.find(basename_of(mission.file));
		add_once(rows, make_finding(CoreFinding::PlayMissionUnfinished, DiagnosticSeverity::Warning,
		                            named + " began loading " + (file ? file->logical_name : mission.file) +
		                                    " in its last Play and never finished (its " + gameprofile::kGraphicsLogName +
		                                    " has no \"Mission loading complete\" after it): it crashed, hung or was "
		                                    "stopped while the mission loaded.",
		                            file ? file->relative_path : std::string()));
	}
	return rows;
}

} // namespace opennova::editor
