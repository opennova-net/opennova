#pragma once

#include <string>

#include <editor/import/import_pass.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>
#include <editor/project/project_refresh.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// The project's files read again as the session's operation (ADR 0046 S13 A3): a Rescan, and a
// Reimport, whose import pass imports again, even unchanged, the sources `only` names when `force`
// says so (every one when it is empty). ProjectRefresh stepped by bytes within the poll's budget:
// the import pass, the scan, the requirements. Nothing reaches the view before finish(), which
// takes the refresh into it (SessionCore::absorb_refresh) and leaves the validation due; a
// Reimport's outcome carries the pass's findings on the sources it asked for, and the status says
// how many it imported.
class RefreshOperation : public SessionOperation {
public:
	RefreshOperation(const ProjectPaths &paths, const ProjectDocument &document, bool reimport, bool force,
			std::string only);

	OperationKind kind() const override { return OperationKind::Refresh; }
	bool step(const StepBudget &budget) override { return refresh_.step(budget.bytes); }
	OperationProgress progress() const override;
	void cancel() override {}
	OperationOutcome finish(SessionCore &core) override;

private:
	bool reimport_ = false;
	std::string only_;
	ProjectRefresh refresh_;
};

// What a program changed of the watched files refreshed alone (S18's external round trip, ExternalChanges):
// the import pass over the sources it names (ImportPass::limit_to), stepped by bytes; no walk of the
// project. finish() takes it into the view (SessionCore::absorb_changed: the scan updated for those
// sources and the files alone).
class ChangedSourcesOperation : public SessionOperation {
public:
	ChangedSourcesOperation(const ProjectPaths &paths, const ProjectDocument &document, ExternalChanges changes);

	OperationKind kind() const override { return OperationKind::Refresh; }
	bool step(const StepBudget &budget) override { return pass_.step(budget.bytes); }
	OperationProgress progress() const override;
	void cancel() override {}
	OperationOutcome finish(SessionCore &core) override;

private:
	ExternalChanges changes_;
	ImportPass pass_;
};

} // namespace opennova::editor
