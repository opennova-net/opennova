#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <editor/session/view/navigation_view.h>

namespace opennova::editor {

// The navigation history (CONTEXT.md "Navigation history"): the places a person was taken from, which
// Back takes them to again, and the places Back took them from, which Forward does, as a browser keeps
// its pages. A move (a document switched to, a Go to, a Problems row, a Find in project hit, a screen of
// a menu, Show in Files) keeps the place it left as Back's nearest and drops what Forward held; a run of
// moves each within kCoalesceMs of the last (a list of hits stepped through, rows clicked through) is
// one step, Back going to where the run began; Back and Forward move places between the two sides and
// keep none of their own. Portable and clocked by its caller, so a test steps the time. The session's
// NavigationController owns the one history of the open project, which goes with it.
class NavigationHistory {
public:
	// The places each side keeps, the farthest dropping past it.
	static constexpr size_t kMost = 50;
	// A move this soon after the last move goes on with that move's run.
	static constexpr int64_t kCoalesceMs = 750;

	// A move from `from` to `to` at `now_ms`: `from` is Back's nearest (unless it is already, or the move
	// goes on with a run that kept where it began) and Forward's places go. Nothing at all for a move from
	// nowhere or to the place it left. True when either side changed.
	bool moved(const NavigationPlace &from, const NavigationPlace &to, int64_t now_ms);
	// Back (`back`) or Forward `steps` places (at least one) from `here`: the place there, taken off its
	// side into `to`; `here` (unless nowhere) and the places stepped over put on the other side, so the
	// other way retraces them. False, nothing changed, when the side holds fewer. The next move begins a
	// run of its own.
	bool step(bool back, size_t steps, const NavigationPlace &here, NavigationPlace &to);
	// The places `gone` says of dropped from both sides (a file the project no longer has); true when any
	// went.
	bool drop(const std::function<bool(const NavigationPlace &)> &gone);
	// The files a rename moved (each from, to): their places shown at the new path, their records found
	// again by their locators (the document is read again there). True when any moved.
	bool follow_moves(const std::vector<std::pair<std::string, std::string>> &moved);
	// Every place gone (the project closes, another opens).
	void clear();

	const std::deque<NavigationPlace> &back() const { return back_; }
	const std::deque<NavigationPlace> &forward() const { return forward_; }

private:
	std::deque<NavigationPlace> back_;    // nearest first
	std::deque<NavigationPlace> forward_; // nearest first
	bool in_run_ = false;                 // a move came, and no step since
	int64_t last_move_ms_ = 0;            // when it came
};

} // namespace opennova::editor
