// The page of a file the editor has no editor for (file_page.h).
#include <editor/session/file_page.h>

#include <editor/assets/asset_kind_words.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/project/project_files.h>
#include <editor/session/problem_query.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {
namespace {

// Where a build puts a file of the kind (asset_kinds.h's ArchiveSlot) [orig: PFF_OpenAllArchives @
// 0x4a4310 over the name table @ 0x829f90].
std::string build_words(AssetKind kind) {
	if (!asset_kind_packed(kind)) return "A build leaves it out: the game never asks for it.";
	switch (asset_kind_row(kind).archive_slot) {
	case ArchiveSlot::Language: return "A build packs it into language.pff.";
	case ArchiveSlot::Localres: return "A build packs it into localres.pff.";
	case ArchiveSlot::Resource: return "A build packs it into resource.pff.";
	case ArchiveSlot::Loose: return "A build copies it beside the archives, where the game reads it loose.";
	case ArchiveSlot::None: break;
	}
	return "A build leaves it out: the game never asks for it.";
}

} // namespace

FilePage file_page(const SessionView &view, const std::string &path) {
	FilePage page;
	page.path = path;
	if (!view.project.open || !view.project.scan) return page;
	const AssetEntry *entry = view.project.scan->at_path(path);
	if (!entry) entry = view.project.scan->find(path);
	if (!entry) return page;
	page.found = true;
	page.path = entry->relative_path;
	page.name = entry->logical_name;
	page.kind = asset_kind_label(entry->kind);
	page.size = entry->size_bytes;
	const AssetKindWords &words = asset_kind_words(entry->kind);
	page.what = words.what;
	page.read_by = words.read_by;
	page.cite = words.cite;
	page.build = build_words(entry->kind);
	page.editor = entry->kind == AssetKind::ImportSource
	                      ? "The editor imports it: its outputs are the project's files, opened as their kinds are."
	              : is_editable_kind(entry->kind)
	                      ? "The editor opens it as a document."
	                      : "The editor has no editor for this kind yet: it keeps the file as it is and packs it as it is.";
	if (const AssetGraph *graph = view.findings.graph.get()) {
		for (const GraphEdge *edge : graph->usages_of(page.path)) {
			const AssetEntry *source = view.project.scan->at_path(edge->source);
			FilePageLine line;
			const std::string place = edge_place_words(*edge, source ? source->kind : AssetKind::Unknown);
			line.text = (source ? source->logical_name : edge->source) + (place.empty() ? std::string() : ": " + place);
			line.target = usage_target(*view.project.scan, *edge);
			page.used_by.push_back(std::move(line));
		}
		for (const GraphEdge *edge : graph->references_of(page.path)) {
			std::string file;
			const ReferenceStatus status = graph->resolve(*edge, &file);
			if (status == ReferenceStatus::NotAReference) continue;
			FilePageLine line;
			line.text = std::string(reference_row(edge->kind).label) + " " + edge->value;
			line.missing = status == ReferenceStatus::Missing;
			if (line.missing) line.text += " (not in the project)";
			if (!file.empty()) line.target = file_target(*view.project.scan, file);
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
