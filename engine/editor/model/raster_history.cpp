#include "raster_history.h"

#include <algorithm>
#include <set>
#include <utility>

namespace opennova::editor {

RasterSideState::~RasterSideState() = default;

uint32_t RasterSideState::parts_changed(const RasterSideState &before) const {
	(void)before;
	return 0;
}

void RasterHistory::reset() {
	steps_.clear();
	cursor_ = 0;
	revision_ = saved_revision_ = 0;
	next_revision_ = 1;
	gesture_ = 0;
	bytes_ = 0;
}

RasterHistory RasterHistory::frozen() const {
	RasterHistory out(budget_);
	out.revision_ = revision_;
	out.saved_revision_ = saved_revision_;
	out.next_revision_ = next_revision_;
	return out;
}

RasterHistory::Batch RasterHistory::absorb(Step &step, RasterChange &change, uint64_t revision) {
	Batch batch;
	batch.revision = revision;
	if (change.whole) {
		batch.whole = true;
		step.whole = true;
		step.whole_before = std::move(change.whole_before);
		step.whole_after = std::move(change.whole_after);
		return batch;
	}
	for (TileSwap &swap : change.swaps) {
		const std::pair<int, int> key{swap.plane, swap.tile};
		batch.tiles.push_back(key);
		auto found = step.swaps.find(key);
		if (found == step.swaps.end())
			step.swaps.emplace(key, std::move(swap));
		else
			found->second.after = std::move(swap.after);
	}
	if (change.side_after) {
		if (!step.side_after) step.side_before = change.side_before;
		batch.parts = change.side_before ? change.side_after->parts_changed(*change.side_before) : ~uint32_t(0);
		step.side_after = std::move(change.side_after);
	}
	return batch;
}

size_t RasterHistory::bytes_of(const Step &step) {
	size_t bytes = sizeof(Step) + step.batches.size() * sizeof(Batch);
	for (const Batch &batch : step.batches) bytes += batch.tiles.size() * sizeof(std::pair<int, int>);
	for (const auto &entry : step.swaps) {
		bytes += sizeof(TileSwap);
		if (entry.second.before) bytes += entry.second.before->bytes.size();
		if (entry.second.after) bytes += entry.second.after->bytes.size();
	}
	// The side states: the one the step began from whole, the one it ended with over what it shares with it (a
	// landing's tiles and bake nodes, which the states share but for those it remade).
	if (step.side_before) bytes += step.side_before->footprint();
	if (step.side_after) bytes += step.side_after->footprint_over(step.side_before.get());
	if (step.whole)
		for (const RasterState *state : {&step.whole_before, &step.whole_after}) {
			for (const RasterPlane &plane : state->planes)
				bytes += static_cast<size_t>(plane.tile_count()) * plane.tile_bytes();
			if (state->side) bytes += state->side->footprint();
		}
	return bytes;
}

void RasterHistory::put(const Step &step, RasterState &state, bool after) {
	if (step.whole) {
		state = after ? step.whole_after : step.whole_before;
		return;
	}
	for (const auto &entry : step.swaps) {
		const TileSwap &swap = entry.second;
		if (swap.plane < 0 || static_cast<size_t>(swap.plane) >= state.planes.size()) continue;
		state.planes[static_cast<size_t>(swap.plane)].set_tile(swap.tile, after ? swap.after : swap.before);
	}
	if (step.side_after) state.side = after ? step.side_after : step.side_before;
}

bool RasterHistory::folds(uint64_t gesture) const {
	// A group never swallows the saved checkpoint (undo must be able to return to exactly what is on disk),
	// and a whole change is a step of its own.
	return gesture && gesture == gesture_ && cursor_ && cursor_ == steps_.size() && !steps_.back().whole &&
	       steps_.back().after_revision() != saved_revision_;
}

void RasterHistory::forget_redo() {
	for (size_t i = cursor_; i < steps_.size(); ++i) bytes_ -= steps_[i].bytes;
	steps_.erase(steps_.begin() + std::ptrdiff_t(cursor_), steps_.end());
}

void RasterHistory::trim() {
	while (bytes_ > budget_.bytes && steps_.size() > budget_.min_steps && cursor_ > 0) {
		bytes_ -= steps_.front().bytes;
		steps_.pop_front();
		--cursor_;
	}
}

void RasterHistory::commit(RasterChange change, uint64_t gesture) {
	const bool fold = !change.whole && folds(gesture);
	forget_redo();
	const uint64_t before = revision_;
	revision_ = next_revision_++;
	if (fold) {
		Step &last = steps_.back();
		bytes_ -= last.bytes;
		last.batches.push_back(absorb(last, change, revision_));
		last.bytes = bytes_of(last);
		last.mark.serial = next_step_serial();
		bytes_ += last.bytes;
	} else {
		Step step;
		step.before_revision = before;
		step.batches.push_back(absorb(step, change, revision_));
		step.bytes = bytes_of(step);
		step.mark = StepMark{next_step_serial(), gesture};
		bytes_ += step.bytes;
		steps_.push_back(std::move(step));
		++cursor_;
	}
	gesture_ = change.whole ? 0 : gesture;
	trim();
}

bool RasterHistory::can_amend() const {
	return !steps_.empty() && cursor_ == steps_.size() && !steps_.back().whole &&
	       steps_.back().after_revision() != saved_revision_;
}

bool RasterHistory::amend_last(RasterChange change) {
	if (change.whole || !can_amend()) return false;
	Step &last = steps_.back();
	revision_ = next_revision_++;
	bytes_ -= last.bytes;
	last.batches.push_back(absorb(last, change, revision_));
	last.bytes = bytes_of(last);
	bytes_ += last.bytes;
	trim();
	return true;
}

void RasterHistory::commit_alone(RasterChange change, uint64_t link) {
	forget_redo();
	Step step;
	step.before_revision = revision_;
	revision_ = next_revision_++;
	step.batches.push_back(absorb(step, change, revision_));
	step.bytes = bytes_of(step);
	step.mark = StepMark{next_step_serial(), link};
	bytes_ += step.bytes;
	steps_.push_back(std::move(step));
	++cursor_;
	gesture_ = 0;
	trim();
}

bool RasterHistory::holds_link(uint64_t link) const {
	if (!link) return false;
	for (const Step &step : steps_)
		if (step.mark.link == link) return true;
	return false;
}

bool RasterHistory::drop_open(uint64_t gesture, RasterState &state) {
	if (!gesture || !folds(gesture)) return false;
	const Step &step = steps_[--cursor_];
	put(step, state, false);
	revision_ = step.before_revision;
	forget_redo();
	gesture_ = 0;
	return true;
}

void RasterHistory::undo(RasterState &state) {
	if (!cursor_) return;
	const Step &step = steps_[--cursor_];
	put(step, state, false);
	revision_ = step.before_revision;
	gesture_ = 0;
}

void RasterHistory::redo(RasterState &state) {
	if (cursor_ >= steps_.size()) return;
	const Step &step = steps_[cursor_++];
	put(step, state, true);
	revision_ = step.after_revision();
	gesture_ = 0;
}

bool RasterHistory::changes_since(uint64_t revision, std::vector<std::pair<int, int>> &tiles, uint32_t &parts) const {
	tiles.clear();
	parts = 0;
	if (revision == revision_) return true;
	if (steps_.empty()) return false;
	// Every batch kept, in order: the state `revision` names is after one of them (or before the first);
	// the history is after those of the steps before the cursor.
	std::vector<const Batch *> batches;
	size_t from = SIZE_MAX, at = 0;
	if (revision == steps_.front().before_revision) from = 0;
	for (size_t s = 0; s < steps_.size(); ++s) {
		if (s == cursor_) at = batches.size();
		for (const Batch &batch : steps_[s].batches) {
			batches.push_back(&batch);
			if (batch.revision == revision) from = batches.size();
		}
	}
	if (cursor_ == steps_.size()) at = batches.size();
	if (from == SIZE_MAX) return false;
	std::set<std::pair<int, int>> seen;
	for (size_t i = std::min(from, at); i < std::max(from, at); ++i) {
		const Batch &batch = *batches[i];
		if (batch.whole) {
			tiles.clear();
			parts = 0;
			return false;
		}
		seen.insert(batch.tiles.begin(), batch.tiles.end());
		parts |= batch.parts;
	}
	tiles.assign(seen.begin(), seen.end());
	return true;
}

} // namespace opennova::editor
