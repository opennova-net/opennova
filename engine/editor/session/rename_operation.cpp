#include <editor/session/rename_operation.h>

#include <utility>

#include <editor/session/rename_controller.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

RenameOperation::RenameOperation(const ProjectPaths &paths, const ProjectDocument &document,
		std::shared_ptr<const AssetScan> scan, const AssetGraph &graph, RenamePlan plan, Kept kept) :
		paths_(paths),
		document_(document),
		scan_(std::move(scan)),
		file_plan_(std::move(plan)),
		kept_(std::move(kept)),
		transaction_(paths_, document_, *scan_, graph, file_plan_) {}

RenameOperation::RenameOperation(const ProjectPaths &paths, const ProjectDocument &document,
		std::shared_ptr<const AssetScan> scan, const AssetGraph &graph, SymbolRenamePlan plan, Kept kept) :
		paths_(paths),
		document_(document),
		scan_(std::move(scan)),
		symbol_(true),
		symbol_plan_(std::move(plan)),
		kept_(std::move(kept)),
		transaction_(paths_, document_, *scan_, graph, symbol_plan_) {}

bool RenameOperation::step(const StepBudget &budget) {
	if (!transaction_.committed()) {
		if (!transaction_.step()) return false;
		// An import source's rename: the import pass makes its outputs under the new name, and the
		// scan lists them (a rename that did not commit wrote nothing to read again).
		if (!symbol_ && transaction_.ok() && !file_plan_.sidecar.empty())
			refresh_ = std::make_unique<ProjectRefresh>(paths_, document_);
		return refresh_ == nullptr;
	}
	return !refresh_ || refresh_->step(budget.bytes);
}

OperationProgress RenameOperation::progress() const {
	OperationProgress progress;
	progress.unit = OperationUnit::Files;
	// The files staged and the commit, then the refresh's.
	progress.done = transaction_.files_staged() + (transaction_.committed() ? 1 : 0) + (refresh_ ? refresh_->files_done() : 0);
	progress.total = transaction_.files_total() + 1 + (refresh_ ? refresh_->files_total() : 0);
	progress.label = refresh_                       ? refresh_->label()
	        : transaction_.files_staged() < transaction_.files_total() ? "Rewriting " + transaction_.current()
	                                                                   : "Writing the renamed files";
	return progress;
}

OperationOutcome RenameOperation::finish(SessionCore &core) {
	return core.renames().absorb_rename(*this);
}

} // namespace opennova::editor
