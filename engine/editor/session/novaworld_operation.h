#pragma once

// A NovaWorld job as the session's operation (ADR 0069; session/novaworld_controller.h): a check,
// a bind, a pull or a push, stepped within the poll's budget in the one slot, so the busy gate
// weighs what it reads and writes as its kind's row says (session_operation.cpp: a pull writes the
// files and the documents, a check and a push read the files). It is done once the job is (done,
// failed, cancelled, or a pull stopped for its author); its finish hands the job to the controller,
// which takes in what it came to.

#include <memory>

#include <editor/novaworld/novaworld_jobs.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class NovaWorldController;

class NovaWorldOperation : public SessionOperation {
public:
	NovaWorldOperation(
			OperationKind kind, NovaWorldController &controller, std::unique_ptr<NovaWorldJob> job) :
			kind_(kind), controller_(controller), job_(std::move(job)) {}

	OperationKind kind() const override { return kind_; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	void cancel() override { job_->cancel(); }
	OperationOutcome finish(SessionCore &core) override;

private:
	OperationKind kind_;
	NovaWorldController &controller_;
	std::unique_ptr<NovaWorldJob> job_;
};

} // namespace opennova::editor
