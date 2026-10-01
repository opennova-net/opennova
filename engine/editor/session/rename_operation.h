#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/diagnostic.h>
#include <editor/model/node.h>
#include <editor/project/project_document.h>
#include <editor/project/project_refresh.h>
#include <editor/session/selection.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class ProblemsService;

// A rename committed as the session's operation (ADR 0046 S13 A3, RenameApply). First the
// validation left due or under way is joined (its remaining steps are the rename's first:
// ProblemsService::advance), so the graph the plan reads holds every edit saved before it; then
// the plan (the planner the request gave, over the scan and the graph as they are then: one
// refused ends the operation failed, nothing written, its refusals what it came to); then the
// rename transaction stepped a file at a time (each file of the sites read and rewritten in
// memory, nothing written), then its commit, the one step that writes, after which it cannot be
// cancelled; an import source's rename then reads the project's files again (ProjectRefresh: the
// import pass makes its outputs under the new name), where any other rename reads again only the
// files it touched, in its finish. The transaction commits what it staged: nothing writes the
// project's files or the open documents while the operation holds them, so a validation stepping
// between two of its steps brings the graph to the same files and moves no site it staged. It
// keeps what its finish restores: the document active and the selection when it started.
// finish() reads again the open documents it rewrote and the files it touched
// (RenameController::absorb_rename).
class RenameOperation : public SessionOperation {
public:
	// What the modder was in when the rename started, kept through the reloads.
	struct Kept {
		std::string active;
		Selection selection;
	};
	// The plan made once the validation the rename joins has ended: the scan it was made over (the
	// view's then) set beside it.
	using FilePlanner = std::function<RenamePlan(std::shared_ptr<const AssetScan> &scan)>;
	using SymbolPlanner = std::function<SymbolRenamePlan(std::shared_ptr<const AssetScan> &scan)>;

	RenameOperation(ProblemsService &problems, const ProjectPaths &paths, const ProjectDocument &document,
			const AssetGraph &graph, FilePlanner planner, Kept kept);
	RenameOperation(ProblemsService &problems, const ProjectPaths &paths, const ProjectDocument &document,
			const AssetGraph &graph, SymbolPlanner planner, Kept kept);
	~RenameOperation() override;

	OperationKind kind() const override { return OperationKind::RenameApply; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	bool cancellable() const override { return !transaction_ || !transaction_->committed(); }
	void cancel() override {}
	OperationOutcome finish(SessionCore &core) override;

	bool symbol() const { return symbol_; }
	const RenamePlan &file_plan() const { return file_plan_; }
	const SymbolRenamePlan &symbol_plan() const { return symbol_plan_; }
	// The plan's refusals when it was refused (nothing written then, no transaction made).
	const std::vector<Diagnostic> &refusals() const { return symbol_ ? symbol_plan_.refusals : file_plan_.refusals; }
	bool refused() const { return phase_ == Phase::Done && !transaction_; }
	const Kept &kept() const { return kept_; }
	// The transaction, made once the plan was (null before it, or for a plan refused).
	const RenameTransaction *transaction() const { return transaction_.get(); }
	// The refresh an import source's rename ends with; null for any other.
	ProjectRefresh *refresh() { return refresh_.get(); }

private:
	enum class Phase : uint8_t { Validation, Plan, Stage, Refresh, Done };

	ProblemsService &problems_;
	ProjectPaths paths_;
	ProjectDocument document_;
	const AssetGraph &graph_;
	bool symbol_ = false;
	FilePlanner file_planner_;
	SymbolPlanner symbol_planner_;
	std::shared_ptr<const AssetScan> scan_;
	RenamePlan file_plan_;
	SymbolRenamePlan symbol_plan_;
	Kept kept_;
	std::unique_ptr<RenameTransaction> transaction_;
	std::unique_ptr<ProjectRefresh> refresh_;
	Phase phase_ = Phase::Validation;
};

} // namespace opennova::editor
