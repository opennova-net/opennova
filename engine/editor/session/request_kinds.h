#pragma once

#include <cstdint>

#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// What a request asks of the busy gate (ADR 0046 S13 A1: the session's gate, in handle()) when an
// operation runs that its request kind's row conflicts with, or that serves it or gives way to it.
enum class OnBusy : uint8_t {
	Refuse,        // refused while it conflicts: an operation.busy warning, the outcome not done
	Join,          // served by the running operation when its row's joined_by names the kind (a
	               // Build or a Play onto a build, the Play starting the game when it lands)
	Supersede,     // takes the running operation's place when its row's superseded_by names the kind
	               // (a new import plan over a running one): the operation is cancelled for it
	CancelRunning, // goes on: its own flow cancels the operation when it commits (a project closed,
	               // Quit), and is refused when the operation cannot be cancelled
};

// What the unsaved-changes prompt guards of a request (UnsavedGuard, S13 A2): nothing; the
// document it names, when that has unsaved edits (a Close, a Reload); every document with unsaved
// edits (a project switch, Quit, and Build and Play, which pack the files on disk); or those among
// the files its plan writes over or rewrites (an import that replaces files, a rename, an
// assignment, a rename everywhere), its planner asked.
enum class GuardScope : uint8_t { None, Document, AllDirty, PlannedWrites };

// One row per request kind, in the enum's order: what the request reads and writes of the
// session's resources (Holds), what it asks of the gate, what the unsaved-changes prompt guards of
// it and whether the prompt offers Discard for it (never for what packs the files on disk or writes
// over them: Build, Play, the imports and the renames, whose Save writes the edits first). A
// request conflicts with the running operation when it writes what the operation reads or writes,
// or reads what the operation writes. The table grows a column per piece of request policy (S13
// A4); every row is static_asserted into place (request_kinds.cpp).
struct RequestKindRow {
	EditorRequestKind kind;
	Holds reads;
	Holds writes;
	OnBusy on_busy;
	GuardScope guard;
	bool can_discard;
};

const RequestKindRow &request_kind_row(EditorRequestKind kind);

// What the gate does with a request of `kind` while the operation `running` shows runs.
enum class GateAnswer : uint8_t {
	Proceed,       // no operation, or none it conflicts with: it runs
	Refuse,        // it conflicts: refused with operation.busy
	Join,          // the running operation serves it
	Supersede,     // the running operation is cancelled and it runs
	CancelRunning, // it runs, and cancels the operation when it commits
};

// The gate's answer, from the request's row and the operation's (what it reads and writes, the
// kinds it joins and gives way to, whether it can be cancelled): Join and Supersede first, when
// the operation's row names the kind and the request's row asks for it (a Supersede of an
// operation that cannot be cancelled is refused: what would take its place waits), then Proceed
// when the two do not conflict, CancelRunning for a request that asks it of an operation that can
// be cancelled, else Refuse.
GateAnswer gate_answer(EditorRequestKind kind, const OperationStatus &running);
// True when the gate refuses `kind` while `running` runs: what the windows disable, exactly what
// the session refuses (a request the unsaved-changes prompt holds meets the gate once answered).
bool busy_refuses(EditorRequestKind kind, const OperationStatus &running);
// True when the unsaved-changes prompt's answer `choice` is refused while `running` runs and a
// request of `waiting` waits on the prompt, before anything is saved or dropped: what waits is
// refused at the gate (it would be once answered: a Discard would lose the edits and still not
// run it); or, when it does not cancel the operation as it commits (a project switch, Quit, which
// cancel it first), the answer itself conflicts with it: a Save writes the files and the
// documents as a Save All does, a Discard drops documents, a write of them. What the prompt's
// buttons are enabled by, and what the session refuses.
bool busy_refuses_answer(EditorRequestKind waiting, UnsavedChoice choice, const OperationStatus &running);

} // namespace opennova::editor
