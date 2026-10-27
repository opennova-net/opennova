#include <editor/session/session_operation.h>
#include <editor/session/row_table.h>

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
	// A check and a push read the files (a save waits), a bind reads which project is open (its binding goes into
	// the project's .opennova/novaworld/, so a close waits or cancels it), and a pull writes the files and the
	// documents read again, as an import's write; each gives way to novaworld_cancel.
	{OperationKind::NovaWorldCheck, "novaworld_check", HoldsFiles, HoldsSlot, {}, {K::NovaWorldCancel}, "Checking",
	 "the NovaWorld check"},
	{OperationKind::NovaWorldBind, "novaworld_bind", HoldsProject, HoldsSlot, {}, {K::NovaWorldCancel}, "Binding",
	 "the binding"},
	{OperationKind::NovaWorldPull, "novaworld_pull", kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, {},
	 {K::NovaWorldCancel}, "Pulling", "the pull"},
	{OperationKind::NovaWorldPush, "novaworld_push", HoldsFiles, HoldsSlot, {}, {K::NovaWorldCancel}, "Pushing",
	 "the push"},
	// A base export reads the open project's setting of its base game (a settings change waits, a close cancels
	// it) and none of its files (an edit and a save go on); a Build, a Play and an Export join it, the build
	// starting once the base is in (session/base_export.h).
	{OperationKind::BaseExport, "base_export", HoldsProject, HoldsSlot, {K::Build, K::Play, K::Export}, {},
	 "Exporting the base game", "the base game's export"},
};

constexpr bool every_operation_writes_the_slot() {
	for (const OperationKindRow &row : kOperationKindRows)
		if (!holds_any(row.writes, HoldsSlot)) return false;
	return true;
}
static_assert(every_operation_writes_the_slot(), "one operation runs at a time: each writes the slot");

static_assert(std::size(kOperationKindRows) == kOperationKindCount, "every operation kind has exactly one row");

static_assert(rows_follow(kOperationKindRows, &OperationKindRow::kind), "the operation kind rows follow the enum's order");

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
	follows_ = finishing_; // started by a finish to go on with its work, or 0
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
	const uint64_t id = id_, follows = follows_;
	id_ = 0;
	follows_ = 0;
	done_ = false;
	finishing_ = follows ? follows : id; // what its finish starts goes on with the work this one went on with
	OperationOutcome outcome = operation->finish(core);
	finishing_ = 0;
	outcome.id = id;
	outcome.kind = operation->kind();
	outcome.follows = follows;
	// The operation its finish started goes on under its id (a base export's Build): the id it took is
	// never handed out again, as no id is, and a wait on this one waits on it. Any other is the one that
	// follows (then), which runs now.
	const bool hands_on = operation_ && operation->hands_on_id();
	outcome.then = operation_ && !hands_on ? id_ : 0;
	last_ = std::move(outcome);
	if (hands_on) {
		id_ = id;
		follows_ = follows;
	}
	return true;
}

bool OperationSlot::cancel() {
	if (!operation_ || !operation_->cancellable()) return false;
	operation_->cancel();
	if (operation_->finishes_on_cancel()) {
		done_ = true; // finished as the cancel is asked (SessionCore::cancel_operation)
		return true;
	}
	last_ = OperationOutcome{id_, operation_->kind(), OperationEnd::Cancelled, {}};
	last_.follows = follows_;
	operation_.reset();
	id_ = 0;
	follows_ = 0;
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
