#pragma once

#include <optional>
#include <string>
#include <vector>

#include <editor/session/editor_request.h>

namespace opennova::editor {

class SessionCore;
struct SessionView;

// The unsaved-changes prompt (ADR 0046 S11a, S12 C, S13 A2), session policy: a request that would
// lose, pack or write over unsaved edits waits on the prompt instead of running, the prompt listing
// those files, and runs once it is answered (Save writes them first, Discard drops them, Cancel
// drops the request). What a request guards is its row's (request_kinds.h: `guard`, and whether
// the prompt offers Discard, `can_discard`): the document it names, every document with unsaved
// edits, or those among the files it plans to write, each planner asked (ImportController,
// RenameController). An answer is weighed against the running operation before anything is saved
// or dropped (busy_refuses_answer), as the prompt's buttons are enabled.
class UnsavedGuard {
public:
	explicit UnsavedGuard(SessionCore &core);
	UnsavedGuard(const UnsavedGuard &) = delete;
	UnsavedGuard &operator=(const UnsavedGuard &) = delete;

	// What a request the prompt guards would lose, pack or write over, now (its row's guard): a
	// Close or a Reload, its document when that has unsaved edits; a project switch, Quit, Build
	// and Play, every document with unsaved edits; an import that replaces files, the documents
	// with unsaved edits among the files it writes; a rename (and an assignment, which renames),
	// those among the files it rewrites and the renamed file itself; a name renamed everywhere,
	// those among the files it rewrites. False for a request the prompt does not guard.
	bool files(const EditorRequest &request, std::vector<std::string> &out);
	// True when `request` now waits on the prompt, which lists the files it would lose, pack or
	// write over. One that finds nothing unsaved goes ahead, and a prompt still open from an
	// earlier request is dropped: what it waited on was saved another way, and this request comes
	// after it.
	bool holds(const EditorRequest &request);
	// The prompt's answer: the request that waited, to run now (the files it lists written by a
	// Save, dropped by a Discard); none when nothing runs (a Cancel, an answer refused, a prompt
	// renewed with a file made unsaved since it opened, a Save that could not write every file).
	std::optional<EditorRequest> resolve(UnsavedChoice choice);
	// The prompt closed, the request it held dropped.
	void close_prompt();
	void close_prompt_if_open() {
		if (pending_) close_prompt();
	}

private:
	// The prompt's answer (Save or Discard) refused against the running operation (true, said
	// why, the prompt kept): what waits as the gate would answer it, then the answer itself.
	bool answer_refused(UnsavedChoice choice);

	SessionCore &core_;
	SessionView &view_;
	std::optional<EditorRequest> pending_; // what the prompt holds
};

} // namespace opennova::editor
