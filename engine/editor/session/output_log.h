#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::editor {

// What the editor said and what the running game says, oldest first (ADR 0046 d10, S13 A1): the
// Output window's lines and the view JSON's output pages. Past kMaxLines the oldest line drops.
// Every line keeps an absolute index (first_index() counts the lines ever dropped or cleared), so a
// client that pages by an absolute cursor neither skips nor repeats a line while new ones arrive.
//
// A line may hold others folded under it (the UX round's problems lane): an import's files under its
// one summary line, a running game's whole log under its one line, so neither pushes everything else
// out of the log; the Output window opens a line's folded lines with a click and the output query
// pages them (folded_at). A line counts once toward kMaxLines whatever it holds; it holds at most
// kMaxFolded, the rest counted (folded_dropped).
class OutputLog {
public:
	static constexpr size_t kMaxLines = 2000;
	static constexpr size_t kMaxFolded = 20000;

	void append(std::string line);
	// A line with `folded` under it; its absolute index.
	uint64_t append_folded(std::string line, std::vector<std::string> folded);
	// The line at absolute `index` made `line`, `more` folded under it after what it holds (a game's log as
	// it comes); false, nothing done, when the log no longer holds that line (dropped, or cleared).
	bool fold_into(uint64_t index, std::string line, std::vector<std::string> more);
	// Empties the log; the indices go on from where they were.
	void clear();

	bool empty() const { return lines_.empty(); }
	size_t size() const { return lines_.size(); }
	// The i-th line held, oldest first.
	const std::string &operator[](size_t i) const { return lines_[i]; }
	std::deque<std::string>::const_iterator begin() const { return lines_.begin(); }
	std::deque<std::string>::const_iterator end() const { return lines_.end(); }
	// The lines folded under the i-th line held (none for most), and how many more it held past
	// kMaxFolded.
	const std::vector<std::string> &folded(size_t i) const { return folded_[i].lines; }
	size_t folded_dropped(size_t i) const { return folded_[i].dropped; }
	// The absolute index of the oldest line held, and of the next line to come (one past the
	// newest). A cursor below first_index() missed the lines dropped since.
	uint64_t first_index() const { return first_; }
	uint64_t next_index() const { return first_ + lines_.size(); }
	// The line at absolute `index`, first_index() <= index < next_index(), and its folded lines.
	const std::string &at(uint64_t index) const { return lines_[static_cast<size_t>(index - first_)]; }
	const std::vector<std::string> &folded_at(uint64_t index) const { return folded(static_cast<size_t>(index - first_)); }
	// Moves with every change, a line changed in place (fold_into) included.
	uint64_t generation() const { return generation_; }

private:
	struct Folded {
		std::vector<std::string> lines;
		size_t dropped = 0;
	};
	void trim();

	std::deque<std::string> lines_;
	std::deque<Folded> folded_;
	uint64_t first_ = 0;
	uint64_t generation_ = 0;
};

// Whether a line of a running game's log is one Output shows as it comes (the rest folded under the
// game's log line): what reads as an error, a warning, a refusal or a missing file.
bool game_line_matters(const std::string &line);

} // namespace opennova::editor
