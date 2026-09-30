#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/import_source.h>
#include <editor/assets/project_scan.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_plan.h>
#include <editor/model/document_base.h>
#include <editor/project/project_document.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// An import's plan as the session's operation (ADR 0046 S13 A3): the project's files read again
// (ProjectScan, stepped: a change made outside the editor since the view's scan reaches the plan;
// no import pass, which would write), a copy of the asset graph brought up to that scan (the open
// documents standing in as they are at that step; its cache reads again only the files that
// changed), the game install mounted where the plan looks there for the files the chosen ones
// need, then the plan (plan_import), each of those three a step. It reads the project's files and
// writes nothing; finish() hands the plan to the import dialog (ImportController::absorb_plan). An
// import's write plans again with one of its own first (ImportOperation), which it steps and never
// finishes.
class ImportPlanOperation : public SessionOperation {
public:
	// `graph` and `open` are the session's (the problems' graph, the open documents), read at the
	// step that copies the graph; the session outlives the operation.
	ImportPlanOperation(const ProjectPaths &paths, const ProjectDocument &document, const AssetGraph &graph,
			const std::vector<std::shared_ptr<const DocumentBase>> &open, std::vector<ImportSource> roots,
			bool with_dependencies, std::string install);

	OperationKind kind() const override { return OperationKind::ImportPlan; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	void cancel() override {}
	OperationOutcome finish(SessionCore &core) override;

	// The plan once done, and the scan it was made over.
	ImportPlan &plan() { return plan_; }
	const AssetScan &scan() const { return scan_; }
	bool done() const { return phase_ == Phase::Done; }

private:
	enum class Phase : uint8_t { Scan, Graph, Mount, Plan, Done };

	ProjectPaths paths_;
	ProjectDocument document_;
	const AssetGraph &graph_;
	const std::vector<std::shared_ptr<const DocumentBase>> &open_;
	std::vector<ImportSource> roots_;
	bool with_dependencies_ = false;
	std::string install_;
	ProjectScan walk_;
	AssetScan scan_;
	std::unique_ptr<AssetGraph> copy_;
	std::shared_ptr<const ImportOrigin> mounted_;
	ImportPlan plan_;
	Phase phase_ = Phase::Scan;
};

} // namespace opennova::editor
