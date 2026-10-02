#include <editor/session/view/view_events.h>

#include <utility>

namespace opennova::editor {

uint64_t ViewEvents::post(ViewEvent event) {
	event.seq = next_++;
	events_.push_back(std::move(event));
	while (events_.size() > kKept) events_.pop_front();
	return events_.back().seq;
}

} // namespace opennova::editor
