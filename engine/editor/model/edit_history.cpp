#include "edit_history.h"

#include <algorithm>
#include <iterator>
#include <map>

namespace opennova::editor {

namespace {

size_t footprint(const std::shared_ptr<const Node> &row) {
	return row ? row->footprint() : 0;
}
size_t footprint(const std::shared_ptr<const FileState> &state) {
	return state ? state->footprint() : 0;
}
// What a source state keeps: the bytes it holds and its findings' text.
size_t footprint(const std::shared_ptr<const SourceState> &source) {
	if (!source) return 0;
	size_t bytes = sizeof(SourceState) + (source->odd_lines ? source->odd_lines->size() : 0);
	for (const SourceIssue &issue : source->issues)
		bytes += sizeof(SourceIssue) + issue.message.size() + issue.record.size() + issue.field.size() +
		         issue.locator.size();
	return bytes;
}
size_t swap_bytes(const RowSwap &swap) {
	return footprint(swap.before) + footprint(swap.after);
}

bool by_id(const RowSwap &a, const RowSwap &b) {
	return a.id() < b.id();
}

// The index of the row `id`: `hint` when the row is there, else wherever it is (rows.size() for
// none).
size_t find_row(const std::vector<std::shared_ptr<const Node>> &rows, NodeId id, size_t hint) {
	if (hint < rows.size() && rows[hint]->id == id)
		return hint;
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == id)
			return i;
	return rows.size();
}

// The identities a step swaps, ascending (its swaps are sorted by them).
std::vector<NodeId> ids_of(const EditStep &step) {
	std::vector<NodeId> ids;
	ids.reserve(step.swaps.size());
	for (const RowSwap &swap : step.swaps)
		ids.push_back(swap.id());
	return ids;
}

} // namespace

bool EditStep::changes_rows() const {
	for (const RowSwap &swap : swaps)
		if (!swap.in_place())
			return true;
	return false;
}

void apply_step(
		std::vector<std::shared_ptr<const Node>> &rows, const EditStep &step, bool forward) {
	struct Place {
		const RowSwap *swap;
		size_t position;
	};
	std::vector<Place> out, in;
	for (const RowSwap &swap : step.swaps) {
		const auto &from = forward ? swap.before : swap.after;
		const auto &to = forward ? swap.after : swap.before;
		if (from && (!to || swap.moved))
			out.push_back({ &swap, forward ? swap.before_position : swap.after_position });
		if (to && (!from || swap.moved))
			in.push_back({ &swap, forward ? swap.after_position : swap.before_position });
	}
	// Taken out from the last index down, so every index taken is still the one the step recorded;
	// then the rows kept stand in their order, and each row put in, from the first index up, lands
	// where the step leaves it.
	std::sort(out.begin(), out.end(),
			[](const Place &a, const Place &b) { return a.position > b.position; });
	for (const Place &place : out) {
		const size_t at = find_row(rows, place.swap->id(), place.position);
		if (at < rows.size())
			rows.erase(rows.begin() + std::ptrdiff_t(at));
	}
	std::sort(in.begin(), in.end(),
			[](const Place &a, const Place &b) { return a.position < b.position; });
	for (const Place &place : in) {
		const auto &to = forward ? place.swap->after : place.swap->before;
		rows.insert(rows.begin() + std::ptrdiff_t(std::min(place.position, rows.size())), to);
	}
	for (const RowSwap &swap : step.swaps) {
		if (!swap.in_place())
			continue;
		const size_t at =
				find_row(rows, swap.id(), forward ? swap.after_position : swap.before_position);
		if (at < rows.size())
			rows[at] = forward ? swap.after : swap.before;
	}
}

size_t EditHistory::mark_bytes(size_t rows) {
	return sizeof(Mark) + rows * sizeof(NodeId);
}

size_t EditHistory::bytes_of(const Entry &entry) {
	size_t bytes = 0;
	for (const RowSwap &swap : entry.step.swaps)
		bytes += swap_bytes(swap);
	if (entry.step.before_state != entry.step.after_state)
		bytes += footprint(entry.step.before_state) + footprint(entry.step.after_state);
	if (entry.step.before_source != entry.step.after_source)
		bytes += footprint(entry.step.before_source) + footprint(entry.step.after_source);
	for (const Mark &mark : entry.marks)
		bytes += mark_bytes(mark.rows.size());
	return bytes;
}

void EditHistory::reset() {
	steps_.clear();
	cursor_ = 0;
	revision_ = saved_revision_ = 0;
	next_revision_ = 1;
	key_.clear();
	bytes_ = 0;
	reopened_ = false;
}

bool EditHistory::open(const std::string &key) const {
	// A group never swallows the saved checkpoint: undo must be able to return to exactly what is
	// on disk.
	return !key.empty() && key == key_ && cursor_ && cursor_ == steps_.size() &&
			steps_.back().after_revision != saved_revision_;
}

bool EditHistory::folds(const std::string &key) const {
	return open(key) && !steps_.back().changes_rows;
}

void EditHistory::forget_redo() {
	for (size_t i = cursor_; i < steps_.size(); ++i)
		bytes_ -= steps_[i].bytes;
	steps_.erase(steps_.begin() + std::ptrdiff_t(cursor_), steps_.end());
}

void EditHistory::commit(EditStep step, const std::string &key) {
	if (!std::is_sorted(step.swaps.begin(), step.swaps.end(), by_id))
		std::sort(step.swaps.begin(), step.swaps.end(), by_id);
	const bool changes_rows = step.changes_rows();
	const bool fold = !changes_rows && folds(key);
	const uint64_t before = revision_, revision = next_revision_++;
	Mark mark{ revision, ids_of(step), step.before_state != step.after_state };
	std::vector<Mark> carried;
	if (cursor_ < steps_.size() && reopened_) {
		// The reopened step gives its place to this one, which starts from what its group found:
		// the revisions it took are this step's, and a row it changed that this one leaves as the
		// group found it changed since them too.
		Entry &replaced = steps_[cursor_];
		carried = std::move(replaced.marks);
		std::vector<NodeId> rows;
		const std::vector<NodeId> earlier = ids_of(replaced.step);
		std::set_union(mark.rows.begin(), mark.rows.end(), earlier.begin(), earlier.end(),
				std::back_inserter(rows));
		mark.rows = std::move(rows);
		mark.state = mark.state || replaced.step.before_state != replaced.step.after_state;
	}
	reopened_ = false;
	forget_redo();
	apply_step(rows_, step, true);
	state_ = step.after_state;
	revision_ = revision;
	if (fold) {
		// Each row the step changes keeps its version before the group and takes its latest after.
		Entry &last = steps_.back();
		bytes_ -= last.bytes;
		std::vector<RowSwap> merged;
		merged.reserve(last.step.swaps.size() + step.swaps.size());
		auto kept = last.step.swaps.begin();
		for (RowSwap &swap : step.swaps) {
			while (kept != last.step.swaps.end() && kept->id() < swap.id())
				merged.push_back(std::move(*kept++));
			if (kept != last.step.swaps.end() && kept->id() == swap.id()) {
				RowSwap folded = std::move(*kept++);
				folded.after = std::move(swap.after);
				merged.push_back(std::move(folded));
			} else {
				merged.push_back(std::move(swap));
			}
		}
		while (kept != last.step.swaps.end())
			merged.push_back(std::move(*kept++));
		last.step.swaps = std::move(merged);
		last.step.after_state = step.after_state;
		last.after_revision = revision;
		last.marks.push_back(std::move(mark));
		last.bytes = bytes_of(last);
		bytes_ += last.bytes;
	} else {
		Entry entry;
		entry.before_revision = before;
		entry.after_revision = revision;
		entry.marks = std::move(carried);
		entry.marks.push_back(std::move(mark));
		entry.changes_rows = changes_rows;
		entry.step = std::move(step);
		entry.bytes = bytes_of(entry);
		bytes_ += entry.bytes;
		steps_.push_back(std::move(entry));
		++cursor_;
	}
	key_ = changes_rows ? std::string() : key;
	trim();
}

void EditHistory::trim() {
	while (bytes_ > budget_.bytes && steps_.size() > budget_.min_steps && cursor_ > 0) {
		bytes_ -= steps_.front().bytes;
		steps_.pop_front();
		--cursor_;
	}
}

bool EditHistory::reopen(const std::string &key) {
	if (!open(key))
		return false;
	step_back();
	reopened_ = true;
	return true;
}

void EditHistory::resume() {
	if (cursor_ < steps_.size())
		step_forward();
	reopened_ = false;
}

void EditHistory::drop() {
	forget_redo();
	key_.clear();
	reopened_ = false;
}

void EditHistory::step_back() {
	const Entry &entry = steps_[--cursor_];
	apply_step(rows_, entry.step, false);
	state_ = entry.step.before_state;
	revision_ = entry.before_revision;
}

void EditHistory::step_forward() {
	const Entry &entry = steps_[cursor_++];
	apply_step(rows_, entry.step, true);
	state_ = entry.step.after_state;
	revision_ = entry.after_revision;
}

void EditHistory::undo() {
	if (!cursor_)
		return;
	step_back();
	end_edit_group();
	reopened_ = false;
}

void EditHistory::redo() {
	if (cursor_ >= steps_.size())
		return;
	step_forward();
	end_edit_group();
	reopened_ = false;
}

bool EditHistory::changes_since(uint64_t revision, RowChanges &out) const {
	out = RowChanges();
	if (revision == revision_)
		return true;
	if (steps_.empty())
		return false;
	// Where the state `revision` is: a boundary between the steps (after `boundary` of them; 0 the
	// state before the first one kept), or inside step `inside` at its mark `mark`, a state a fold
	// took into it (always a step that changes rows in place).
	size_t boundary = SIZE_MAX, inside = SIZE_MAX, mark = 0;
	if (revision == steps_.front().before_revision) {
		boundary = 0;
	} else {
		const auto step = std::lower_bound(steps_.begin(), steps_.end(), revision,
				[](const Entry &entry, uint64_t r) { return entry.after_revision < r; });
		if (step == steps_.end())
			return false;
		const size_t index = size_t(step - steps_.begin());
		if (step->after_revision == revision) {
			boundary = index + 1;
		} else {
			const auto found = std::lower_bound(step->marks.begin(), step->marks.end(), revision,
					[](const Mark &m, uint64_t r) { return m.revision < r; });
			if (found == step->marks.end() || found->revision != revision)
				return false;
			inside = index;
			mark = size_t(found - step->marks.begin());
		}
	}

	// Each row met on the way: whether it is there at each end, and its version at the start
	// (unknown for a row a fold changed: a state inside a step keeps no versions).
	struct Track {
		bool had = false, has = false, known = true;
		const Node *start = nullptr, *end = nullptr;
	};
	std::map<NodeId, Track> tracks;
	bool state_changed = false;
	const auto walk = [&](const Entry &entry, bool forward) {
		for (const RowSwap &swap : entry.step.swaps) {
			const auto &from = forward ? swap.before : swap.after;
			const auto &to = forward ? swap.after : swap.before;
			const auto placed = tracks.try_emplace(swap.id());
			Track &track = placed.first->second;
			if (placed.second) {
				track.had = bool(from);
				track.start = from.get();
			}
			track.has = bool(to);
			track.end = to.get();
			if (swap.moved && from && to)
				out.reordered = true;
		}
	};
	const auto folded = [&](const Entry &entry, size_t first, size_t last) {
		for (size_t m = first; m <= last && m < entry.marks.size(); ++m) {
			for (const NodeId id : entry.marks[m].rows) {
				const auto placed = tracks.try_emplace(id);
				Track &track = placed.first->second;
				if (placed.second) {
					track.had = track.has = true;
					track.known = false;
				}
			}
			state_changed = state_changed || entry.marks[m].state;
		}
	};
	const auto state_at = [&](size_t at) {
		return at == 0 ? steps_.front().step.before_state : steps_[at - 1].step.after_state;
	};
	if (inside != SIZE_MAX) {
		const Entry &entry = steps_[inside];
		if (cursor_ > inside) {
			folded(entry, mark + 1, entry.marks.size() - 1);
			boundary = inside + 1;
		} else {
			folded(entry, 0, mark);
			boundary = inside;
		}
	}
	if (boundary < cursor_)
		for (size_t i = boundary; i < cursor_; ++i)
			walk(steps_[i], true);
	else
		for (size_t i = boundary; i > cursor_; --i)
			walk(steps_[i - 1], false);
	state_changed = state_changed || state_at(boundary) != state_;

	for (const auto &[id, track] : tracks) {
		if (!track.had && track.has)
			out.added.push_back(id);
		else if (track.had && !track.has)
			out.removed.push_back(id);
		else if (track.had && track.has && (!track.known || track.start != track.end))
			out.changed.push_back(id);
	}
	out.file_state = state_changed;
	return true;
}

} // namespace opennova::editor
