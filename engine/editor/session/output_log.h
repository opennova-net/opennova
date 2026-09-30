#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace opennova::editor {

// What the editor said and what the running game says, oldest first (ADR 0046 d10, S13 A1): the
// Output window's lines and the view JSON's output pages. Past kMaxLines the oldest line drops.
// Every line keeps an absolute index (first_index() counts the lines ever dropped or cleared), so a
// client that pages by an absolute cursor neither skips nor repeats a line while new ones arrive.
class OutputLog {
public:
	static constexpr size_t kMaxLines = 2000;

	void append(std::string line);
	// Empties the log; the indices go on from where they were.
	void clear();

	bool empty() const { return lines_.empty(); }
	size_t size() const { return lines_.size(); }
	// The i-th line held, oldest first.
	const std::string &operator[](size_t i) const { return lines_[i]; }
	std::deque<std::string>::const_iterator begin() const { return lines_.begin(); }
	std::deque<std::string>::const_iterator end() const { return lines_.end(); }
	// The absolute index of the oldest line held, and of the next line to come (one past the
	// newest). A cursor below first_index() missed the lines dropped since.
	uint64_t first_index() const { return first_; }
	uint64_t next_index() const { return first_ + lines_.size(); }
	// The line at absolute `index`, first_index() <= index < next_index().
	const std::string &at(uint64_t index) const { return lines_[static_cast<size_t>(index - first_)]; }

private:
	std::deque<std::string> lines_;
	uint64_t first_ = 0;
};

} // namespace opennova::editor
