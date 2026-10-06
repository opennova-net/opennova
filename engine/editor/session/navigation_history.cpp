#include <editor/session/navigation_history.h>

#include <algorithm>

namespace opennova::editor {

namespace {

// `place` made the nearest of `side`, the farthest past kMost dropping.
void keep_nearest(std::deque<NavigationPlace> &side, NavigationPlace place) {
	side.push_front(std::move(place));
	while (side.size() > NavigationHistory::kMost) side.pop_back();
}

} // namespace

bool NavigationHistory::moved(const NavigationPlace &from, const NavigationPlace &to, int64_t now_ms) {
	if (from.nowhere() || same_place(from, to)) return false;
	const bool changed = !forward_.empty();
	forward_.clear();
	// A move soon after the last one goes on with its run: Back keeps where the run began.
	const bool run = in_run_ && now_ms - last_move_ms_ < kCoalesceMs && !back_.empty();
	in_run_ = true;
	last_move_ms_ = now_ms;
	if (run || (!back_.empty() && same_place(back_.front(), from))) return changed;
	keep_nearest(back_, from);
	return true;
}

bool NavigationHistory::step(bool back, size_t steps, const NavigationPlace &here, NavigationPlace &to) {
	std::deque<NavigationPlace> &from_side = back ? back_ : forward_;
	std::deque<NavigationPlace> &to_side = back ? forward_ : back_;
	steps = std::max<size_t>(steps, 1);
	if (from_side.size() < steps) return false;
	// Here first, then each place stepped over, nearest last: the other way retraces them in order.
	if (!here.nowhere()) keep_nearest(to_side, here);
	for (size_t i = 0; i + 1 < steps; ++i) keep_nearest(to_side, from_side[i]);
	to = from_side[steps - 1];
	from_side.erase(from_side.begin(), from_side.begin() + std::ptrdiff_t(steps));
	in_run_ = false;
	return true;
}

bool NavigationHistory::drop(const std::function<bool(const NavigationPlace &)> &gone) {
	const size_t before = back_.size() + forward_.size();
	back_.erase(std::remove_if(back_.begin(), back_.end(), gone), back_.end());
	forward_.erase(std::remove_if(forward_.begin(), forward_.end(), gone), forward_.end());
	return back_.size() + forward_.size() != before;
}

bool NavigationHistory::follow_moves(const std::vector<std::pair<std::string, std::string>> &moved) {
	bool any = false;
	for (std::deque<NavigationPlace> *side : {&back_, &forward_})
		for (NavigationPlace &place : *side)
			for (const auto &[from, to] : moved) {
				if (place.path != from) continue;
				place.path = to;
				place.document = 0; // read again at its new path: its record by its locator
				any = true;
				break;
			}
	return any;
}

void NavigationHistory::clear() {
	back_.clear();
	forward_.clear();
	in_run_ = false;
	last_move_ms_ = 0;
}

} // namespace opennova::editor
