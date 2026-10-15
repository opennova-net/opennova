#pragma once

#include <memory>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/import/import_plan.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/project/project_refresh.h>
#include <editor/session/import_plan_operation.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// An import written as the session's operation (ADR 0046 S13 A3). With the import dialog open, the
// import is planned again first (an ImportPlanOperation's steps, over the files as they are now),
// and nothing is written when that is not the plan the dialog showed (same_import) or a row asked
// for is not in it; then the import (AssetImport: every file checked and staged before any is
// published, a file a step), then, when it imported a file, the project's files read again
// (ProjectRefresh: the import pass makes what the files written need, the scan lists them). It can
// be cancelled until it publishes its first file (what it staged goes), not after. finish() shows
// the new plan, or closes the dialog, reads
// again the open documents whose files the import replaced and takes the refresh into the view
// (ImportController::absorb_import).
class ImportOperation : public SessionOperation {
public:
	// `replan`: the dialog's plan made again (null with no dialog open); `shown`, the plan it showed.
	ImportOperation(const ProjectPaths &paths, const ProjectDocument &document, std::vector<ImportChoice> imports,
			bool replace, std::unique_ptr<ImportPlanOperation> replan, std::shared_ptr<const ImportPlan> shown);

	OperationKind kind() const override { return OperationKind::ImportApply; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	bool cancellable() const override;
	void cancel() override;
	OperationOutcome finish(SessionCore &core) override;

	// What the import dialog showed and what planning it again made (none with no dialog open).
	bool replanned() const { return replan_ != nullptr; }
	const std::shared_ptr<const ImportPlan> &shown() const { return shown_; }
	std::shared_ptr<const ImportPlan> new_plan() const { return new_plan_; }
	// Why nothing was written: the plan changed (import.changed is the caller's to say, from the plans),
	// or a row asked for is not in it (its import.not_planned finding).
	bool changed() const { return changed_; }
	const std::vector<Diagnostic> &refusals() const { return refusals_; }
	// What it asked to import, what the import did, and the refresh after it (done when it imported a
	// file).
	const std::vector<ImportChoice> &imports() const { return imports_; }
	const ImportResult &result() const { return result_; }
	bool refreshed() const { return refresh_ != nullptr; }
	ProjectRefresh &refresh() { return *refresh_; }

private:
	enum class Phase : uint8_t { Plan, Write, Refresh, Done };

	ProjectPaths paths_;
	ProjectDocument document_;
	std::vector<ImportChoice> imports_;
	bool replace_ = false;
	std::unique_ptr<ImportPlanOperation> replan_;
	std::shared_ptr<const ImportPlan> shown_;
	std::shared_ptr<const ImportPlan> new_plan_;
	bool changed_ = false;
	std::vector<Diagnostic> refusals_;
	std::unique_ptr<AssetImport> import_; // the write under way
	uint64_t write_done_ = 0, write_total_ = 0; // its files once it ended
	ImportResult result_;
	bool written_ = false;
	std::unique_ptr<ProjectRefresh> refresh_;
	Phase phase_ = Phase::Plan;
};

} // namespace opennova::editor
