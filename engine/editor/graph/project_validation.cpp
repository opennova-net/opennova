#include <editor/graph/project_validation.h>

#include <array>
#include <cstddef>

#include <editor/assets/asset_kinds.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/use_checks.h>

namespace opennova::editor {

std::vector<const AssetEntry *> validation_files(const AssetScan &scan) {
	// Each type's files in the scan's order, then the types in the registry's (DocumentTypeId's).
	std::array<std::vector<const AssetEntry *>, kDocumentTypeCount + 1> by_type;
	for (const AssetEntry &asset : scan.entries) {
		const size_t type = static_cast<size_t>(asset_kind_row(asset.kind).document);
		if (type != 0 && type < by_type.size())
			by_type[type].push_back(&asset);
	}
	std::vector<const AssetEntry *> out;
	for (const std::vector<const AssetEntry *> &files : by_type)
		out.insert(out.end(), files.begin(), files.end());
	return out;
}

std::vector<Diagnostic> validate_project(
		const ValidationInput &input, AssetGraph &graph, ValidationCache &cache) {
	std::vector<Diagnostic> out;
	// The graph first: the use checks read what the files define and who uses it.
	graph.update(input.paths, input.project, input.scan, input.open);
	cache.begin();
	for (const AssetEntry *asset : validation_files(input.scan)) {
		const std::vector<Diagnostic> &own = cache.file_findings(input, *asset);
		out.insert(out.end(), own.begin(), own.end());
	}
	run_use_checks(graph, cache, out);
	cache.end();
	const std::vector<Diagnostic> &references = graph.diagnostics();
	out.insert(out.end(), references.begin(), references.end());
	return out;
}

} // namespace opennova::editor
