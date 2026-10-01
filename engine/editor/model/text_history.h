#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <editor/model/edit_history.h>

namespace opennova::editor {

// One replacement of a text (ADR 0046 S13 D9): at `offset` the characters `removed` gave way to
// `inserted`. Its reverse puts `removed` back in place of `inserted`.
struct TextReplacement {
	size_t offset = 0;
	std::string removed, inserted;
};

// A range of the text as it stands: `length` characters from `offset` (0 for a place characters
// were removed at).
struct TextRange {
	size_t offset = 0, length = 0;
};

// The undo/redo journal of one text document (ADR 0046 S13 D9): each step the batches of span
// replacements it took, kept as the replacements themselves (reversed to undo, done again to redo),
// under a byte budget (HistoryBudget, as a record document's: the oldest steps given up first,
// never the last). Revisions are monotonic and never given twice, so the saved checkpoint survives
// undo, redo and the steps given up, and dirty() is exact. Consecutive batches under the same open
// group key (a gesture's token, a typing burst) fold into one step, never over the saved
// checkpoint. The journal does not hold the text: it records what a batch did to it, and each undo
// and redo hands the replacements to do to the document's `apply`.
class TextHistory {
public:
	// What an undo or a redo asks of the text: `count` characters at `offset` replaced by `with`.
	using Apply = std::function<void(size_t offset, size_t count, const std::string &with)>;

	explicit TextHistory(HistoryBudget budget = {}) : budget_(budget) {}

	void reset();
	// The history's state with none of its steps (a snapshot's, S13 D9): its revision, its saved
	// revision and its budget; it has nothing to undo or redo, and changes_since answers its own
	// revision alone.
	TextHistory frozen() const;
	// A batch the document did (its replacements in the order it did them) recorded, the redo
	// branch discarded; it folds into the last step while the group `key` is open ("" is none).
	void commit(std::vector<TextReplacement> batch, const std::string &key);
	void undo(const Apply &apply);
	void redo(const Apply &apply);
	void end_edit_group() { key_.clear(); }
	void mark_saved() { saved_revision_ = revision_; }

	bool can_undo() const { return cursor_ != 0; }
	bool can_redo() const { return cursor_ < steps_.size(); }
	bool dirty() const { return revision_ != saved_revision_; }
	uint64_t revision() const { return revision_; }
	// What the steps hold: each replacement's two texts and its own object, and each batch's and
	// step's own; and how many steps it keeps, to undo and to redo.
	size_t bytes() const { return bytes_; }
	size_t steps() const { return steps_.size(); }
	const HistoryBudget &budget() const { return budget_; }
	// The ranges of the text as it stands that changed since the state `revision` (walking the
	// batches between, undone or done again), sorted and apart; none for the state it is at. False
	// when it holds no such state: one of a branch an edit after an undo discarded, or older than the
	// steps it kept.
	bool changes_since(uint64_t revision, std::vector<TextRange> &out) const;

private:
	struct Batch {
		std::vector<TextReplacement> replacements;
		uint64_t revision = 0; // the state after it
	};
	struct Step {
		std::vector<Batch> batches;
		uint64_t before_revision = 0;
		size_t bytes = 0;
		uint64_t after_revision() const { return batches.back().revision; }
	};

	bool folds(const std::string &key) const;
	void forget_redo();
	void trim();
	// What a batch adds to its step's bytes: its own object, and each replacement's object and two
	// texts (a step's bytes are its own object's and its batches').
	static size_t bytes_of(const std::vector<TextReplacement> &batch);
	// One batch done forward, or undone, through `apply`.
	static void forward(const Batch &batch, const Apply &apply);
	static void backward(const Batch &batch, const Apply &apply);

	HistoryBudget budget_;
	std::deque<Step> steps_;
	size_t cursor_ = 0;
	uint64_t revision_ = 0, saved_revision_ = 0, next_revision_ = 1;
	std::string key_;
	size_t bytes_ = 0;
};

} // namespace opennova::editor
