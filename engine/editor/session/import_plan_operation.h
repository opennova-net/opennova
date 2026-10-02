#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/import_choice.h>
#include <editor/assets/project_scan.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_plan.h>
#include <editor/model/document_base.h>
#include <editor/project/project_document.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class ProblemsService;

// An import's plan as the session's operation (ADR 0046 S13 A3): first the validation left due or
// under way joined (its remaining steps are the plan's first: ProblemsService::advance, so the
// graph it copies holds every edit made before it); then the project's files read again
// (ProjectScan, stepped: a change made outside the editor since the view's scan reaches the plan;
// no import pass, which would write), a copy of the asset graph brought up to that scan (the open
// documents standing in as they are at that step; it reads again only the files that changed), the
// game install mounted where the plan looks there for the files the chosen ones need, each of those
// a step, then the plan a step at a time (ImportPlanner: the chosen files, then each planned file's
// references within the step's bytes). It reads the project's files and writes nothing;
// finish() hands the plan to the import dialog (ImportController::absorb_plan). An import's write
// plans again with one of its own first (ImportOperation), which it steps and never finishes.
class ImportPlanOperation : public SessionOperation {
public:
	// `problems`, `graph` and `open` are the session's (the validation it joins, the problems' graph,
	// the open documents), the graph and the documents read at the step that copies the graph; the
	// session outlives the operation.
	ImportPlanOperation(ProblemsService &problems, const ProjectPaths &paths, const ProjectDocument &document,
			const AssetGraph &graph, const std::vector<std::shared_ptr<const DocumentBase>> &open,
			std::vector<ImportChoice> roots, bool with_dependencies, std::string install);

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
	enum class Phase : uint8_t { Validation, Scan, Graph, Mount, Plan, Done };

	ProblemsService &problems_;
	ProjectPaths paths_;
	ProjectDocument document_;
	const AssetGraph &graph_;
	const std::vector<std::shared_ptr<const DocumentBase>> &open_;
	std::vector<ImportChoice> roots_;
	bool with_dependencies_ = false;
	std::string install_;
	ProjectScan walk_;
	AssetScan scan_;
	std::unique_ptr<AssetGraph> copy_;
	std::shared_ptr<const ImportOrigin> mounted_;
	std::unique_ptr<ImportPlanner> planner_; // the plan under way (the Plan phase)
	uint64_t plan_files_ = 0;                // the files it knew of when it ended
	ImportPlan plan_;
	Phase phase_ = Phase::Validation;
};

} // namespace opennova::editor
