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

// One row per request kind, in the enum's order: what the request reads and writes of the
// session's resources (Holds), and what it asks of the gate. A request conflicts with the running
// operation when it writes what the operation reads or writes, or reads what the operation writes.
// The table grows a column per piece of request policy (S13 A4); every row is static_asserted into
// place (request_kinds.cpp).
struct RequestKindRow {
	EditorRequestKind kind;
	Holds reads;
	Holds writes;
	OnBusy on_busy;
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

} // namespace opennova::editor
