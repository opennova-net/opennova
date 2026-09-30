#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <editor/model/edit_history.h>
#include <editor/model/node.h>

namespace opennova::editor {

// The rows as a batch leaves them before it commits (ADR 0046 S13 D7). It starts as the document's
// committed rows and file-wide state; the first edit that touches a row clones it (each row cloned
// once, however many edits of the batch change it), the rows the batch adds, removes and moves change
// the order, and nothing reaches the document until the batch commits the step this makes (step()):
// a swap per row that changed, in place for a row kept among the others, so a batch of Sets on k rows
// makes k clones and k swaps however many rows the document holds.
class StagedRows {
public:
	// A committed row's index by its identity (rows.size() for none): the document's own index, so a
	// batch that changes rows in place never indexes the rows itself.
	using RowFinder = std::function<size_t(NodeId)>;
	StagedRows(const std::vector<std::shared_ptr<const Node>> &rows, std::shared_ptr<const FileState> state,
	           RowFinder find);
	StagedRows(const StagedRows &) = delete;
	StagedRows &operator=(const StagedRows &) = delete;

	// The rows as the batch leaves them so far, in order, a touched row as its clone.
	const std::vector<std::shared_ptr<const Node>> &rows() const;
	size_t size() const;
	// The index of the row `id` among them, and the row (size() and null for none: a row the document
	// never had, or one the batch removed).
	size_t index_of(NodeId id) const;
	const Node *find(NodeId id) const;
	// A committed row an earlier edit of the batch removed.
	bool removed(NodeId id) const { return removed_.count(id) != 0; }
	// The row `id` to edit: its clone, made on the batch's first touch of it (a row the batch adds is
	// its own); null for a row the batch does not have.
	Node *touch(NodeId id);
	// An edit changed the touched row `id` (a row only touched commits as it was); one changed what it
	// holds (a nested record's place is then found by walking the clone, not the committed row's index).
	void mark_changed(NodeId id);
	void mark_reshaped(NodeId id);
	bool reshaped(NodeId id) const;
	// A row the batch adds (a new row, a copy, a pasted row) at `position` (the end when past it): a
	// changed row of its own.
	void insert(std::shared_ptr<Node> row, size_t position);
	void remove(NodeId id);
	// The row `id` to `position` among the others (the last when past it); false when it is there.
	bool move(NodeId id, size_t position);
	// The file-wide state as the batch leaves it (an edit replaces it with a changed copy).
	std::shared_ptr<const FileState> &state() { return state_; }
	// Each row an edit changed, touched or added: what the document's type refreshes before the step
	// (Document::after_edit).
	void for_each_changed(const std::function<void(Node &)> &fn);
	// The step from the committed rows to these: a row removed, a row added, a row moved (the fewest
	// the new order needs: those off the longest run of rows kept in their order), and a row changed
	// in place; sorted by the rows' identities.
	EditStep step() const;

private:
	struct Touched {
		std::shared_ptr<Node> row;
		bool changed = false, reshaped = false, added = false;
	};
	// The staged list made from the committed one (list_), kept from then on.
	void list() const;

	const std::vector<std::shared_ptr<const Node>> &committed_;
	RowFinder find_;
	std::shared_ptr<const FileState> state_before_, state_;
	std::unordered_map<NodeId, Touched> touched_;
	std::unordered_set<NodeId> removed_;
	mutable std::vector<std::shared_ptr<const Node>> list_;
	mutable bool listed_ = false; // list_ is the staged order (made on the first ask of it)
	bool shaped_ = false;         // a row was added, removed or moved: the order is list_'s own
	mutable std::unordered_map<NodeId, size_t> positions_;
	mutable bool positions_known_ = false;
};

} // namespace opennova::editor
