#include <editor/session/session_operation.h>

#include <chrono>
#include <iterator>
#include <utility>

namespace opennova::editor {

namespace {

using K = EditorRequestKind;

constexpr Holds kFilesAndDocuments = HoldsFiles | HoldsDocuments;

// What each operation reads and writes (every one writes the slot: one runs at a time), and the
// requests it serves or gives way to while it runs. A build reads the project's files and writes
// only its own output, so what reads the files and starts no operation (a rename's plan, an open)
// goes on beside it and what writes them waits. Opening a project holds everything: a request that
// reads or writes anything of it waits, and a project switch or Quit cancels it. A refresh, an
// import's write and a rename's commit read and write the files and the open documents (the import
// pass writes the imported outputs, a finish reads documents again): an edit waits. An import's plan
// reads the files: an edit goes on, a save waits, and a new plan (or the dialog's Cancel) takes its
// place; the dialog's Cancel stops an import's write too, before it writes (after, it is refused
// until the import ends).
constexpr OperationKindRow kOperationKindRows[] = {
	{OperationKind::Open, "open", kHoldsAll, kHoldsAll | HoldsSlot, {}, {}, "Opening", "opening the project"},
	{OperationKind::Refresh, "refresh", kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, {}, {}, "Refreshing",
	 "the refresh"},
	{OperationKind::Build, "build", HoldsFiles, HoldsSlot, {K::Build, K::Play, K::Export}, {}, "Building", "the build"},
	{OperationKind::ImportPlan, "import_plan", HoldsFiles, HoldsSlot, {},
	 {K::PreviewImport, K::PlanImport, K::PreviewInstallImport, K::CancelImport},
	 "Planning the import", "the import's plan"},
	{OperationKind::ImportApply, "import_apply", kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, {},
	 {K::CancelImport}, "Importing", "the import"},
	{OperationKind::RenameApply, "rename_apply", kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, {}, {},
	 "Renaming", "the rename"},
};

constexpr bool every_operation_writes_the_slot() {
	for (const OperationKindRow &row : kOperationKindRows)
		if (!holds_any(row.writes, HoldsSlot)) return false;
	return true;
}
static_assert(every_operation_writes_the_slot(), "one operation runs at a time: each writes the slot");

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
	if (holds_any(holds, HoldsSlot)) tokens.push_back("slot");
	return tokens;
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
	status.reads = operation_->reads();
	status.writes = operation_->writes();
	return status;
}

} // namespace opennova::editor
