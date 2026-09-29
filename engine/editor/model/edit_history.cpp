#include "edit_history.h"

#include <algorithm>

namespace opennova::editor {

void EditHistory::reset() {
	history_.clear();
	cursor_ = 0;
	revision_ = saved_revision_ = 0;
	next_revision_ = 1;
	coalesce_key_.clear();
}

void apply_change(std::vector<std::shared_ptr<const Node>> &rows, const Change &c, bool forward) {
	const auto &old = forward ? c.before : c.after;
	const auto &replacement = forward ? c.after : c.before;
	if (old) {
		for (size_t index = 0; index < rows.size(); ++index) {
			if (rows[index]->id != old->id) continue;
			rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(index));
			break;
		}
	}
	if (replacement) {
		const size_t position = std::min(forward ? c.after_position : c.before_position, rows.size());
		rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(position), replacement);
	}
}

void EditHistory::restore(const Change &c, bool forward) {
	apply_change(rows_, c, forward);
	state_ = forward ? c.after_state : c.before_state;
	revision_ = forward ? c.after_revision : c.before_revision;
}

bool EditHistory::open(const std::string &key) const {
	// A coalesced step never swallows the saved checkpoint: undo must be able to
	// return to exactly what is on disk.
	return !key.empty() && key == coalesce_key_ && cursor_ && cursor_ == history_.size() &&
	       history_.back().after_revision != saved_revision_;
}

bool EditHistory::folds(const std::string &key) const {
	// The last change of a step over several rows is another row's: nothing folds into it.
	return open(key) && !history_.back().joined;
}

void EditHistory::commit(Change c, const std::string &key) {
	const bool fold = folds(key);
	c.before_revision = revision_;
	c.after_revision = next_revision_++;
	if (cursor_ < history_.size()) history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
	restore(c, true);
	if (fold) {
		history_.back().after = c.after;
		history_.back().after_revision = c.after_revision;
		history_.back().after_state = c.after_state;
	} else {
		history_.push_back(std::move(c));
		++cursor_;
	}
	coalesce_key_ = key;
}

void EditHistory::commit(std::vector<Change> changes, const std::string &key) {
	if (changes.empty()) return;
	if (changes.size() == 1) return commit(std::move(changes.front()), key);
	if (cursor_ < history_.size()) history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
	for (size_t i = 0; i < changes.size(); ++i) {
		Change &c = changes[i];
		c.joined = i > 0;
		c.before_revision = revision_;
		c.after_revision = next_revision_++;
		restore(c, true);
		history_.push_back(std::move(c));
		++cursor_;
	}
	coalesce_key_ = key;
}

bool EditHistory::reopen(const std::string &key) {
	if (!open(key)) return false;
	step_back();
	return true;
}

void EditHistory::resume() {
	if (cursor_ < history_.size()) step_forward();
}

void EditHistory::drop() {
	history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
	coalesce_key_.clear();
}

void EditHistory::step_back() {
	do {
		restore(history_[--cursor_], false);
	} while (cursor_ && history_[cursor_].joined);
}

void EditHistory::step_forward() {
	do {
		restore(history_[cursor_++], true);
	} while (cursor_ < history_.size() && history_[cursor_].joined);
}

void EditHistory::undo() {
	if (!cursor_) return;
	step_back();
	end_edit_group();
}

void EditHistory::redo() {
	if (cursor_ >= history_.size()) return;
	step_forward();
	end_edit_group();
}

} // namespace opennova::editor
