#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/node.h>

namespace opennova::editor {

// One undoable change: a row swap (before -> after, either may be empty for an
// insert or a removal) plus the file-wide state on both sides. Revisions are
// monotonic so a save checkpoint survives undo and redo.
struct Change {
	std::shared_ptr<const Node> before, after;
	size_t before_position = 0, after_position = 0;
	std::shared_ptr<const FileState> before_state, after_state;
	uint64_t before_revision = 0, after_revision = 0;
	// Undone and redone with the change before it: one step over several rows (a commit of
	// several changes, the step a batch over several rows makes).
	bool joined = false;
};

// A change's row swap on a row list: forward replaces `before` with `after` at
// after_position, backward the reverse (the undo and redo the journal makes, and the
// row list a document type checks a change against before it commits).
void apply_change(std::vector<std::shared_ptr<const Node>> &rows, const Change &change, bool forward);

// The undo/redo journal of one document over the document's own row list and
// file-wide state (ADR 0046 d9). Consecutive changes carrying the same coalesce
// key fold into one step until the group ends or a save checkpoints the journal.
class EditHistory {
public:
	EditHistory(std::vector<std::shared_ptr<const Node>> &rows, std::shared_ptr<const FileState> &state)
	    : rows_(rows), state_(state) {}
	// The journal another keeps, as it stands, over another document's row list and file-wide
	// state (a snapshot's: Document::snapshot).
	EditHistory(const EditHistory &other, std::vector<std::shared_ptr<const Node>> &rows,
			std::shared_ptr<const FileState> &state)
			: rows_(rows), state_(state), history_(other.history_), cursor_(other.cursor_),
			  revision_(other.revision_), saved_revision_(other.saved_revision_),
			  next_revision_(other.next_revision_), coalesce_key_(other.coalesce_key_) {}
	EditHistory(const EditHistory &) = delete;
	EditHistory &operator=(const EditHistory &) = delete;

	void reset();
	// Apply the change forward and record it (truncating any redo branch).
	void commit(Change change, const std::string &coalesce_key);
	// Several changes as one step, each joined to the one before it (undone and redone
	// together); one change is a commit. A step of several changes never folds, nor does
	// a later change fold into it: the group `coalesce_key` opens continues by reopen().
	void commit(std::vector<Change> changes, const std::string &coalesce_key);
	// While the group `key` is open (its step is the last one and not the saved
	// checkpoint), undo that step and keep it as the redo branch, the group still open: the
	// next commit replaces it (a coalesced batch starting again from what its group found)
	// and resume() puts it back. False, with nothing undone, when the group is not open.
	bool reopen(const std::string &key);
	void resume();
	// After reopen(), the step it undid forgotten instead and the group ended: what the group
	// changed came back to what it found, so it leaves no step (and no redo) behind.
	void drop();
	// One step back or forward: a change and every change joined to it.
	void undo();
	void redo();
	void end_edit_group() { coalesce_key_.clear(); }
	void mark_saved() { saved_revision_ = revision_; }

	bool can_undo() const { return cursor_ != 0; }
	bool can_redo() const { return cursor_ < history_.size(); }
	bool dirty() const { return revision_ != saved_revision_; }
	uint64_t revision() const { return revision_; }

private:
	bool open(const std::string &key) const;  // the group `key` is open: its step is the last
	bool folds(const std::string &key) const; // the next commit under `key` joins the last step
	void restore(const Change &change, bool forward);
	void step_back();    // the step before the cursor undone, its joined changes with it
	void step_forward(); // the step at the cursor redone, its joined changes with it

	std::vector<std::shared_ptr<const Node>> &rows_;
	std::shared_ptr<const FileState> &state_;
	std::vector<Change> history_;
	size_t cursor_ = 0;
	uint64_t revision_ = 0, saved_revision_ = 0, next_revision_ = 1;
	std::string coalesce_key_;
};

} // namespace opennova::editor
