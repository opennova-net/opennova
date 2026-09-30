#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include <editor/session/view/view_events.h>

namespace opennova::editor {

// The view events one window is sent (ADR 0046 S13 V4): the workspace posts each new event to
// the mailbox of the window it is for (EditorWindows::begin_frame), and the window takes them when
// it draws, so an event waits while its window does not draw (a hidden tab, a closed window) and
// is taken once. The same ask made twice is two events, taken twice.
class ViewEventMailbox {
public:
	void post(const ViewEvent &event) { held_.push_back(event); }
	// The events held, oldest first, now the owner's: none are held after.
	std::vector<ViewEvent> take() { return std::exchange(held_, {}); }
	size_t held() const { return held_.size(); }

private:
	std::vector<ViewEvent> held_;
};

} // namespace opennova::editor
