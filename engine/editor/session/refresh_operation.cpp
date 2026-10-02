#include <editor/session/refresh_operation.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <editor/import/import_run.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

RefreshOperation::RefreshOperation(const ProjectPaths &paths, const ProjectDocument &document, bool reimport, bool force,
		std::string only) :
		reimport_(reimport), only_(std::move(only)), refresh_(paths, document, force, only_) {}

OperationProgress RefreshOperation::progress() const {
	return {refresh_.files_done(), refresh_.files_total(), OperationUnit::Files, refresh_.label()};
}

OperationOutcome RefreshOperation::finish(SessionCore &core) {
	const ImportRunResult imported = core.absorb_refresh(refresh_);
	OperationOutcome outcome;
	if (!reimport_) return outcome;
	// The pass's findings are Problems rows already (they ride the scan); the ones on the sources
	// asked for are also what the Reimport came to.
	std::vector<std::string> asked;
	for (const ImportedSource &ran : imported.sources)
		if (import_source_named(only_, ran.source)) {
			asked.push_back(ran.source);
			asked.push_back(ran.sidecar);
		}
	for (const Diagnostic &d : imported.diagnostics)
		if (std::find(asked.begin(), asked.end(), d.asset) != asked.end()) {
			outcome.findings.push_back(d);
			if (d.severity == DiagnosticSeverity::Error) outcome.end = OperationEnd::Failed;
		}
	core.view().activity.status =
			std::to_string(imported.reimported) + " source" + (imported.reimported == 1 ? "" : "s") + " imported.";
	core.touch(ViewConcern::Output);
	return outcome;
}

} // namespace opennova::editor
