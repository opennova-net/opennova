#pragma once

#include <string>

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

} // namespace opennova::editor
