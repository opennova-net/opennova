#pragma once

#include <cstddef>
#include <deque>
#include <iterator>
#include <utility>
#include <vector>

#include <editor/session/view/view_events.h>

namespace opennova::editor {

// The view events one window is sent (ADR 0046 S13 V4): the workspace posts each new event to
// the mailbox of the window it is for (EditorWindows::begin_frame), and the window takes them when
// it draws, so an event waits while its window does not draw (a hidden tab, a closed window) and
// is taken once. The same ask made twice is two events, taken twice. At most ViewEvents::kKept
// wait, as many as the view keeps: one more drops the oldest. `Held` is what the window keeps
// of each: the event, or the event with what the window noted as it came (the Inspector's).
template <class Held = ViewEvent> class ViewEventMailbox {
public:
	void post(Held held) {
		held_.push_back(std::move(held));
		while (held_.size() > ViewEvents::kKept) held_.pop_front();
	}
	// The events held, oldest first, now the owner's: none are held after.
	std::vector<Held> take() {
		std::vector<Held> out(
				std::make_move_iterator(held_.begin()), std::make_move_iterator(held_.end()));
		held_.clear();
		return out;
	}
	size_t held() const { return held_.size(); }

private:
	std::deque<Held> held_;
};

} // namespace opennova::editor
