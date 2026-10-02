#include <editor/session/rename_operation.h>

#include <utility>

#include <editor/session/problems_service.h>
#include <editor/session/rename_controller.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

RenameOperation::RenameOperation(ProblemsService &problems, const ProjectPaths &paths, const ProjectDocument &document,
		const AssetGraph &graph, FilePlanner planner, Kept kept) :
		problems_(problems),
		paths_(paths),
		document_(document),
		graph_(graph),
		file_planner_(std::move(planner)),
		kept_(std::move(kept)) {}

RenameOperation::RenameOperation(ProblemsService &problems, const ProjectPaths &paths, const ProjectDocument &document,
		const AssetGraph &graph, SymbolPlanner planner, Kept kept) :
		problems_(problems),
		paths_(paths),
		document_(document),
		graph_(graph),
		symbol_(true),
		symbol_planner_(std::move(planner)),
		kept_(std::move(kept)) {}

RenameOperation::~RenameOperation() = default;

bool RenameOperation::step(const StepBudget &budget) {
	switch (phase_) {
	case Phase::Validation:
		// The validation left due, joined: the plan reads the graph, which then holds every edit the
		// documents were saved with.
		if (problems_.advance(budget.bytes)) phase_ = Phase::Plan;
		return false;
	case Phase::Plan: {
		const bool ok = symbol_ ? (symbol_plan_ = symbol_planner_(scan_)).ok() : (file_plan_ = file_planner_(scan_)).ok();
		if (!ok || !scan_) {
			phase_ = Phase::Done; // refused: nothing written
			return true;
		}
		transaction_ = symbol_ ? std::make_unique<RenameTransaction>(paths_, document_, *scan_, graph_, symbol_plan_)
		                       : std::make_unique<RenameTransaction>(paths_, document_, *scan_, graph_, file_plan_);
		phase_ = Phase::Stage;
		return false;
	}
	case Phase::Stage:
		if (!transaction_->step()) return false;
		// An import source's rename: the import pass makes its outputs under the new name, and the
		// scan lists them (a rename that did not commit wrote nothing to read again).
		if (!symbol_ && transaction_->ok() && !file_plan_.sidecar.empty()) {
			refresh_ = std::make_unique<ProjectRefresh>(paths_, document_);
			phase_ = Phase::Refresh;
			return false;
		}
		phase_ = Phase::Done;
		return true;
	case Phase::Refresh:
		if (!refresh_->step(budget.bytes)) return false;
		phase_ = Phase::Done;
		return true;
	case Phase::Done: return true;
	}
	return true;
}

OperationProgress RenameOperation::progress() const {
	OperationProgress progress;
	progress.unit = OperationUnit::Files;
	if (!transaction_) {
		progress.label = phase_ == Phase::Validation ? "Validating the project first" : "Planning the rename";
		return progress;
	}
	// The files staged and the commit, then the refresh's.
	progress.done = transaction_->files_staged() + (transaction_->committed() ? 1 : 0) +
	                (refresh_ ? refresh_->files_done() : 0);
	progress.total = transaction_->files_total() + 1 + (refresh_ ? refresh_->files_total() : 0);
	progress.label = refresh_ ? refresh_->label()
	        : transaction_->files_staged() < transaction_->files_total() ? "Rewriting " + transaction_->current()
	                                                                     : "Writing the renamed files";
	return progress;
}

OperationOutcome RenameOperation::finish(SessionCore &core) {
	return core.renames().absorb_rename(*this);
}

} // namespace opennova::editor
