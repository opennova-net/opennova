#include "text_history.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace opennova::editor {

namespace {

// The ranges that changed, as a replacement of `old_length` characters at `offset` by `new_length`
// leaves them: a range the replacement meets (or touches) joins its new characters, one after it
// moves with the text, one before it stays.
void track(std::vector<TextRange> &ranges, size_t offset, size_t old_length, size_t new_length) {
	const size_t old_end = offset + old_length;
	const std::ptrdiff_t delta = std::ptrdiff_t(new_length) - std::ptrdiff_t(old_length);
	size_t start = offset, end = offset + new_length;
	std::vector<TextRange> before, after;
	for (const TextRange &range : ranges) {
		const size_t a = range.offset, b = range.offset + range.length;
		if (b < offset) {
			before.push_back(range);
		} else if (a > old_end) {
			after.push_back({size_t(std::ptrdiff_t(a) + delta), range.length});
		} else {
			start = std::min(start, a);
			end = std::max(end, b > old_end ? size_t(std::ptrdiff_t(b) + delta) : offset + new_length);
		}
	}
	ranges = std::move(before);
	ranges.push_back({start, end - start});
	ranges.insert(ranges.end(), after.begin(), after.end());
}

} // namespace

void TextHistory::reset() {
	steps_.clear();
	cursor_ = 0;
	revision_ = saved_revision_ = 0;
	next_revision_ = 1;
	key_.clear();
	bytes_ = 0;
}

TextHistory TextHistory::frozen() const {
	TextHistory out(budget_);
	out.revision_ = revision_;
	out.saved_revision_ = saved_revision_;
	out.next_revision_ = next_revision_;
	return out;
}

size_t TextHistory::bytes_of(const std::vector<TextReplacement> &batch) {
	size_t bytes = sizeof(Batch);
	for (const TextReplacement &replacement : batch)
		bytes += sizeof(TextReplacement) + replacement.removed.size() + replacement.inserted.size();
	return bytes;
}

bool TextHistory::folds(const std::string &key) const {
	// A group never swallows the saved checkpoint: undo must be able to return to exactly what is
	// on disk.
	return !key.empty() && key == key_ && cursor_ && cursor_ == steps_.size() &&
			steps_.back().after_revision() != saved_revision_;
}

void TextHistory::forget_redo() {
	for (size_t i = cursor_; i < steps_.size(); ++i)
		bytes_ -= steps_[i].bytes;
	steps_.erase(steps_.begin() + std::ptrdiff_t(cursor_), steps_.end());
}

void TextHistory::trim() {
	while (bytes_ > budget_.bytes && steps_.size() > budget_.min_steps && cursor_ > 0) {
		bytes_ -= steps_.front().bytes;
		steps_.pop_front();
		--cursor_;
	}
}

void TextHistory::commit(std::vector<TextReplacement> batch, const std::string &key) {
	const bool fold = folds(key);
	forget_redo();
	const uint64_t before = revision_;
	revision_ = next_revision_++;
	// The step's bytes grow by the batch's alone: a typing burst folds in linear time.
	const size_t added = bytes_of(batch);
	if (fold) {
		Step &last = steps_.back();
		last.batches.push_back({std::move(batch), revision_});
		last.bytes += added;
		bytes_ += added;
	} else {
		Step step;
		step.before_revision = before;
		step.batches.push_back({std::move(batch), revision_});
		step.bytes = sizeof(Step) + added;
		bytes_ += step.bytes;
		steps_.push_back(std::move(step));
		++cursor_;
	}
	key_ = key;
	trim();
}

void TextHistory::forward(const Batch &batch, const Apply &apply) {
	for (const TextReplacement &replacement : batch.replacements)
		apply(replacement.offset, replacement.removed.size(), replacement.inserted);
}

void TextHistory::backward(const Batch &batch, const Apply &apply) {
	for (auto it = batch.replacements.rbegin(); it != batch.replacements.rend(); ++it)
		apply(it->offset, it->inserted.size(), it->removed);
}

void TextHistory::undo(const Apply &apply) {
	if (!cursor_) return;
	const Step &step = steps_[--cursor_];
	for (auto it = step.batches.rbegin(); it != step.batches.rend(); ++it)
		backward(*it, apply);
	revision_ = step.before_revision;
	key_.clear();
}

void TextHistory::redo(const Apply &apply) {
	if (cursor_ >= steps_.size()) return;
	const Step &step = steps_[cursor_++];
	for (const Batch &batch : step.batches)
		forward(batch, apply);
	revision_ = step.after_revision();
	key_.clear();
}

bool TextHistory::changes_since(uint64_t revision, std::vector<TextRange> &out) const {
	out.clear();
	if (revision == revision_) return true;
	if (steps_.empty()) return false;
	// Every batch kept, in order: the state `revision` names is after one of them (or before the
	// first); the history is after those of the steps before the cursor.
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
	const auto replace = [&out](size_t offset, size_t count, const std::string &with) {
		track(out, offset, count, with.size());
	};
	if (from < at)
		for (size_t i = from; i < at; ++i)
			forward(*batches[i], replace);
	else
		for (size_t i = from; i > at; --i)
			backward(*batches[i - 1], replace);
	return true;
}

} // namespace opennova::editor
