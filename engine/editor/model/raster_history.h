#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <editor/model/edit.h>
#include <editor/model/edit_history.h>
#include <editor/model/raster_plane.h>

namespace opennova::editor {

// What a raster holds beside its planes, its type's (a heights document's baked terrain, a texture's file
// form), immutable once made so that the history's steps share it.
struct RasterSideState {
	virtual ~RasterSideState();
	// What it holds, in bytes (the history's budget).
	virtual size_t footprint() const = 0;
	// The parts in which it differs from `before`, a bit each of its type's: what changes_since names
	// beside the regions (RasterChanges::parts). None by default.
	virtual uint32_t parts_changed(const RasterSideState &before) const;
};

// A raster's content: its planes and its side state.
struct RasterState {
	std::vector<RasterPlane> planes;
	std::shared_ptr<const RasterSideState> side;
};

// One batch's change to a raster, as its edits made it: the tiles they wrote, and the side state before and
// after where they changed it (both null where they did not); or a whole change (lane B's byte-level
// operations: a resize, a re-encode), the state before and after whole.
struct RasterChange {
	std::vector<TileSwap> swaps;
	std::shared_ptr<const RasterSideState> side_before, side_after;
	bool whole = false;
	RasterState whole_before, whole_after;
	bool empty() const { return !whole && swaps.empty() && !side_after; }
};

// The undo/redo journal of one raster document (ADR 0046 S25, the raster core), modelled on TextHistory: each
// step the tiles its batches swapped (each tile's first before and latest after) and its side state's first
// before and latest after, or a whole change, under a byte budget (the oldest steps given up first, never the
// last). Revisions are monotonic and never given twice, so the saved checkpoint survives undo, redo and the
// steps given up, and dirty() is exact. Consecutive batches under the same open gesture fold into one step,
// never over the saved checkpoint, a whole change never; each batch keeps the tiles and parts it changed, so
// changes_since answers any state the kept steps passed through. A bake's landing joins its stroke's step
// (amend_last). Each step keeps its StepMark (S25: its serial, taken anew by every batch made or folded
// into it, and its link, the gesture token).
class RasterHistory {
public:
	explicit RasterHistory(HistoryBudget budget = {}) : budget_(budget) {}

	void reset();
	// The history's state with none of its steps (a snapshot's): its revision, its saved revision and its
	// budget; nothing to undo or redo, and changes_since answers its own revision alone.
	RasterHistory frozen() const;

	// A batch the document applied to `state` recorded (the redo branch discarded); it folds into the last
	// step while the gesture `gesture` (0 none) is open. The step's link is the gesture.
	void commit(RasterChange change, uint64_t gesture);
	// The batch joins the last step (a bake's landing in its stroke's step), a revision of its own: only
	// while nothing is to redo, the last step is no whole change and is not the saved checkpoint. False,
	// nothing recorded, where it cannot (the caller's change stays applied: it puts it back).
	bool amend_last(RasterChange change);
	bool can_amend() const;
	void undo(RasterState &state);
	void redo(RasterState &state);
	void end_edit_group() { gesture_ = 0; }
	// A cancel of the gesture `gesture` (Esc mid-stroke): while its group is open (the last step, past the
	// saved checkpoint), that step undone on `state` and forgotten, no redo left of it; false, nothing done,
	// when it is not open.
	bool drop_open(uint64_t gesture, RasterState &state);
	void mark_saved() { saved_revision_ = revision_; }

	StepMark undo_mark() const { return cursor_ ? steps_[cursor_ - 1].mark : StepMark(); }
	StepMark redo_mark() const { return cursor_ < steps_.size() ? steps_[cursor_].mark : StepMark(); }
	bool can_undo() const { return cursor_ != 0; }
	bool can_redo() const { return cursor_ < steps_.size(); }
	bool dirty() const { return revision_ != saved_revision_; }
	uint64_t revision() const { return revision_; }
	size_t bytes() const { return bytes_; }
	size_t steps() const { return steps_.size(); }
	const HistoryBudget &budget() const { return budget_; }

	// The tiles (plane, tile) and side-state parts that changed since the state `revision`, walking the
	// batches between (undone or done again); none for the state it is at. False when it holds no such
	// state (a branch an edit after an undo discarded, a state older than the steps it kept) or a whole
	// change lies between.
	bool changes_since(uint64_t revision, std::vector<std::pair<int, int>> &tiles, uint32_t &parts) const;

private:
	struct Batch {
		uint64_t revision = 0; // the state after it
		std::vector<std::pair<int, int>> tiles;
		uint32_t parts = 0;
		bool whole = false;
	};
	struct Step {
		std::vector<Batch> batches;
		uint64_t before_revision = 0;
		// (plane, tile) -> the tile's first before and latest after over the step's batches.
		std::map<std::pair<int, int>, TileSwap> swaps;
		std::shared_ptr<const RasterSideState> side_before, side_after;
		bool whole = false;
		RasterState whole_before, whole_after;
		size_t bytes = 0;
		StepMark mark;
		uint64_t after_revision() const { return batches.back().revision; }
	};

	bool folds(uint64_t gesture) const;
	void forget_redo();
	void trim();
	// The batch's record (tiles, parts) and what it adds to the step's swaps and side states.
	static Batch absorb(Step &step, RasterChange &change, uint64_t revision);
	static size_t bytes_of(const Step &step);
	static void put(const Step &step, RasterState &state, bool after);

	HistoryBudget budget_;
	std::deque<Step> steps_;
	size_t cursor_ = 0;
	uint64_t revision_ = 0, saved_revision_ = 0, next_revision_ = 1;
	uint64_t gesture_ = 0;
	size_t bytes_ = 0;
};

} // namespace opennova::editor
