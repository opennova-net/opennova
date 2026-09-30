#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/change_set.h>
#include <editor/model/node.h>

namespace opennova::editor {

// One row's part of an undo step (ADR 0046 S13 D7): the row's version before the step and after it
// (either empty: a row the step adds or removes), with its index among the rows on each side, a hint
// the undo and the redo try first (a row not found there is looked for by its identity). A row the
// step moves among the rows it keeps is taken out and put back (`moved`: its two versions may be one);
// any other row both sides have is swapped in place, so a step that changes the fields of k rows
// undoes and redoes in k swaps, however many rows the document holds.
struct RowSwap {
	std::shared_ptr<const Node> before, after;
	size_t before_position = 0, after_position = 0;
	bool moved = false;
	NodeId id() const { return before ? before->id : after ? after->id : 0; }
	bool in_place() const { return before && after && !moved; }
};

// One undo step: a swap per row it changes (sorted by the rows' identities) and the file-wide state
// on both sides. What a batch commits (Document::apply), which the document's type may veto first
// (Document::accept_step).
struct EditStep {
	std::vector<RowSwap> swaps;
	std::shared_ptr<const FileState> before_state, after_state;
	// It adds, removes or moves a row: such a step never folds and ends its edit group.
	bool changes_rows() const;
	bool empty() const { return swaps.empty() && before_state == after_state; }
};

// A step's swaps done to a row list, forward (its before to its after) or back: the rows it takes out
// from the last index down, the rows it puts in from the first index up, the rest swapped in place.
void apply_step(std::vector<std::shared_ptr<const Node>> &rows, const EditStep &step, bool forward);

// What a history keeps (ADR 0046 S13 D7): at most `bytes` of rows (each step's rows' footprints,
// before and after), the oldest steps given up first, and never fewer than `min_steps` steps (the
// last step always undoes, however large).
struct HistoryBudget {
	size_t bytes = size_t(64) << 20;
	size_t min_steps = 1;
};

// The undo/redo journal of one document over the document's own row list and file-wide state (ADR
// 0046 d9, S13 D7). Revisions are monotonic and never given twice, so a save checkpoint survives
// undo, redo and the steps given up, and dirty() is exact. Consecutive steps under the same open
// group key fold into one: each row's version before the group and its latest after, over any rows
// (a gesture dragging several records is one step); a step that adds, removes or moves a row never
// folds and ends the group, and a group never folds over the saved checkpoint. Past its budget the
// history gives up its oldest steps.
class EditHistory {
public:
	EditHistory(std::vector<std::shared_ptr<const Node>> &rows, std::shared_ptr<const FileState> &state,
	            HistoryBudget budget = {})
	    : rows_(rows), state_(state), budget_(budget) {}
	// The journal another keeps, as it stands, over another document's row list and file-wide
	// state (a snapshot's: Document::snapshot).
	EditHistory(const EditHistory &other, std::vector<std::shared_ptr<const Node>> &rows,
	            std::shared_ptr<const FileState> &state)
	    : rows_(rows), state_(state), budget_(other.budget_), steps_(other.steps_), cursor_(other.cursor_),
	      revision_(other.revision_), saved_revision_(other.saved_revision_),
	      next_revision_(other.next_revision_), key_(other.key_), bytes_(other.bytes_) {}
	EditHistory(const EditHistory &) = delete;
	EditHistory &operator=(const EditHistory &) = delete;

	void reset();
	// The step done forward and recorded, the redo branch discarded. It folds into the last step while
	// the group `key` is open and neither changes rows; a step that changes rows ends the group.
	void commit(EditStep step, const std::string &key);
	// While the group `key` is open (its step is the last one and not the saved checkpoint), undo that
	// step and keep it as the redo branch, the group still open: the next commit replaces it (a
	// coalesced batch starting again from what its group found; the revisions it took are the new
	// step's) and resume() puts it back. False, with nothing undone, when the group is not open.
	bool reopen(const std::string &key);
	void resume();
	// After reopen(), the step it undid forgotten instead and the group ended: what the group
	// changed came back to what it found, so it leaves no step (and no redo) behind.
	void drop();
	void undo();
	void redo();
	void end_edit_group() { key_.clear(); }
	void mark_saved() { saved_revision_ = revision_; }

	bool can_undo() const { return cursor_ != 0; }
	bool can_redo() const { return cursor_ < steps_.size(); }
	bool dirty() const { return revision_ != saved_revision_; }
	uint64_t revision() const { return revision_; }
	// What the steps hold: their rows' footprints, before and after (a version two steps share is
	// counted in each); and how many steps it keeps, to undo and to redo.
	size_t bytes() const { return bytes_; }
	size_t steps() const { return steps_.size(); }
	const HistoryBudget &budget() const { return budget_; }
	// The rows that changed from the state `revision` to the one the history is at, walking the steps
	// between (undone or redone); a state a fold took into a step answers the rows the step's later
	// folds changed. Empty for the state it is at. False when it holds no such state: one of a branch
	// an edit after an undo discarded, of a coalesced group dropped, or older than the steps it kept.
	bool changes_since(uint64_t revision, RowChanges &out) const;

private:
	// A revision a step took (its first, then each fold's): the rows that changed since the step's
	// state before it, and whether the file-wide state did.
	struct Mark {
		uint64_t revision = 0;
		std::vector<NodeId> rows;
		bool state = false;
	};
	struct Entry {
		EditStep step;
		uint64_t before_revision = 0, after_revision = 0;
		std::vector<Mark> marks; // ascending; the last is after_revision
		size_t bytes = 0;
		bool changes_rows = false;
	};

	bool open(const std::string &key) const;  // the group `key` is open: its step is the last
	bool folds(const std::string &key) const; // the next commit under `key` folds into the last step
	void step_back();    // the step before the cursor undone
	void step_forward(); // the step at the cursor redone
	void forget_redo();  // the steps past the cursor forgotten
	void trim();         // the oldest steps given up while the history is past its budget

	std::vector<std::shared_ptr<const Node>> &rows_;
	std::shared_ptr<const FileState> &state_;
	HistoryBudget budget_;
	std::deque<Entry> steps_;
	size_t cursor_ = 0;
	uint64_t revision_ = 0, saved_revision_ = 0, next_revision_ = 1;
	std::string key_;
	size_t bytes_ = 0;
	bool reopened_ = false; // the step at the cursor is the one reopen() undid
};

} // namespace opennova::editor
