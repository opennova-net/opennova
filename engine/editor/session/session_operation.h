#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class ProjectSession;
// What an operation's finish writes through. The session is one class today; S13 A2 gives its
// state a class of its own (SessionCore) and this alias goes.
using SessionCore = ProjectSession;

// The long jobs the session runs one at a time, a step at a time, so the window that hosts it
// keeps drawing (ADR 0046 S13 A1). Build has a body; the other kinds are rows already, so the
// slices that move them onto the slot add bodies, not shapes.
enum class OperationKind : uint8_t {
	Open,        // a project opened: the install's names, the import pass, the scan, the requirements
	Refresh,     // the files read again: a Rescan, a Reimport, the refresh after an import
	Build,       // the project packed into an immutable build directory (project_build/build_run.h)
	ImportPlan,  // an import's plan: the scan, the graph, the install
	ImportApply, // an import written
	RenameApply, // a rename's files rewritten
	kCount,
};

inline constexpr size_t kOperationKindCount = static_cast<size_t>(OperationKind::kCount);

// The session's resources an operation and a request read or write (request_kinds.h, the
// session's busy gate): a request conflicts with the running operation when it writes what the
// operation reads or writes, or reads what the operation writes. Bit flags.
enum Holds : uint8_t {
	HoldsNothing = 0,
	HoldsFiles = 1 << 0,     // the project's files on disk: a build reads them, a save or an import writes them
	HoldsDocuments = 1 << 1, // the open documents: an edit writes them, an import or a rename reads them again
	HoldsProject = 1 << 2,   // which project is open, and its settings
	HoldsSlot = 1 << 3,      // the one operation slot: every operation writes it, as does a request that starts one
};

constexpr Holds operator|(Holds a, Holds b) {
	return static_cast<Holds>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline constexpr Holds kHoldsAll = HoldsFiles | HoldsDocuments | HoldsProject;
// True when `a` and `b` share a flag.
constexpr bool holds_any(Holds a, Holds b) {
	return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}
// True when what reads `reads` and writes `writes` conflicts with what reads `held_reads` and
// writes `held_writes`: it writes what the other reads or writes, or reads what the other writes.
constexpr bool holds_conflict(Holds reads, Holds writes, Holds held_reads, Holds held_writes) {
	return holds_any(writes, held_reads | held_writes) || holds_any(reads, held_writes);
}

// A set of request kinds, one bit each.
struct RequestKindSet {
	uint64_t bits = 0;
	constexpr RequestKindSet() = default;
	constexpr RequestKindSet(std::initializer_list<EditorRequestKind> kinds) {
		for (const EditorRequestKind kind : kinds) bits |= uint64_t(1) << static_cast<unsigned>(kind);
	}
	constexpr bool has(EditorRequestKind kind) const {
		return ((bits >> static_cast<unsigned>(kind)) & 1u) != 0;
	}
};
static_assert(kEditorRequestKindCount <= 64, "a RequestKindSet holds every request kind");

// Each kind's row: its wire token, what it reads and writes, the request kinds it serves as they
// are while it runs (joined_by: a Build and a Play onto a build) and those that take its place,
// cancelling it (superseded_by: a new import plan over a running one), and its words (the menu
// bar's "Building", a sentence's "the build").
struct OperationKindRow {
	OperationKind kind;
	const char *token;
	Holds reads;
	Holds writes;
	RequestKindSet joined_by;
	RequestKindSet superseded_by;
	const char *verb;
	const char *noun;
};

const OperationKindRow &operation_kind_row(OperationKind kind);
// A holds set's flags by their tokens ("files", "documents", "project", "slot"), in bit order.
std::vector<const char *> holds_tokens(Holds holds);

// How much one step of an operation may do: the bytes it reads, hashes, writes or copies.
struct StepBudget {
	uint64_t bytes = 0;
};

// How much one poll of the session may do: steps of `step_bytes` for `ms` milliseconds, at least
// one; `ms` 0 is exactly one step per poll (a test's).
struct PollBudget {
	int64_t ms = 0;
	uint64_t step_bytes = 0;
};

// The editor's: a slice of a frame at 60 Hz.
inline constexpr PollBudget kDefaultPollBudget{10, uint64_t(1) << 20};

enum class OperationUnit : uint8_t { Bytes, Files, Steps };
const char *operation_unit_token(OperationUnit unit);

// Where an operation stands: `done` of `total` (never more, never going back), in `unit`, and
// what it works on now ("Packing localres.pff").
struct OperationProgress {
	uint64_t done = 0;
	uint64_t total = 0;
	OperationUnit unit = OperationUnit::Steps;
	std::string label;
};

// How an operation ended: its work done, its work failed (a build refused or broken off: its
// findings say why), or cancelled between two steps (its work discarded, never finished).
enum class OperationEnd : uint8_t { Done, Failed, Cancelled };
const char *operation_end_token(OperationEnd end);

// What an operation came to (the view's last_operation): which one, how it ended and the findings
// it reported.
struct OperationOutcome {
	uint64_t id = 0;
	OperationKind kind = OperationKind::Build;
	OperationEnd end = OperationEnd::Done;
	std::vector<Diagnostic> findings;
};

// The operation that runs, as the view shows it (id 0: none), with what the busy gate weighs a
// request against (request_kinds.h): what it reads and writes, and whether it can be cancelled.
struct OperationStatus {
	uint64_t id = 0;
	OperationKind kind = OperationKind::Build;
	std::string label;
	uint64_t done = 0;
	uint64_t total = 0;
	OperationUnit unit = OperationUnit::Steps;
	bool cancellable = false;
	Holds reads = HoldsNothing;
	Holds writes = HoldsNothing;
	bool running() const { return id != 0; }
};

// One long job (ADR 0046 S13 A1). It steps within a budget and never writes the session's view
// while it runs: finish() absorbs its work, once, on the poll that sees it done. Single-threaded:
// the session calls it between two frames' requests.
class SessionOperation {
public:
	virtual ~SessionOperation() = default;

	virtual OperationKind kind() const = 0;
	virtual Holds reads() const { return operation_kind_row(kind()).reads; }
	virtual Holds writes() const { return operation_kind_row(kind()).writes; }
	// One step within `budget`; true once the operation's work is done (finish() then absorbs it).
	virtual bool step(const StepBudget &budget) = 0;
	virtual OperationProgress progress() const = 0;
	virtual bool cancellable() const { return true; }
	// Stops between two steps and discards the work (a build's staging directory goes): the
	// operation never finishes.
	virtual void cancel() = 0;
	// A request its row's joined_by names, served by this operation as it is: what it adds (a
	// Play onto a build starts the game when the build lands).
	virtual void join(const EditorRequest &request) { (void)request; }
	// The work absorbed into the session: what the operation came to (how it ended and its
	// findings; the slot names it).
	virtual OperationOutcome finish(SessionCore &core) = 0;
};

// Milliseconds, only ever compared with each other: what a poll's budget is measured on.
using OperationClock = std::function<int64_t()>;
int64_t steady_clock_ms();

// The one operation the session runs (ADR 0046 S13 A1): started, stepped within each poll's
// budget, finished once or cancelled between two steps; what the last one came to stays.
class OperationSlot {
public:
	// Takes `operation` when none runs: its id (from 1, never reused); 0 while another runs, and
	// `operation` is dropped.
	uint64_t start(std::unique_ptr<SessionOperation> operation);
	// Steps the running operation within `budget`, measured on `clock`; true when one is done and
	// waits for finish().
	bool poll(const PollBudget &budget, const OperationClock &clock);
	// Steps the running operation to its end (a test, a command line); true when one is done.
	bool run_to_end();
	// The done operation's finish(core), once: last() is what it came to, and the slot is free
	// (before its finish runs, so a finish may start the next one). False when none is done.
	bool finish(SessionCore &core);
	// Cancels the running operation between two steps: its work discarded, last() says so, the
	// slot free. False when none runs or it cannot be cancelled.
	bool cancel();

	SessionOperation *running() const { return operation_.get(); }
	bool done() const { return operation_ != nullptr && done_; }
	OperationStatus status() const;
	const OperationOutcome &last() const { return last_; }

private:
	std::unique_ptr<SessionOperation> operation_;
	uint64_t id_ = 0;
	uint64_t next_id_ = 1;
	bool done_ = false;
	OperationOutcome last_;
};

} // namespace opennova::editor
