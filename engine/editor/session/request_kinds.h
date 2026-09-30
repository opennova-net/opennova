#pragma once

#include <cstdint>

#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// What a request does when the operation that runs holds what it needs (ADR 0046 S13 A1: the
// session's busy gate, in handle()). A request the unsaved-changes prompt would hold, and which
// the row does not refuse, asks first and meets the gate once it goes ahead.
enum class OnBusy : uint8_t {
	Refuse,        // refused: an operation.busy warning, the outcome not done, nothing changed
	Join,          // served by the running operation when it takes it (SessionOperation::join: a
	               // Build or a Play onto a build, the Play starting the game when it lands), else
	               // refused
	Supersede,     // the running operation cancelled for this one when it takes its place
	               // (SessionOperation::superseded_by: a new import plan over a running one), else
	               // refused
	CancelRunning, // the running operation cancelled first, then the request runs (a project
	               // switch, Quit)
};

// One row per request kind, in the enum's order: what the request needs that an operation may
// hold, and what it does when one does. The table grows a column per piece of request policy
// (S13 A4); every row is static_asserted into place (request_kinds.cpp).
struct RequestKindRow {
	EditorRequestKind kind;
	Holds needs;
	OnBusy on_busy;
};

const RequestKindRow &request_kind_row(EditorRequestKind kind);

// True when the gate refuses `kind` while the operation `running` shows runs: it holds what the
// request needs and the row refuses (a Join or a Supersede the operation might not take counts as
// allowed: the session answers it). Nothing runs, nothing is refused. The windows disable what it
// refuses.
bool busy_refuses(EditorRequestKind kind, const OperationStatus &running);

} // namespace opennova::editor
