#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/node.h>
#include <editor/project/project_document.h>
#include <editor/project/project_refresh.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// A rename committed as the session's operation (ADR 0046 S13 A3, RenameApply): the rename
// transaction stepped a file at a time (each file of the sites read and rewritten in memory,
// nothing written), then its commit, the one step that writes, after which it cannot be
// cancelled; an import source's rename then reads the project's files again (ProjectRefresh: the
// import pass makes its outputs under the new name), where any other rename reads again only the
// files it touched, in its finish. It holds the scan the rename was planned over; the graph is the
// session's, which nothing moves while the operation holds the files and the documents. It keeps
// what its finish restores: the document active and the selection when it started. finish() reads
// again the open documents it rewrote and the files it touched (RenameController::absorb_rename).
class RenameOperation : public SessionOperation {
public:
	// What the modder was in when the rename started, kept through the reloads.
	struct Kept {
		std::string active;
		NodeAddress selection;
		std::vector<NodeAddress> selected;
	};

	RenameOperation(const ProjectPaths &paths, const ProjectDocument &document, std::shared_ptr<const AssetScan> scan,
			const AssetGraph &graph, RenamePlan plan, Kept kept);
	RenameOperation(const ProjectPaths &paths, const ProjectDocument &document, std::shared_ptr<const AssetScan> scan,
			const AssetGraph &graph, SymbolRenamePlan plan, Kept kept);

	OperationKind kind() const override { return OperationKind::RenameApply; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	bool cancellable() const override { return !transaction_.committed(); }
	void cancel() override {}
	OperationOutcome finish(SessionCore &core) override;

	bool symbol() const { return symbol_; }
	const RenamePlan &file_plan() const { return file_plan_; }
	const SymbolRenamePlan &symbol_plan() const { return symbol_plan_; }
	const Kept &kept() const { return kept_; }
	const RenameTransaction &transaction() const { return transaction_; }
	// The refresh an import source's rename ends with; null for any other.
	ProjectRefresh *refresh() { return refresh_.get(); }

private:
	ProjectPaths paths_;
	ProjectDocument document_;
	std::shared_ptr<const AssetScan> scan_;
	bool symbol_ = false;
	RenamePlan file_plan_;
	SymbolRenamePlan symbol_plan_;
	Kept kept_;
	RenameTransaction transaction_;
	std::unique_ptr<ProjectRefresh> refresh_;
};

} // namespace opennova::editor
