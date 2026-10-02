#include <editor/session/output_log.h>

#include <utility>

namespace opennova::editor {

void OutputLog::append(std::string line) {
	lines_.push_back(std::move(line));
	while (lines_.size() > kMaxLines) {
		lines_.pop_front();
		++first_;
	}
}

void OutputLog::clear() {
	first_ += lines_.size();
	lines_.clear();
}

} // namespace opennova::editor
