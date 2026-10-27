#include <editor/session/novaworld_operation.h>

#include <algorithm>

#include <editor/session/novaworld_controller.h>

namespace opennova::editor {

bool NovaWorldOperation::step(const StepBudget &budget) {
	return job_->step(std::max<uint64_t>(budget.bytes, 1)) != NovaWorldJobState::Running;
}

OperationProgress NovaWorldOperation::progress() const {
	const NovaWorldJobProgress p = job_->progress();
	OperationProgress out;
	out.label = p.label;
	out.total = p.total;
	out.done = std::min(p.done, p.total);
	out.unit = p.total == 0 ? OperationUnit::Steps
			: p.bytes		? OperationUnit::Bytes
							: OperationUnit::Files;
	return out;
}

OperationOutcome NovaWorldOperation::finish(SessionCore &) {
	return controller_.finish_job(kind_, std::move(job_));
}

} // namespace opennova::editor
