// The page of a file the editor has no editor for (file_page.h).
#include <editor/session/file_page.h>

#include <editor/assets/asset_kind_words.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/documents/name_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// The place a Go to names a record of the file by (usage_target, symbol_target): its locator, else its
// path as the graph names it where the file keeps no places.
const std::string &place_of(const std::string &locator, const std::string &record) {
	return locator.empty() ? record : locator;
}

// Whether a record of the file is the one the page's Go to names: its place, and the field where the Go to
// names one (a record named alone marks each of its lines; the file alone, none).
bool marked(const FilePage &page, const std::string &place, const std::string &field) {
	if (page.at_locator.empty() && page.at_field.empty()) return false;
	return place == page.at_locator && (page.at_field.empty() || field.empty() || field == page.at_field);
}

// A use as the page lists it: the naming file, the record in its words, where a click goes.
FilePageLine use_line(const SessionView &view, const GraphEdge &edge) {
	const AssetEntry *source = view.project.scan->at_path(edge.source);
	FilePageLine line;
	const std::string place = edge_place_words(edge, source ? source->kind : AssetKind::Unknown);
	line.text = (source ? source->logical_name : edge.source) + (place.empty() ? std::string() : ": " + place);
	line.target = usage_target(*view.project.scan, edge);
	return line;
}

} // namespace

FilePage shown_file_page(const SessionView &view, const std::string &path) {
	const bool shown = !path.empty() && path == view.documents.page;
	return shown ? file_page(view, path, view.documents.page_locator, view.documents.page_field) : file_page(view, path);
}

FilePage file_page(const SessionView &view, const std::string &path, const std::string &locator, const std::string &field) {
	FilePage page;
	page.path = path;
	page.at_locator = locator;
	page.at_field = field;
	if (!view.project.open || !view.project.scan) return page;
	const AssetEntry *entry = view.project.scan->at_path(path);
	if (!entry) entry = view.project.scan->find(path);
	if (!entry) return page;
	page.found = true;
	page.path = entry->relative_path;
	page.name = entry->logical_name;
	page.kind = asset_kind_label(entry->kind);
	page.size = entry->size_bytes;
	page.wave = entry->kind == AssetKind::Wave;
	const AssetKindWords &words = asset_kind_words(entry->kind);
	page.what = words.what;
	page.read_by = words.read_by;
	page.cite = words.cite;
	// Where a build puts it, from the decision the build plan makes (a player's file, an archive, a name no
	// archive stores among them), and what the editor does with it.
	const BuildPlaceWords place =
	        build_place_words(*entry, view.project.document ? view.project.document->expansion.name : std::string());
	page.build = place.words;
	const bool packed = place.packed;
	page.editor = entry->kind == AssetKind::ImportSource
	                      ? "The editor imports it: its outputs are the project's files, opened as their kinds are."
	              : is_editable_kind(entry->kind) ? "The editor opens it as a document."
	              : packed ? "The editor has no editor for this kind yet: it keeps the file as it is, and a build packs it as it is."
	                       : "The editor has no editor for this kind yet: it keeps the file as it is.";
	if (const AssetGraph *graph = view.findings.graph.get()) {
		// What it defines (DI-17), each name with who names it, in the words the project's names give it.
		const GraphNameSource names(*graph);
		for (const FileDefinition &definition : file_definitions(*graph, page.path)) {
			const GraphSymbol &symbol = *definition.symbol;
			FilePageDefinition defined;
			defined.text = std::string(reference_row(symbol.kind).label) + " " + symbol.display;
			const std::string words = definition_words(symbol, &names);
			if (!words.empty() && words != symbol.display) defined.text += ": " + words;
			defined.read = definition.read;
			if (!definition.read) defined.unread = symbol.inert_reason.empty() ? "no lookup of the game finds it" : symbol.inert_reason;
			defined.at = marked(page, place_of(symbol.locator, symbol.record), symbol.field);
			for (const GraphEdge *edge : definition.users) defined.users.push_back(use_line(view, *edge));
			page.defines.push_back(std::move(defined));
		}
		for (const GraphEdge *edge : graph->usages_of(page.path)) page.used_by.push_back(use_line(view, *edge));
		for (const GraphEdge *edge : graph->references_of(page.path)) {
			std::string file;
			const ReferenceStatus status = graph->resolve(*edge, &file);
			if (status == ReferenceStatus::NotAReference) continue;
			FilePageLine line;
			// Where in the file it is named (DI-17: the line a Go to marks reads which record it is), its
			// record, else its field.
			const std::string &where = edge->record.empty() ? edge->field : edge->record;
			line.text = (where.empty() ? std::string() : where + ": ") + reference_row(edge->kind).label + " " + edge->value;
			line.missing = status == ReferenceStatus::Missing;
			line.at = marked(page, place_of(edge->locator, edge->record), edge->field);
			if (!file.empty()) line.target = file_target(*view.project.scan, file);
			if (line.missing) {
				// A name nothing resolves: a symbol's Go to lands where it belongs; a file's in Problems, its fixes.
				line.text += " (not in the project)";
				line.name = edge->value;
				missing_target(ReferenceSubject{edge->kind, edge->value, graph->lookup_scope(*edge), edge->loader_arg}, view,
				               line.target);
			}
			page.names.push_back(std::move(line));
		}
	}
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.asset != page.path) continue;
		if (d.severity == DiagnosticSeverity::Error) ++page.errors;
		if (d.severity == DiagnosticSeverity::Warning) ++page.warnings;
	}
	return page;
}

} // namespace opennova::editor
