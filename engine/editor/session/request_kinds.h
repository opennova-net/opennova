#pragma once

#include <cstdint>
#include <string>

#include <editor/session/editor_request.h>
#include <editor/session/request_fields.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class SessionCore;

// What a request asks of the busy gate (ADR 0046 S13 A1: the session's gate, serve_request) when an
// operation runs that its request kind's row conflicts with, or that serves it or gives way to it.
enum class OnBusy : uint8_t {
	Refuse, // refused while it conflicts: an operation.busy warning, the outcome not done
	Join, // served by the running operation when its row's joined_by names the kind (a
		  // Build or a Play onto a build, the Play starting the game when it lands)
	Supersede, // takes the running operation's place when its row's superseded_by names the kind
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

// Who serves a request kind (S13 A4).
enum class ServedBy : uint8_t {
	Session, // the portable session, by its row's handler (ProjectSession::handle)
	Shell, // the shell alone: an OS's file manager (RevealPath)
	ShellNeedsPerson, // the shell, with a person to answer its native dialog (the pickers): never
					  // through the editor MCP, whose requests pass the path instead
};

// What serves a session row: a function over the session's core, which reaches every part of the
// session (SessionCore::Parts).
using RequestHandler = void (*)(SessionCore &core, const EditorRequest &request);

// The request table (S13 A4): one row per request kind, in the enum's order, each column one piece
// of request policy that a switch over the kinds answered before (the facade's dispatch, the
// windows' deferral and active-document rules, the prompt's words, the wire tokens). Its token on
// the wire; who serves it and, for the session, the handler; the fields it takes (params: those
// it may carry and those it must, which the wire reader holds a request to); what the unsaved-
// changes prompt guards of it and whether the prompt offers Discard (never for what packs the files
// on disk or writes over them: Build, Play, the imports and the renames, whose Save writes the
// edits first); whether it acts on the files as saved (raised while a frame draws, it waits for the
// frame's other requests: a Ctrl+S pressed as the Inspector sets a field saves the field too);
// whether an empty path names the active document; whether it ends every edit group first (Build
// and Play pack the files as saved); whether the validation an edit left due runs before it (its
// plan reads the graph); what it reads and writes of the session's resources (Holds) and what it
// asks of the busy gate (a request conflicts with the running operation when it writes what the
// operation reads or writes, or reads what the operation writes); the prompt's words for what waits
// on it ("Close %s", the %s the file) and its Save button; and what it does. Every row is
// static_asserted into place (request_kinds.cpp).
struct RequestKindRow {
	EditorRequestKind kind = EditorRequestKind::kCount;
	const char *token = "";
	ServedBy served_by = ServedBy::Session;
	RequestHandler handler = nullptr;
	RequestParams params;
	GuardScope guard = GuardScope::None;
	bool can_discard = false;
	bool acts_on_saved = false;
	bool names_active = false;
	bool ends_edit_groups = false;
	Holds reads = HoldsNothing;
	Holds writes = HoldsNothing;
	OnBusy on_busy = OnBusy::Refuse;
	const char *waiting = nullptr; // null where the prompt does not guard the kind
	const char *save_label = nullptr; // likewise
	const char *doc = "";
};

// A kind's row; RevealPath's for a value past the last kind.
const RequestKindRow &request_kind_row(EditorRequestKind kind);
// The kind a wire token names ("new_project"); false for none.
bool request_kind_from_token(const std::string &token, EditorRequestKind &out);

// What the gate does with a request of `kind` while the operation `running` shows runs.
enum class GateAnswer : uint8_t {
	Proceed, // no operation, or none it conflicts with: it runs
	Refuse, // it conflicts: refused with operation.busy
	Join, // the running operation serves it
	Supersede, // the running operation is cancelled and it runs
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
bool busy_refuses_answer(
		EditorRequestKind waiting, UnsavedChoice choice, const OperationStatus &running);

// The request served by its row: false for a shell row (the shell serves it); else, in this order,
// every edit group ended where the row says so (Build, Play), or for a row naming a document the
// gesture open in that document ended unless the request is that gesture's batch (S13 V8,
// DocumentSet::end_gesture_for), the busy gate (refused, or joined to
// the running operation: served), the validation an edit left due where the row reads the graph
// first, the unsaved-changes prompt (the request held: served), then the row's handler. The facade
// serves a request from outside through it (ProjectSession::handle), and the prompt's answer what
// waited on it, never through handle().
bool serve_request(SessionCore &core, const EditorRequest &request);

// What waits on the unsaved-changes prompt, in the words of the menu that asked for it: its row's
// `waiting` with the file name of `target` for the %s ("Close main.mnu", "Build"); "" for a kind
// the prompt does not guard.
std::string waiting_words(EditorRequestKind kind, const std::string &target);

} // namespace opennova::editor
