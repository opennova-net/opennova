#include <editor/session/session_operation.h>

#include <chrono>
#include <iterator>
#include <utility>

namespace opennova::editor {

namespace {

constexpr OperationKindRow kOperationKindRows[] = {
	{OperationKind::Open, "open", kHoldsAll, "Opening", "opening the project"},
	{OperationKind::Refresh, "refresh", HoldsFiles | HoldsDocuments, "Refreshing", "the refresh"},
	{OperationKind::Build, "build", HoldsFiles, "Building", "the build"},
	{OperationKind::ImportPlan, "import_plan", HoldsFiles, "Planning the import", "the import's plan"},
	{OperationKind::ImportApply, "import_apply", HoldsFiles | HoldsDocuments, "Importing", "the import"},
	{OperationKind::RenameApply, "rename_apply", HoldsFiles | HoldsDocuments, "Renaming", "the rename"},
};

static_assert(std::size(kOperationKindRows) == kOperationKindCount, "every operation kind has exactly one row");

constexpr bool operation_kind_rows_in_order() {
	for (size_t i = 0; i < kOperationKindCount; ++i)
		if (kOperationKindRows[i].kind != static_cast<OperationKind>(i)) return false;
	return true;
}
static_assert(operation_kind_rows_in_order(), "the operation kind rows follow the enum's order");

// A step budget no operation exhausts: run_to_end() steps as far as each step goes.
constexpr StepBudget kWholeStep{UINT64_MAX};

} // namespace

const OperationKindRow &operation_kind_row(OperationKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return kOperationKindRows[index < kOperationKindCount ? index : static_cast<size_t>(OperationKind::Build)];
}

std::vector<const char *> holds_tokens(Holds holds) {
	std::vector<const char *> tokens;
	if (holds_any(holds, HoldsFiles)) tokens.push_back("files");
	if (holds_any(holds, HoldsDocuments)) tokens.push_back("documents");
	if (holds_any(holds, HoldsProject)) tokens.push_back("project");
	return tokens;
}

const char *operation_unit_token(OperationUnit unit) {
	switch (unit) {
	case OperationUnit::Bytes: return "bytes";
	case OperationUnit::Files: return "files";
	case OperationUnit::Steps: return "steps";
	}
	return "steps";
}

const char *operation_end_token(OperationEnd end) {
	switch (end) {
	case OperationEnd::Done: return "done";
	case OperationEnd::Failed: return "failed";
	case OperationEnd::Cancelled: return "cancelled";
	}
	return "done";
}

int64_t steady_clock_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t OperationSlot::start(std::unique_ptr<SessionOperation> operation) {
	if (operation_ || !operation) return 0;
	operation_ = std::move(operation);
	id_ = next_id_++;
	done_ = false;
	return id_;
}

bool OperationSlot::poll(const PollBudget &budget, const OperationClock &clock) {
	if (!operation_ || done_) return done();
	const int64_t start = budget.ms > 0 ? clock() : 0;
	do {
		done_ = operation_->step(StepBudget{budget.step_bytes});
	} while (!done_ && budget.ms > 0 && clock() - start < budget.ms);
	return done_;
}

bool OperationSlot::run_to_end() {
	while (operation_ && !done_) done_ = operation_->step(kWholeStep);
	return done();
}

bool OperationSlot::finish(SessionCore &core) {
	if (!done()) return false;
	std::unique_ptr<SessionOperation> operation = std::move(operation_);
	const uint64_t id = id_;
	id_ = 0;
	done_ = false;
	OperationOutcome outcome = operation->finish(core);
	outcome.id = id;
	outcome.kind = operation->kind();
	last_ = std::move(outcome);
	return true;
}

bool OperationSlot::cancel() {
	if (!operation_ || !operation_->cancellable()) return false;
	operation_->cancel();
	last_ = OperationOutcome{id_, operation_->kind(), OperationEnd::Cancelled, {}};
	operation_.reset();
	id_ = 0;
	done_ = false;
	return true;
}

OperationStatus OperationSlot::status() const {
	OperationStatus status;
	if (!operation_) return status;
	const OperationProgress progress = operation_->progress();
	status.id = id_;
	status.kind = operation_->kind();
	status.label = progress.label;
	status.done = progress.done;
	status.total = progress.total;
	status.unit = progress.unit;
	status.cancellable = operation_->cancellable();
	status.holds = operation_->holds();
	return status;
}

} // namespace opennova::editor
