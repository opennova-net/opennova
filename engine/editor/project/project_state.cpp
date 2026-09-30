#include <editor/project/project_state.h>

namespace opennova::editor {

ProjectState refresh_project_state(const ProjectPaths &paths, const ProjectDocument &doc, bool force_import,
                                   const std::string &only) {
	ProjectState state;
	state.imports = run_imports(paths, doc, force_import, only);
	state.scan = scan_project_assets(paths, doc);
	// A record both read (a sidecar that does not parse, left as the author wrote it) is one finding.
	for (const Diagnostic &d : state.imports.diagnostics) {
		bool listed = false;
		for (const Diagnostic &row : state.scan.diagnostics)
			listed = listed || (row.severity == d.severity && row.row() == d.row() && row.asset == d.asset && row.message == d.message);
		if (!listed) state.scan.diagnostics.push_back(d);
	}
	state.requirements = evaluate_requirements(doc, state.scan);
	return state;
}

} // namespace opennova::editor
