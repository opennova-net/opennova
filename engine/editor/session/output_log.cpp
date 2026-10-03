#include <editor/session/output_log.h>

#include <utility>

#include <base/io/strutil.h>

namespace opennova::editor {

void OutputLog::append(std::string line) {
	append_folded(std::move(line), {});
}

uint64_t OutputLog::append_folded(std::string line, std::vector<std::string> folded) {
	lines_.push_back(std::move(line));
	Folded held;
	if (folded.size() > kMaxFolded) {
		held.dropped = folded.size() - kMaxFolded;
		folded.resize(kMaxFolded);
	}
	held.lines = std::move(folded);
	folded_.push_back(std::move(held));
	trim();
	++generation_;
	return next_index() - 1;
}

bool OutputLog::fold_into(uint64_t index, std::string line, std::vector<std::string> more) {
	if (index < first_ || index >= next_index()) return false;
	const size_t at = static_cast<size_t>(index - first_);
	lines_[at] = std::move(line);
	Folded &held = folded_[at];
	for (std::string &each : more) {
		if (held.lines.size() < kMaxFolded) held.lines.push_back(std::move(each));
		else ++held.dropped;
	}
	++generation_;
	return true;
}

void OutputLog::trim() {
	while (lines_.size() > kMaxLines) {
		lines_.pop_front();
		folded_.pop_front();
		++first_;
	}
}

void OutputLog::clear() {
	first_ += lines_.size();
	lines_.clear();
	folded_.clear();
	++generation_;
}

bool game_line_matters(const std::string &line) {
	const std::string lower = strutil::to_lower(line);
	for (const char *word : {"error", "warning", "fail", "missing", "unable", "could not", "cannot", "can't", "not found",
	                         "exception", "crash", "assert"})
		if (lower.find(word) != std::string::npos) return true;
	return false;
}

} // namespace opennova::editor
